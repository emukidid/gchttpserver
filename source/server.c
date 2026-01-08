// server.c
//
// Minimal GameCube HTTP file server using libogc2 + LWP + a single-file HTTP parser.
// - Uses picohttpparser.h (MIT) for HTTP parsing (https://github.com/h2o/picohttpparser).
// - Serves files from the /www directory to the web root.
// - LRU cache with fixed total size from a malloc'ed arena.
// - Large file streaming support
// - /status and /storage generated pages
// - Simple per-connection thread model via LWP.

#include <gccore.h>
#include <network.h>
#include <ogc/lwp.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <dirent.h>
#include <ogc/timesupp.h>
#include "picohttpparser.h"

// Config
#define SERVER_PORT       8080
#define MAX_HEADER_SIZE   1024
#define MAX_REQUEST_SIZE  2048
#define MAX_PATH_LEN      256
#define MAX_THREADS       8

// Cache settings
#define CACHE_SIZE_BYTES  (1 * 1024 * 1024)
#define MAX_CACHE_ENTRIES 128
enum {
	CACHE_ERROR = 0,	// not found, error
	CACHE_HIT,	// cached hit
	CACHE_LOADED, //loaded but NOT cached (caller must free)
	CACHE_STREAM	// too big, stream instead (caller must stream)
};

#define STREAMING_THRESHOLD (512 * 1024)
#define STREAM_CHUNK_SIZE   (32 * 1024)

static mutex_t fs_mutex;
static u64 server_start_ticks = 0;
static mutex_t stats_mutex;
static u64 g_total_bytes_served = 0;

#define PAGE_SIZE 100   // entries per page

// LWP types
typedef struct client_ctx_s {
    int sock;
    struct sockaddr_in addr;
} client_ctx_t;

static lwp_t g_threads[MAX_THREADS];
static int   g_thread_used[MAX_THREADS];

// LRU cache
typedef struct cache_entry_s {
    struct cache_entry_s *prev;
    struct cache_entry_s *next;
    char   path[MAX_PATH_LEN]; // cache key: normalized path
    char  *data;               // pointer into arena
    size_t size;
} cache_entry_t;

typedef struct cache_s {
    char          *arena;
    size_t         arena_size;
    size_t         used;

    cache_entry_t  entries[MAX_CACHE_ENTRIES];
    cache_entry_t *lru_head;   // most recently used
    cache_entry_t *lru_tail;   // least recently used
} cache_t;

static cache_t g_cache;

// Initialize cache with a single malloc'ed arena
static void cache_init(size_t size_bytes)
{
    memset(&g_cache, 0, sizeof(g_cache));
    g_cache.arena = (char *)malloc(size_bytes);
    g_cache.arena_size = size_bytes;
    g_cache.used = 0;
	printf("[CACHE] Initialized arena: %u bytes at %08X\n", (unsigned)size_bytes, (u32)g_cache.arena);
}

// Move entry to front of LRU list (most recently used)
static void cache_lru_move_to_front(cache_entry_t *e)
{
    if (g_cache.lru_head == e) return;

    // detach
    if (e->prev) e->prev->next = e->next;
    if (e->next) e->next->prev = e->prev;
    if (g_cache.lru_tail == e) g_cache.lru_tail = e->prev;

    // insert at head
    e->prev = NULL;
    e->next = g_cache.lru_head;
    if (g_cache.lru_head) g_cache.lru_head->prev = e;
    g_cache.lru_head = e;
    if (!g_cache.lru_tail) g_cache.lru_tail = e;
}

// Remove tail entry from LRU list (for eviction)
// Note: doesn't reclaim arena offset, just marks entry free.
static void cache_evict_one(void)
{
    cache_entry_t *e = g_cache.lru_tail;
    if (!e) return;

    // detach
    if (e->prev) e->prev->next = NULL;
    g_cache.lru_tail = e->prev;
    if (!g_cache.lru_tail) g_cache.lru_head = NULL;

    printf("[CACHE] Evicting: %s (%u bytes)\n",
           e->path, (unsigned)e->size);
    // mark unused (simple: size=0, path empty)
    e->data = NULL;
    e->size = 0;
    e->path[0] = '\0';
    e->prev = e->next = NULL;
    // Arena memory is not reclaimed; we just leak into arena.
}

// Find existing cache entry by path
static cache_entry_t *cache_find(const char *path)
{
    for (int i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (g_cache.entries[i].size == 0) continue;
        if (strcmp(g_cache.entries[i].path, path) == 0) {
            return &g_cache.entries[i];
        }
    }
    return NULL;
}

// Create a new entry for path with given size, returns pointer to writable buffer
static cache_entry_t *cache_create(const char *path, size_t size)
{
    if (size > (g_cache.arena_size - g_cache.used)) {
        // Not enough arena; evict until there is (or until we give up)
        int guard = MAX_CACHE_ENTRIES * 2;
        while (size > (g_cache.arena_size - g_cache.used) && guard-- > 0) {
            if (!g_cache.lru_tail) break;
            cache_evict_one();
            //  don't reclaim arena.used so eviction does not actually free bytes.
        }
        if (size > (g_cache.arena_size - g_cache.used)) {
            return NULL; // still not enough, skip caching
        }
    }

    // Find free entry
    cache_entry_t *slot = NULL;
    for (int i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (g_cache.entries[i].size == 0 && g_cache.entries[i].path[0] == '\0') {
            slot = &g_cache.entries[i];
            break;
        }
    }
    if (!slot) {
        // evict one and reuse
        if (!g_cache.lru_tail) return NULL;
        slot = g_cache.lru_tail;
        cache_evict_one();
        // slot is marked free now
    }

    // assign from arena
    slot->data = g_cache.arena + g_cache.used;
    slot->size = size;
    strncpy(slot->path, path, MAX_PATH_LEN - 1);
    slot->path[MAX_PATH_LEN - 1] = '\0';

    g_cache.used += size;

    // insert into LRU as MRU
    slot->prev = slot->next = NULL;
    cache_lru_move_to_front(slot);

    return slot;
}

// Lookup or load file into cache
static int cache_get_file(const char *doc_root, const char *rel_path,
                          const char **out_data, size_t *out_size)
{
    char full_path[512];
    snprintf(full_path, sizeof(full_path), "%s/%s", doc_root, rel_path);

    // First stat the file to get its size
    LWP_MutexLock(fs_mutex);
	printf("About to fopen [%s]\n" ,full_path);
	FILE *f = fopen(full_path, "rb");
	if (!f) {
		LWP_MutexUnlock(fs_mutex);
		return CACHE_ERROR;
	}

	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	printf("Got size [%ld]\n" ,sz);

	if (sz < 0) {
		fclose(f);
		LWP_MutexUnlock(fs_mutex);
		return CACHE_ERROR;
	}

	// If file is large, caller must stream it
	if ((size_t)sz > STREAMING_THRESHOLD) {
		fclose(f);
		LWP_MutexUnlock(fs_mutex);
		*out_size = (size_t)sz;
		return CACHE_STREAM;   // STREAM
	}

	// Check cache for small files
	cache_entry_t *e = cache_find(full_path);
	if (e) {
		cache_lru_move_to_front(e);
		*out_data = e->data;
		*out_size = e->size;
		fclose(f);
		LWP_MutexUnlock(fs_mutex);
		return CACHE_HIT;
	}

	// Load small file into cache or temp buffer
	cache_entry_t *new_e = cache_create(full_path, (size_t)sz);
	if (!new_e) {
		char *buf = (char*)malloc(sz);
		if (!buf) { fclose(f); LWP_MutexUnlock(fs_mutex); return 0; }
		if (fread(buf, 1, sz, f) != (size_t)sz) {
			fclose(f);
			free(buf);
			LWP_MutexUnlock(fs_mutex);
			return CACHE_ERROR;
		}
		fclose(f);
		LWP_MutexUnlock(fs_mutex);
		*out_data = buf;
		*out_size = (size_t)sz;
		return CACHE_LOADED;
	}

	// Read into cache arena
	printf("fread size %ld to %08X\n", sz, (u32)new_e->data);
	if (fread(new_e->data, 1, sz, f) != (size_t)sz) {
		fclose(f);
		new_e->size = 0;
		new_e->path[0] = '\0';
		LWP_MutexUnlock(fs_mutex);
		return CACHE_ERROR;
	}
	fclose(f);
	LWP_MutexUnlock(fs_mutex);

	*out_data = new_e->data;
	*out_size = (size_t)sz;
	return CACHE_HIT;
}

// ----------------------------- HTTP helpers ---------------------------
static const char *guess_content_type(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    ext++;

    if (!strcmp(ext, "html") || !strcmp(ext, "htm")) return "text/html";
    if (!strcmp(ext, "css"))  return "text/css";
    if (!strcmp(ext, "js"))   return "application/javascript";
    if (!strcmp(ext, "png"))  return "image/png";
    if (!strcmp(ext, "jpg") || !strcmp(ext, "jpeg")) return "image/jpeg";
    if (!strcmp(ext, "gif"))  return "image/gif";
    if (!strcmp(ext, "txt"))  return "text/plain";

    return "application/octet-stream";
}

static void split_path_query(char *req_path, char **path_out, char **query_out)
{
    char *q = strchr(req_path, '?');
    if (q) {
        *q = '\0';
        *query_out = q + 1;
    } else {
        *query_out = NULL;
    }
    *path_out = req_path;
}

static void url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t di = 0;

    for (size_t i = 0; src[i] && di + 4 < dst_size; i++) {
        unsigned char c = src[i];
        if (isalnum(c) || c=='-' || c=='_' || c=='.' || c=='~') {
            dst[di++] = c;
        } else {
            dst[di++] = '%';
            dst[di++] = hex[c >> 4];
            dst[di++] = hex[c & 0xF];
        }
    }
    dst[di] = '\0';
}

// Decode %xx URL encoding in-place.
// Returns length of decoded string.
static size_t url_decode(char *str)
{
    char *src = str;
    char *dst = str;

    while (*src) {
        if (*src == '%' &&
            src[1] && src[2] &&
            isxdigit((unsigned char)src[1]) &&
            isxdigit((unsigned char)src[2])) {

            char hex[3] = { src[1], src[2], 0 };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        }
        else if (*src == '+') {
            *dst++ = ' ';
            src++;
        }
        else {
            *dst++ = *src++;
        }
    }

    *dst = '\0';
    return dst - str;
}

// Normalize request path into safe relative path (no .., no leading /)
static void normalize_path(const char *req_path, char *out, size_t out_size)
{
    // req_path starts with '/', e.g. "/index.html"
    if (strcmp(req_path, "/") == 0) {
        snprintf(out, out_size, "index.html");
        return;
    }

    // skip leading '/'
    const char *p = req_path;
    if (*p == '/') p++;

    // copy and strip any ".."
    char buf[MAX_PATH_LEN];
    size_t len = 0;
    while (*p && len < sizeof(buf) - 1) {
        if (p[0] == '.' && p[1] == '.') {
            p += 2;
            continue;
        }
        buf[len++] = *p++;
    }
    buf[len] = '\0';
	
	url_decode(buf);

    snprintf(out, out_size, "%s", buf);
}

static int send_all(int sock, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int ret = net_send(sock, buf + sent, len - sent, 0);
        if (ret <= 0) return -1;
        sent += ret;
    }
    return 0;
}

// simple HTTP response
static void send_response(int sock, int status, const char *status_text,
                          const char *content_type,
                          const char *body, size_t body_len)
{
    char header[512];
    int n = snprintf(header, sizeof(header),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Length: %u\r\n"
                     "Content-Type: %s\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     status, status_text, (unsigned)body_len, content_type);
    if (n < 0) return;

    send_all(sock, header, (size_t)n);
    if (body && body_len > 0) {
        send_all(sock, body, body_len);
		LWP_MutexLock(stats_mutex);
		g_total_bytes_served += body_len;
		LWP_MutexUnlock(stats_mutex);
    }
}

static int get_page_number(const char *url_path)
{
    const char *q = strchr(url_path, '?');
    if (!q) return 1;

    if (strncmp(q, "?page=", 6) == 0) {
        int p = atoi(q + 6);
        return p > 0 ? p : 1;
    }

    return 1;
}

static void send_directory_listing(int sock,
                                   const char *fs_path,
                                   const char *clean_url,
                                   const char *display_url)
{
    int page = get_page_number(display_url);
    int start_index = (page - 1) * PAGE_SIZE;
    int end_index = start_index + PAGE_SIZE;

    LWP_MutexLock(fs_mutex);
    DIR *d = opendir(fs_path);
    LWP_MutexUnlock(fs_mutex);

    if (!d) {
        const char *msg = "Directory not found";
        send_response(sock, 404, "Not Found", "text/plain", msg, strlen(msg));
        return;
    }

    // First pass: count entries
    int total_entries = 0;
    struct dirent *ent;

    while (1) {
        LWP_MutexLock(fs_mutex);
        ent = readdir(d);
        LWP_MutexUnlock(fs_mutex);

        if (!ent) break;
        if (!strcmp(ent->d_name, ".")) continue;
        total_entries++;
    }

    // Rewind for second pass
    LWP_MutexLock(fs_mutex);
    rewinddir(d);
    LWP_MutexUnlock(fs_mutex);

    char pagebuf[65536];
    int n = snprintf(pagebuf, sizeof(pagebuf),
		"<html><body>"
		"<div style='display:flex;align-items:center;gap:12px;'>"
			"<h1 style='margin:0;'>Index of %s</h1>"
		"</div>"
		"[ <a href=\"/storage\">Home</a> | <a href=\"/status\">Status</a> ]"
		"<p>Page %d of %d</p>"
		"<ul>",
		display_url,
		page,
		(total_entries + PAGE_SIZE - 1) / PAGE_SIZE
	);

    int index = 0;

    while (1) {
        LWP_MutexLock(fs_mutex);
        ent = readdir(d);
        LWP_MutexUnlock(fs_mutex);

        if (!ent) break;
        if (!strcmp(ent->d_name, ".")) continue;

        if (index >= start_index && index < end_index) {
            char encoded[512];
            url_encode(ent->d_name, encoded, sizeof(encoded));

            char full_url[768];
            snprintf(full_url, sizeof(full_url), "%s%s", clean_url, encoded);

            if (ent->d_type == DT_DIR) {
                n += snprintf(pagebuf + n, sizeof(pagebuf) - n,
                    "<li><b><a href=\"%s/\">%s/</a></b></li>",
                    full_url, ent->d_name);
            } else {
                n += snprintf(pagebuf + n, sizeof(pagebuf) - n,
                    "<li><a href=\"%s\">%s</a></li>",
                    full_url, ent->d_name);
            }
        }

        index++;
    }

    // Pagination controls
    int total_pages = (total_entries + PAGE_SIZE - 1) / PAGE_SIZE;

    n += snprintf(pagebuf + n, sizeof(pagebuf) - n, "</ul><p>");

    if (page > 1) {
        n += snprintf(pagebuf + n, sizeof(pagebuf) - n,
            "<a href=\"%s?page=%d\">Prev</a> ",
            clean_url, page - 1);
    }

    if (page < total_pages) {
        n += snprintf(pagebuf + n, sizeof(pagebuf) - n,
            "<a href=\"%s?page=%d\">Next</a>", clean_url, page+1);
    }

    snprintf(pagebuf + n, sizeof(pagebuf) - n, "</p></body></html>");

    LWP_MutexLock(fs_mutex);
    closedir(d);
    LWP_MutexUnlock(fs_mutex);

    send_response(sock, 200, "OK", "text/html", pagebuf, strlen(pagebuf));
}

static void stream_file(int sock, const char *full_path, size_t size, const char *content_type)
{
    LWP_MutexLock(fs_mutex);

	FILE *f = fopen(full_path, "rb");
	if (!f) {
		LWP_MutexUnlock(fs_mutex);
		const char *msg = "Not Found";
		send_response(sock, 404, "Not Found", "text/plain", msg, strlen(msg));
		return;
	}

	// We already know size; no need to seek here
	LWP_MutexUnlock(fs_mutex);

	// Send headers first (no FS access)
	char header[512];
	int n = snprintf(header, sizeof(header),
					 "HTTP/1.1 200 OK\r\n"
					 "Content-Length: %u\r\n"
					 "Content-Type: %s\r\n"
					 "Connection: close\r\n"
					 "\r\n",
					 (unsigned)size, content_type);
	send_all(sock, header, n);

	char *chunk = malloc(STREAM_CHUNK_SIZE);
	if (!chunk) {
		LWP_MutexLock(fs_mutex);
		fclose(f);
		LWP_MutexUnlock(fs_mutex);
		return;
	}

	size_t remaining = size;
	while (remaining > 0) {
		size_t to_read = remaining > STREAM_CHUNK_SIZE ? STREAM_CHUNK_SIZE : remaining;

		LWP_MutexLock(fs_mutex);
		size_t r = fread(chunk, 1, to_read, f);
		LWP_MutexUnlock(fs_mutex);

		if (r == 0) break;
		if (send_all(sock, chunk, r) < 0) break;
		LWP_MutexLock(stats_mutex);
		g_total_bytes_served += r;
		LWP_MutexUnlock(stats_mutex);
		remaining -= r;
	}
	free(chunk);

	LWP_MutexLock(fs_mutex);
	fclose(f);
	LWP_MutexUnlock(fs_mutex);
}

static void format_bytes(u64 bytes, char *out, size_t out_size)
{
    if (bytes < 1024) {
        snprintf(out, out_size, "%llu B", bytes);
    } else if (bytes < 1024ULL * 1024ULL) {
        snprintf(out, out_size, "%.2f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_size, "%.2f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static void send_status_page(int sock)
{
    char page[8192];
    char data_served_str[64];

	LWP_MutexLock(stats_mutex);
	u64 served = g_total_bytes_served;
	LWP_MutexUnlock(stats_mutex);

	format_bytes(served, data_served_str, sizeof(data_served_str));


    int used_entries = 0;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (g_cache.entries[i].size > 0)
            used_entries++;
    }

    int active_threads = 0;
    for (int i = 0; i < MAX_THREADS; i++) {
        if (g_thread_used[i])
            active_threads++;
    }

    float used_kb = (float)g_cache.used / 1024.0f;
    float total_kb = (float)g_cache.arena_size / 1024.0f;

    u64 now = gettime();
    u32 uptime_sec = diff_sec(server_start_ticks, now);

    int n = snprintf(page, sizeof(page),
        "<html><body>"
        "<h1>GameCube HTTP Server Status</h1>"
        "<p><b>Uptime:</b> %u seconds</p>"
        "<p><b>Active threads:</b> %d / %d</p>"
        "<p><b>Cache usage:</b> %.1f KB / %.1f KB</p>"
        "<p><b>Cached entries:</b> %d</p>"
        "<p><b>Total data served:</b> %s</p>"
        "<h2>Thread Slots</h2><ul>",
        uptime_sec,
        active_threads, MAX_THREADS,
        used_kb, total_kb,
        used_entries,
        data_served_str
    );

    for (int i = 0; i < MAX_THREADS; i++) {
        n += snprintf(page + n, sizeof(page) - n,
            "<li>Slot %d: %s</li>",
            i, g_thread_used[i] ? "ACTIVE" : "free"
        );
    }

    n += snprintf(page + n, sizeof(page) - n,
		"</ul>"
		"<h2>Help / Usage</h2>"
		"<p><b>/www/</b> is mapped to the web root. Any files placed in the SD:/www directory "
		"can be accessed directly via the browser (e.g., <code>/index.html</code>).</p>"
		"<p><a href=\"/status\"><b>/status</b></a> shows this status page with server statistics.</p>"
		"<p><a href=\"/storage\"><b>/storage</b></a> provides a live browser of the SD card's filesystem. "
		"Directories and files under SD:/ are accessible via <code>/storage/&lt;path&gt;</code>. "
		"Large directories are automatically paged.</p>"
		"</body></html>"
	);

    send_response(sock, 200, "OK", "text/html", page, strlen(page));
}

// Handle a single HTTP request from a connected socket
static void handle_http_request(int sock, const char *doc_root)
{
	printf("[HTTP] Receiving request...\n");
    char buf[MAX_REQUEST_SIZE];
    int r = net_recv(sock, buf, sizeof(buf), 0);
    if (r <= 0) return;

    const char *method;
	size_t method_len;

	const char *path;
	size_t path_len;

	int minor_version;

	struct phr_header headers[16];
	size_t num_headers = sizeof(headers) / sizeof(headers[0]);

	int pret = phr_parse_request(
		buf, r,
		&method, &method_len,
		&path, &path_len,
		&minor_version,
		headers, &num_headers,
		0
	);
	if (pret <= 0) {
        printf("[HTTP] ERROR: Bad request (pret=%d)\n", pret);
        const char *msg = "Bad Request";
        send_response(sock, 400, "Bad Request",
                      "text/plain", msg, strlen(msg));
        return;
    }

    // we only support GET
    if (strncmp(method, "GET", 3) != 0) {
        const char *msg = "Method Not Allowed";
        send_response(sock, 405, "Method Not Allowed",
                      "text/plain", msg, strlen(msg));
        return;
    }

    char req_path[MAX_PATH_LEN];
    if ((size_t)(path - buf) + 1 >= sizeof(req_path)) {
        const char *msg = "Request-URI Too Long";
        send_response(sock, 414, "URI Too Long",
                      "text/plain", msg, strlen(msg));
        return;
    }
	printf("[HTTP] Method: %.*s\n", (int)method_len, method);
    printf("[HTTP] Raw path: %.*s\n", (int)path_len, path);

    // Copy only the actual URL path (no headers)
	snprintf(req_path, sizeof(req_path), "%.*s", (int)path_len, path);

	char *path_only;
	char *query;
	split_path_query(req_path, &path_only, &query);
    // Remove any trailing spaces
	char *space = strchr(req_path, ' ');
	if (space) *space = '\0';


    char rel_path[MAX_PATH_LEN];
    normalize_path(path_only, rel_path, sizeof(rel_path));
    printf("[HTTP] Normalized path: [%s]\n", rel_path);

	if (strncmp(rel_path, "storage", 7) == 0) {

		// Strip "storage" prefix
		const char *sub = rel_path + 7;
		if (*sub == '/') sub++;

		// Build real filesystem path
		char fs_path[512];
		snprintf(fs_path, sizeof(fs_path), "/%s", sub);

		// If ends with / or is empty → directory listing
		if (sub[0] == '\0' || rel_path[strlen(rel_path)-1] == '/') {
			// Clean path prefix (NO query)
			char clean_url[512];
			snprintf(clean_url, sizeof(clean_url), "/storage/%s", sub);

			// Full URL (WITH query) for display only
			char display_url[768];
			if (query)
				snprintf(display_url, sizeof(display_url), "%s?%s", clean_url, query);
			else
				snprintf(display_url, sizeof(display_url), "%s", clean_url);

			send_directory_listing(sock, fs_path, clean_url, display_url);
			return;
		}

		// Otherwise treat as file download
		const char *data = NULL;
		size_t size = 0;

		int cache_state = cache_get_file("/", fs_path + 1, &data, &size);

		if (cache_state == 0) {
			printf("[HTTP] 404 Not Found: %s\n", fs_path);
			const char *msg = "Not Found";
			send_response(sock, 404, "Not Found", "text/plain", msg, strlen(msg));
			return;
		}

		const char *ct = guess_content_type(fs_path);

		if (cache_state == 3) {
			printf("[HTTP] Streaming large file: %s (%u bytes)\n", fs_path, (unsigned)size);
			stream_file(sock, fs_path, size, ct);
			return;
		}

		printf("[HTTP] Sending file %s (%u bytes)\n", fs_path, (unsigned)size);
		send_response(sock, 200, "OK", ct, data, size);

		if (cache_state == 2)
			free((void*)data);

		return;
	}
	
	// Status
	if (strcmp(rel_path, "status") == 0 ||
		strcmp(rel_path, "status.html") == 0)
	{
		printf("[HTTP] Serving status page\n");
		send_status_page(sock);
		return;
	}
    const char *data = NULL;
    size_t size = 0;
    int cache_state = cache_get_file(doc_root, rel_path, &data, &size);

	if (cache_state == 0) {
		printf("[HTTP] 404 Not Found: %s\n", rel_path);
		const char *msg = "Not Found";
		send_response(sock, 404, "Not Found", "text/plain", msg, strlen(msg));
		return;
	}

	const char *ct = guess_content_type(rel_path);

	// STREAMING MODE
	if (cache_state == 3) {
		printf("[HTTP] Streaming large file: %s (%u bytes)\n", rel_path, (unsigned)size);

		char full_path[512];
		snprintf(full_path, sizeof(full_path), "%s/%s", doc_root, rel_path);

		stream_file(sock, full_path, size, ct);
		return;
	}

	// NORMAL MODE (cached or small uncached)
	if (cache_state == 1)
		printf("[HTTP] Cache HIT: %s (%u bytes)\n", rel_path, (unsigned)size);
	else if (cache_state == 2)
		printf("[HTTP] Cache MISS (loaded uncached): %s (%u bytes)\n", rel_path, (unsigned)size);

	printf("[HTTP] Sending 200 OK (%u bytes)\n", (unsigned)size);
	send_response(sock, 200, "OK", ct, data, size);

	if (cache_state == 2)
		free((void*)data);
}

// ----------------------------- Thread entry ---------------------------

static void *client_thread_func(void *arg)
{
    client_ctx_t *ctx = (client_ctx_t *)arg;
    printf("[HTTP] Thread %u handling request\n", (unsigned)LWP_GetSelf());

    const char *doc_root = "/www";
    handle_http_request(ctx->sock, doc_root);
    net_close(ctx->sock);

    free(ctx);

    // Mark this LWP slot as free
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (g_threads[i] == LWP_GetSelf()) {
            g_thread_used[i] = 0;
            break;
        }
    }
	printf("[HTTP] Thread %u finished\n", (unsigned)LWP_GetSelf());
    return NULL;
}

static int find_free_thread_slot(void)
{
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (!g_thread_used[i]) return i;
    }
    return -1;
}

void start_gc_http_server(void)
{
	server_start_ticks = gettime();
	LWP_MutexInit(&fs_mutex, 0);
	LWP_MutexInit(&stats_mutex, 0);
    printf("[HTTP] Initializing cache (%u bytes)\n", CACHE_SIZE_BYTES);
    cache_init(CACHE_SIZE_BYTES);

    printf("[HTTP] Creating socket...\n");
    int sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        printf("[HTTP] ERROR: net_socket failed\n");
        return;
    }

    printf("[HTTP] Binding to port %d...\n", SERVER_PORT);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (net_bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("[HTTP] ERROR: net_bind failed\n");
        net_close(sock);
        return;
    }
    printf("[HTTP] Bind OK\n");

	if (net_listen(sock, 4) < 0) {
        printf("[HTTP] ERROR: net_listen failed\n");
        net_close(sock);
        return;
    }
    printf("[HTTP] Listening on port %d\n", SERVER_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        u32 addr_len = sizeof(client_addr);
		int csock = net_accept(sock, (struct sockaddr*)&client_addr, &addr_len);
        if (csock < 0) {
            printf("[HTTP] WARNING: net_accept returned <0\n");
            continue;
        }
		int timeout_ms = 5000;
		net_setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &timeout_ms, sizeof(timeout_ms));

        printf("[HTTP] Connection from %u.%u.%u.%u\n",
            (client_addr.sin_addr.s_addr >> 24) & 0xFF,
			(client_addr.sin_addr.s_addr >> 16) & 0xFF,
			(client_addr.sin_addr.s_addr >> 8) & 0xFF,
			client_addr.sin_addr.s_addr & 0xFF
            );

        int slot = find_free_thread_slot();
		if (slot < 0) {
            printf("[HTTP] WARNING: No free thread slots, dropping connection\n");
            net_close(csock);
            continue;
        }

        printf("[HTTP] Assigned thread slot %d\n", slot);

        client_ctx_t *ctx = (client_ctx_t *)malloc(sizeof(client_ctx_t));
        ctx->sock = csock;
        ctx->addr = client_addr;

        g_thread_used[slot] = 1;
        LWP_CreateThread(&g_threads[slot],
                         client_thread_func,
                         ctx,
                         NULL,
                         0x20000,
                         LWP_PRIO_NORMAL);
	    LWP_DetachThread(g_threads[slot]);

    }
}