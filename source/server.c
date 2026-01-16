// server.c
//
// Minimal GameCube/Wii HTTP file server using libogc2 + LWP + a single-file HTTP parser.
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
#include <sys/stat.h>
#include <ogc/timesupp.h>
#include "picohttpparser.h"

#ifdef HW_RVL
#define PLATFORM "Wii"
#else
#define PLATFORM "GameCube"
#endif

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
static u64 g_total_bytes_recv = 0;
static mutex_t cache_mutex;
static mutex_t thread_mutex;

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

void handle_upload(int sock,
                          const char *req_buf, int req_len, int header_bytes,
                          struct phr_header *headers, size_t num_headers,
                          const char *query);

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
    cache_entry_t *result = NULL;

    LWP_MutexLock(cache_mutex);
    for (int i = 0; i < MAX_CACHE_ENTRIES; ++i) {
        if (g_cache.entries[i].size == 0) continue;
        if (strcmp(g_cache.entries[i].path, path) == 0) {
            result = &g_cache.entries[i];
            break;
        }
    }
    LWP_MutexUnlock(cache_mutex);

    return result;
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
	//printf("Got size [%ld]\n" ,sz);

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
		LWP_MutexLock(cache_mutex);
		cache_lru_move_to_front(e);
		LWP_MutexUnlock(cache_mutex);
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
	//printf("fread size %ld to %08X\n", sz, (u32)new_e->data);
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
	
	n += snprintf(pagebuf + n, sizeof(pagebuf) - n,
		"<h3>Upload file (to this folder)</h3>"
		"<form id=\"upload_form\" action=\"/upload?path=%s&overwrite=0\" "
		"method=\"POST\" enctype=\"multipart/form-data\" "
		"onsubmit=\"document.getElementById('upload_status').innerText='Uploading... (writing asynchronously to storage)';\">"
		"<input type=\"file\" name=\"file\" required>"
		"<input type=\"hidden\" id=\"ow_hidden\" name=\"overwrite\" value=\"0\">"
		"<label>"
			"<input type=\"checkbox\" id=\"ow_box\" "
			"onclick=\""
				"var v=this.checked?1:0;"
				"document.getElementById('ow_hidden').value=v;"
				"document.getElementById('upload_form').action="
					"'/upload?path=%s&overwrite=' + v;"
			"\"> "
			"Overwrite if file exists"
		"</label>"
		"<input type=\"submit\" value=\"Upload\">"
		"</form>"
		"<div id=\"upload_status\"></div>",
		clean_url, clean_url
	);

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
	char data_recv_str[64];

	LWP_MutexLock(stats_mutex);
	u64 served = g_total_bytes_served;
	u64 recv = g_total_bytes_recv;
	LWP_MutexUnlock(stats_mutex);

	format_bytes(served, data_served_str, sizeof(data_served_str));
	format_bytes(recv, data_recv_str, sizeof(data_recv_str));


    int used_entries = 0;
    for (int i = 0; i < MAX_CACHE_ENTRIES; i++) {
        if (g_cache.entries[i].size > 0)
            used_entries++;
    }

    int active_threads = 0;
    LWP_MutexLock(thread_mutex);
	for (int i = 0; i < MAX_THREADS; i++) {
		if (g_thread_used[i])
			active_threads++;
	}
	LWP_MutexUnlock(thread_mutex);


    float used_kb = (float)g_cache.used / 1024.0f;
    float total_kb = (float)g_cache.arena_size / 1024.0f;

    u64 now = gettime();
    u32 uptime_sec = diff_sec(server_start_ticks, now);

    int n = snprintf(page, sizeof(page),
        "<html><body>"
        "<h1>%s HTTP Server Status</h1>"
        "<p><b>Uptime:</b> %u seconds</p>"
        "<p><b>Active threads:</b> %d / %d</p>"
        "<p><b>Cache usage:</b> %.1f KB / %.1f KB</p>"
        "<p><b>Cached entries:</b> %d</p>"
        "<p><b>Total data served:</b> %s</p>"
		"<p><b>Total data received:</b> %s</p>"
        "<h2>Thread Slots</h2><ul>",
		PLATFORM,
        uptime_sec,
        active_threads, MAX_THREADS,
        used_kb, total_kb,
        used_entries,
        data_served_str,
		data_recv_str
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

// ----------------------------- Upload helpers ---------------------------

static const char *find_header(struct phr_header *headers, size_t num_headers, const char *name)
{
    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len == strlen(name) &&
            strncasecmp(headers[i].name, name, headers[i].name_len) == 0) {
            return headers[i].value;
        }
    }
    return NULL;
}

static int get_content_length(struct phr_header *headers, size_t num_headers)
{
    const char *cl = find_header(headers, num_headers, "Content-Length");
    if (!cl) return -1;
    return atoi(cl);
}

static int extract_boundary(const char *ct, char *out, size_t out_size)
{
    if (!ct) return -1;

    const char *b = strstr(ct, "boundary=");
    if (!b) return -1;

    b += 9; // skip "boundary="

    // Skip leading spaces
    while (*b == ' ' || *b == '\t') b++;

    // Optional quotes
    int quoted = 0;
    if (*b == '"') {
        quoted = 1;
        b++;
    }

    size_t i = 0;
    while (*b && i + 1 < out_size) {
        if (quoted) {
            if (*b == '"') break;
        } else {
            if (*b == ';' || *b == ' ' || *b == '\t' || *b == '\r' || *b == '\n')
                break;
        }
        out[i++] = *b++;
    }
    out[i] = '\0';

    return (i > 0) ? (int)i : -1;
}

static void parse_upload_path_param(const char *query, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!query) return;

    const char *p = strstr(query, "path=");
    if (!p) return;
    p += 5;

    char buf[512];
    size_t i = 0;
    while (*p && *p != '&' && i + 1 < sizeof(buf)) {
        buf[i++] = *p++;
    }
    buf[i] = '\0';

    url_decode(buf);
    snprintf(out, out_size, "%s", buf);
}

static int query_has_overwrite(const char *query)
{
    if (!query) return 0;
    const char *p = strstr(query, "overwrite=");
    if (!p) return 0;
    p += 10;
    return (*p == '1');
}

int net_recv_with_stats(int sock, void *buf, int len, int flags)
{
    int r = net_recv(sock, buf, len, flags);

    if (r > 0) {
        LWP_MutexLock(stats_mutex);
        g_total_bytes_recv += r;
        LWP_MutexUnlock(stats_mutex);
    }

    return r;
}

// Handle a single HTTP request from a connected socket
static void handle_http_request(int sock, const char *doc_root)
{
    printf("[HTTP] Receiving request...\n");
    char buf[MAX_REQUEST_SIZE];
    int r = net_recv_with_stats(sock, buf, sizeof(buf), 0);
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
        send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
        return;
    }

    printf("[HTTP] Method: %.*s\n", (int)method_len, method);
    printf("[HTTP] Raw path: %.*s\n", (int)path_len, path);

    char req_path[MAX_PATH_LEN];
    if ((size_t)(path - buf) + 1 >= sizeof(req_path)) {
        const char *msg = "Request-URI Too Long";
        send_response(sock, 414, "URI Too Long", "text/plain", msg, strlen(msg));
        return;
    }

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

    // POST HANDLING (upload)
    if (strncmp(method, "POST", 4) == 0) {

        if (strcmp(rel_path, "upload") == 0) {
            printf("[HTTP] Handling upload\n");
            int header_len = pret;
			handle_upload(sock, buf, r, header_len, headers, num_headers, query);
            return;
        }

        const char *msg = "Method Not Allowed";
        send_response(sock, 405, "Method Not Allowed", "text/plain", msg, strlen(msg));
        return;
    }

    // GET HANDLING
    if (strncmp(method, "GET", 3) != 0) {
        const char *msg = "Method Not Allowed";
        send_response(sock, 405, "Method Not Allowed", "text/plain", msg, strlen(msg));
        return;
    }

    // /storage/... handling
    if (strncmp(rel_path, "storage", 7) == 0) {

        const char *sub = rel_path + 7;
        if (*sub == '/') sub++;

        char fs_path[512];
        snprintf(fs_path, sizeof(fs_path), "/%s", sub);

        // Directory listing
        if (sub[0] == '\0' || rel_path[strlen(rel_path)-1] == '/') {

            char clean_url[512];
            snprintf(clean_url, sizeof(clean_url), "/storage/%s", sub);

            char display_url[768];
            if (query)
                snprintf(display_url, sizeof(display_url), "%s?%s", clean_url, query);
            else
                snprintf(display_url, sizeof(display_url), "%s", clean_url);

            send_directory_listing(sock, fs_path, clean_url, display_url);
            return;
        }

        // File download
        const char *data = NULL;
        size_t size = 0;

        int cache_state = cache_get_file("/", fs_path + 1, &data, &size);

        if (cache_state == CACHE_ERROR) {
            printf("[HTTP] 404 Not Found: %s\n", fs_path);
            const char *msg = "Not Found";
            send_response(sock, 404, "Not Found", "text/plain", msg, strlen(msg));
            return;
        }

        const char *ct = guess_content_type(fs_path);

        if (cache_state == CACHE_STREAM) {
            printf("[HTTP] Streaming large file: %s (%u bytes)\n", fs_path, (unsigned)size);
            stream_file(sock, fs_path, size, ct);
            return;
        }

        printf("[HTTP] Sending file %s (%u bytes)\n", fs_path, (unsigned)size);
        send_response(sock, 200, "OK", ct, data, size);

        if (cache_state == CACHE_LOADED)
            free((void*)data);

        return;
    }

    // /status
    if (strcmp(rel_path, "status") == 0 ||
        strcmp(rel_path, "status.html") == 0)
    {
        printf("[HTTP] Serving status page\n");
        send_status_page(sock);
        return;
    }

    // Static file mapping handling from /www
    const char *data = NULL;
    size_t size = 0;
    int cache_state = cache_get_file(doc_root, rel_path, &data, &size);

    if (cache_state == CACHE_ERROR) {
        printf("[HTTP] 404 Not Found: %s\n", rel_path);
        const char *msg = "Not Found";
        send_response(sock, 404, "Not Found", "text/plain", msg, strlen(msg));
        return;
    }

    const char *ct = guess_content_type(rel_path);

	// STREAMING MODE
    if (cache_state == CACHE_STREAM) {
        printf("[HTTP] Streaming large file: %s (%u bytes)\n", rel_path, (unsigned)size);

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", doc_root, rel_path);

        stream_file(sock, full_path, size, ct);
        return;
    }

	// NORMAL MODE (cached or small uncached)
    if (cache_state == CACHE_HIT)
        printf("[HTTP] Cache HIT: %s (%u bytes)\n", rel_path, (unsigned)size);
    else if (cache_state == CACHE_LOADED)
        printf("[HTTP] Cache MISS (loaded uncached): %s (%u bytes)\n", rel_path, (unsigned)size);

    printf("[HTTP] Sending 200 OK (%u bytes)\n", (unsigned)size);
    send_response(sock, 200, "OK", ct, data, size);

    if (cache_state == CACHE_LOADED)
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
	LWP_MutexLock(thread_mutex);
    for (int i = 0; i < MAX_THREADS; ++i) {
		if (g_threads[i] == LWP_GetSelf()) {
			g_thread_used[i] = 0;
			g_threads[i] = 0;
			break;
		}
	}
	LWP_MutexUnlock(thread_mutex);
	printf("[HTTP] Thread %u finished\n", (unsigned)LWP_GetSelf());
    return NULL;
}

static int find_free_thread_slot(void)
{
    int idx = -1;
    LWP_MutexLock(thread_mutex);
    for (int i = 0; i < MAX_THREADS; ++i) {
        if (!g_thread_used[i]) {
            g_thread_used[i] = 1; // reserve immediately
            idx = i;
            break;
        }
    }
    LWP_MutexUnlock(thread_mutex);
    return idx;
}


void *gc_http_server(void *arg)
{
	server_start_ticks = gettime();
	LWP_MutexInit(&fs_mutex, 0);
	LWP_MutexInit(&stats_mutex, 0);
	LWP_MutexInit(&cache_mutex, 0);
	LWP_MutexInit(&thread_mutex, 0);

    printf("[HTTP] Initializing cache (%u bytes)\n", CACHE_SIZE_BYTES);
    cache_init(CACHE_SIZE_BYTES);

    printf("[HTTP] Creating socket...\n");
    int sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (sock < 0) {
        printf("[HTTP] ERROR: net_socket failed\n");
        return NULL;
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
        return NULL;
    }
    printf("[HTTP] Bind OK\n");

	if (net_listen(sock, 4) < 0) {
        printf("[HTTP] ERROR: net_listen failed\n");
        net_close(sock);
        return NULL;
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
        } else {
			LWP_MutexLock(thread_mutex);
			printf("[HTTP] Assigned thread slot %d\n", slot);

			client_ctx_t *ctx = (client_ctx_t *)malloc(sizeof(client_ctx_t));
			ctx->sock = csock;
			ctx->addr = client_addr;
			LWP_MutexUnlock(thread_mutex);
			LWP_CreateThread(&g_threads[slot],
							 client_thread_func,
							 ctx,
							 NULL,
							 0x20000,
							 LWP_PRIO_NORMAL);
			LWP_DetachThread(g_threads[slot]);
		}
    }
	return NULL;
}
#define UPLOAD_BUF_SIZE 40960

static const char *memmem_safe(const char *haystack, size_t haystack_len,
                               const char *needle, size_t needle_len)
{
    if (!haystack || !needle || needle_len == 0 || haystack_len < needle_len)
        return NULL;
    for (size_t i = 0; i + needle_len <= haystack_len; i++) {
        if (memcmp(haystack + i, needle, needle_len) == 0)
            return haystack + i;
    }
    return NULL;
}

static int read_more_into_buf(
    int sock,
    char *buf, int *buf_len,
    const char **body_ptr, int *body_already,
    int *remaining
) {
	
	//printf("[RECV] read_more_into_buf: buf_len=%u remaining=%u\n",
	//		   (unsigned)*buf_len, (unsigned)*remaining);

    if (*remaining <= 0)
        return 0;

    if (*buf_len < 0) *buf_len = 0;
	if (*buf_len > UPLOAD_BUF_SIZE) *buf_len = UPLOAD_BUF_SIZE;

	int space = UPLOAD_BUF_SIZE - *buf_len;
	if (space <= 0) {
		// buffer full — caller must consume before reading more
		return 0;
	}

    int to_copy = 0;

    if (*body_already > 0) {
        to_copy = (*body_already < space) ? *body_already : space;
        memcpy(buf + *buf_len, *body_ptr, to_copy);
        *body_ptr += to_copy;
        *body_already -= to_copy;
    } else {	
		//printf("[RECV] about to net_recv: space=%d buf_len=%u remaining=%u\n",
		//	   space, (unsigned)*buf_len, (unsigned)*remaining);
		
        int r = net_recv_with_stats(sock, buf + *buf_len, space, 0);
		//printf("[RECV] net_recv returned %d (len=%d)\n", r, space);
        if (r <= 0)
            return r;
        to_copy = r;
    }

    *buf_len += to_copy;
    *remaining -= to_copy;
    return to_copy;
}

void drain_post_body(int sock, int remaining) {
	// Drain remaining POST body so browser doesn't hang
	char drain_buf[4096];
	while (remaining > 0) {
		int to_read = (remaining > sizeof(drain_buf)) ? sizeof(drain_buf) : remaining;
		int r = net_recv_with_stats(sock, drain_buf, to_read, 0);
		if (r <= 0)
			break;
		remaining -= r;
	}
}

void handle_upload(int sock,
                   const char *req_buf, int req_len, int header_bytes,
                   struct phr_header *headers, size_t num_headers,
                   const char *query)
{
    if (header_bytes > req_len)
        header_bytes = req_len;

    const char *ct = find_header(headers, num_headers, "Content-Type");
    char boundary[128];
    if (extract_boundary(ct, boundary, sizeof(boundary)) <= 0) {
        const char *msg = "Unsupported Content-Type";
		printf("[UPLOAD] Sending Unsupported Content-Type\n");
        send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
        return;
    }

    int content_length = get_content_length(headers, num_headers);
    int body_already = (req_len > header_bytes) ? (req_len - header_bytes) : 0;
    int remaining = content_length - body_already;
	printf("[UPLOAD] Content-Length: %d\n", content_length);
    if (content_length <= 0 || content_length > (0x7FFFFFFF)) {
		drain_post_body(sock, remaining);
        const char *msg = "Invalid or too large upload";
		printf("[UPLOAD] Sending Invalid or too large upload\n");
        send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
        return;
    }

    char b_start[160];
    char b_end[160];
    snprintf(b_start, sizeof(b_start), "--%s", boundary);
    snprintf(b_end, sizeof(b_end), "--%s--", boundary);
    size_t b_start_len = strlen(b_start);
    size_t b_end_len   = strlen(b_end);

    const char *body_ptr = req_buf + header_bytes;
    char buf[UPLOAD_BUF_SIZE];
    int buf_len = 0;

    // Seed buffer with any already-received body bytes
    if (body_already > 0) {
        int to_copy = (body_already < UPLOAD_BUF_SIZE) ? body_already : UPLOAD_BUF_SIZE;
        memcpy(buf, body_ptr, to_copy);
        buf_len = to_copy;
        body_ptr += to_copy;
        body_already -= to_copy;
        remaining = content_length - body_already - buf_len;
    }
	
	// Wii FIX: ensure we have at least some body data before boundary search
	if (buf_len == 0 && remaining > 0) {
		int r = read_more_into_buf(sock, buf, &buf_len, &body_ptr, &body_already, &remaining);
		if (r <= 0) {
			const char *msg = "Malformed multipart data (no initial body)";
			send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
			return;
		}
	}
	
	//printf("[UPLOAD] After initial seed: body_already=%d buf_len=%d remaining=%d\n",
    //   body_already, buf_len, remaining);

    // Skip initial boundary line: "--boundary\r\n"
    while (1) {
        const char *p = memmem_safe(buf, buf_len, b_start, b_start_len);
        if (p) {
            int off = (int)(p - buf);
            int need = off + (int)b_start_len + 2; // + "\r\n"
            while (buf_len < need && remaining > 0) {
                if (read_more_into_buf(sock, buf, &buf_len, &body_ptr, &body_already, &remaining) <= 0)
                    break;
            }
            if (buf_len < need) {
				drain_post_body(sock, remaining);
                const char *msg = "Malformed multipart data (1)";
                send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
                return;
            }
            int consume = need;
			int new_len = buf_len - consume;
			if (new_len < 0) new_len = 0;
			if (new_len > UPLOAD_BUF_SIZE) new_len = UPLOAD_BUF_SIZE;

			memmove(buf, buf + consume, new_len);
			buf_len = new_len;
            break;
        }
        if (buf_len >= (int)(b_start_len + 4)) {
			drain_post_body(sock, remaining);
            const char *msg = "Malformed multipart data (2)";
            send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
            return;
        }
        if (read_more_into_buf(sock, buf, &buf_len, &body_ptr, &body_already, &remaining) <= 0) {
			drain_post_body(sock, remaining);
            const char *msg = "Malformed multipart data (3)";
            send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
            return;
        }
    }
	//printf("[UPLOAD] Skipped boundary\n");

    // Read part headers (Content-Disposition, etc.) until blank line
    char header_block[1024];
    int header_len = 0;
    while (1) {
        const char *sep = memmem_safe(buf, buf_len, "\r\n\r\n", 4);
        if (sep) {
            int hdr_bytes = (int)(sep - buf) + 4;
            int copy = (hdr_bytes < (int)sizeof(header_block)) ? hdr_bytes : (int)sizeof(header_block) - 1;
            memcpy(header_block, buf, copy);
            header_block[copy] = '\0';
            int consume = hdr_bytes;
            int new_len = buf_len - consume;
			if (new_len < 0) new_len = 0;
			if (new_len > UPLOAD_BUF_SIZE) new_len = UPLOAD_BUF_SIZE;

			memmove(buf, buf + consume, new_len);
			buf_len = new_len;
            header_len = copy;
            break;
        }
        if (buf_len >= (int)sizeof(header_block) - 1) {
			drain_post_body(sock, remaining);
            const char *msg = "Multipart headers too large";
            send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
            return;
        }
        if (read_more_into_buf(sock, buf, &buf_len, &body_ptr, &body_already, &remaining) <= 0) {
			drain_post_body(sock, remaining);
            const char *msg = "Malformed multipart headers";
            send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
            return;
        }
    }
	//printf("[UPLOAD] Read part headers\n");

    // Extract filename from header_block
    const char *file_part = strstr(header_block, "Content-Disposition:");
    if (!file_part) {
		drain_post_body(sock, remaining);
        const char *msg = "No file part";
        send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
        return;
    }
    const char *fn = strstr(file_part, "filename=\"");
    if (!fn) {
		drain_post_body(sock, remaining);
        const char *msg = "Missing filename";
        send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
        return;
    }
    fn += 10;
    char filename[256];
    size_t fi = 0;
    while (*fn && *fn != '"' && fi + 1 < sizeof(filename)) {
        filename[fi++] = *fn++;
    }
    filename[fi] = '\0';

    // Build path on SD (etc) from query
    char logical_path[512];
    parse_upload_path_param(query, logical_path, sizeof(logical_path));
    if (logical_path[0] == '\0') {
		drain_post_body(sock, remaining);
        const char *msg = "Missing path parameter";
        send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
        return;
    }

    char fs_dir[512];
    if (strncmp(logical_path, "/storage", 8) == 0) {
        const char *sub = logical_path + 8;
        if (*sub == '/') sub++;
        snprintf(fs_dir, sizeof(fs_dir), "/%s", sub);
    } else {
        snprintf(fs_dir, sizeof(fs_dir), "%s", logical_path);
    }

    size_t dl = strlen(fs_dir);
    if (dl == 0 || fs_dir[dl - 1] != '/') {
        fs_dir[dl] = '/';
        fs_dir[dl + 1] = '\0';
    }

    char fs_path[768];
    snprintf(fs_path, sizeof(fs_path), "%s%s", fs_dir, filename);
	printf("[UPLOAD] Destination %s\n", fs_path);
   
	int overwrite = query_has_overwrite(query);

    struct stat st;
    int exists = (stat(fs_path, &st) == 0);
    if (exists && !overwrite) {
		drain_post_body(sock, remaining);
		char page[1024];
		char existing_size[64];
		format_bytes((u64)st.st_size, existing_size, sizeof(existing_size));
		int n = snprintf(page, sizeof(page),
			"<html><body>"
			"<h1>Upload error</h1>"
			"<p>File <code>%s</code> already exists.</p>"
			"<p><b>Existing size:</b> %s</p>"
			"<p><a href=\"javascript:history.back()\">Go back</a></p>"
			"</body></html>",
			filename, existing_size
		);

		send_response(sock, 409, "Conflict", "text/html", page, (size_t)n);
		return;
	}

    LWP_MutexLock(fs_mutex);
    FILE *f = fopen(fs_path, "wb");
    if (!f) {
        LWP_MutexUnlock(fs_mutex);
		drain_post_body(sock, remaining);
        const char *msg = "Failed to open file for writing";
        send_response(sock, 500, "Internal Server Error", "text/plain", msg, strlen(msg));
        return;
    }

	printf("[STREAM-START] buf_len=%d remaining=%d\n", buf_len, remaining);
    // Stream file data until next boundary
    size_t total_written = 0;
    while (1) {
		//printf("[STREAM] remaining=%d buf_len=%d\n", remaining, buf_len);
        // Ensure we have enough data to check for boundary
        if (buf_len < (int)(b_start_len + 4) && remaining > 0) {
			if (read_more_into_buf(sock, buf, &buf_len, &body_ptr, &body_already, &remaining) <= 0 && remaining > 0) {
                fclose(f);
                LWP_MutexUnlock(fs_mutex);
				drain_post_body(sock, remaining);
                const char *msg = "Upload aborted (1)";
                send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
                return;
            }
        }

        const char *nb = memmem_safe(buf, buf_len, b_start, b_start_len);
        const char *ne = memmem_safe(buf, buf_len, b_end, b_end_len);
        const char *next_boundary = NULL;
        int is_final = 0;

        if (nb && ne)
            next_boundary = (nb < ne) ? nb : ne;
        else if (nb)
            next_boundary = nb;
        else if (ne)
            next_boundary = ne;

        if (next_boundary == ne)
            is_final = 1;

        if (next_boundary) {
            int off = (int)(next_boundary - buf);
            int write_len = off;
            if (write_len >= 2 &&
                buf[write_len - 2] == '\r' &&
                buf[write_len - 1] == '\n')
            {
                write_len -= 2;
            }
            if (write_len > 0) {
                size_t w = fwrite(buf, 1, (size_t)write_len, f);
                total_written += w;
                if (w != (size_t)write_len) {
                    fclose(f);
                    LWP_MutexUnlock(fs_mutex);
					drain_post_body(sock, remaining);
                    const char *msg = "Failed to write complete file";
                    send_response(sock, 500, "Internal Server Error", "text/plain", msg, strlen(msg));
                    return;
                }
            }
            // Consume up to boundary (including preceding CRLF)
            int consume = off;
            int new_len = buf_len - consume;
			if (new_len < 0) new_len = 0;
			if (new_len > UPLOAD_BUF_SIZE) new_len = UPLOAD_BUF_SIZE;

			memmove(buf, buf + consume, new_len);
			buf_len = new_len;
			break;
        }

        // No boundary yet: write all but a tail that might contain partial boundary
        int safe = buf_len - (int)(b_start_len + 4);
        if (safe >= 32768 || remaining == 0) {
			u64 t0 = gettime();
			size_t w = fwrite(buf, 1, safe, f);
			u64 t1 = gettime();
			//printf("[WRITE] wrote %u bytes in %u ms\n",
			//	   (unsigned)w, (unsigned)diff_msec(t0, t1));

            total_written += w;
            if (w != (size_t)safe) {
                fclose(f);
                LWP_MutexUnlock(fs_mutex);
				drain_post_body(sock, remaining);
                const char *msg = "Failed to write complete file";
                send_response(sock, 500, "Internal Server Error", "text/plain", msg, strlen(msg));
                return;
            }
            int new_len = buf_len - safe;
			if (new_len < 0) new_len = 0;
			if (new_len > UPLOAD_BUF_SIZE) new_len = UPLOAD_BUF_SIZE;

			memmove(buf, buf + safe, new_len);
			buf_len = new_len;
        }

        if (remaining <= 0 && buf_len <= (int)(b_start_len + 4)) {
            fclose(f);
            LWP_MutexUnlock(fs_mutex);
			drain_post_body(sock, remaining);
            const char *msg = "Malformed multipart body";
            send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
            return;
        }
        if (read_more_into_buf(sock, buf, &buf_len, &body_ptr, &body_already, &remaining) <= 0 && remaining > 0) {
				printf("[ABORT2] read_more returned <=0, remaining=%d buf_len=%d\n",
		   remaining, buf_len);
            fclose(f);
            LWP_MutexUnlock(fs_mutex);
			drain_post_body(sock, remaining);

            const char *msg = "Upload aborted (2)";
            send_response(sock, 400, "Bad Request", "text/plain", msg, strlen(msg));
            return;
        }
    }

    fclose(f);
    LWP_MutexUnlock(fs_mutex);

    if (strncmp(fs_path, "/www/", 5) == 0) {
        // TODO: evict cache entry for this file if present
    }

    char page[1024];
    int n = snprintf(page, sizeof(page),
        "<html><body>"
        "<h1>Upload complete</h1>"
        "<p>Uploaded <code>%s</code> (%u bytes).</p>"
        "<p>Redirecting back to <code>%s</code> in 2 seconds...</p>"
        "<script>"
        "setTimeout(function(){ window.location='%s'; }, 2000);"
        "</script>"
        "</body></html>",
        filename, (unsigned)total_written, logical_path, logical_path
    );

    send_response(sock, 200, "OK", "text/html", page, (size_t)n);
}
