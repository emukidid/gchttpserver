#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <ogcsys.h>
#include <gccore.h>
#include <network.h>
#include <debug.h>
#include <errno.h>
#include <fat.h>

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

void *initialise();
void *httpd (void *arg);

static	lwp_t httd_handle = (lwp_t)NULL;

//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	s32 ret;

	char localip[16] = {0};
	char gateway[16] = {0};
	char netmask[16] = {0};
	
	xfb = initialise();

	printf ("\nGC HTTP Server\n");
	printf("Configuring network ...\n");

	// Configure the network interface
	ret = if_config ( localip, netmask, gateway, true);
	if (ret>=0) {
		printf ("network configured, ip: %s, gw: %s, mask %s\n", localip, gateway, netmask);

		LWP_CreateThread(	&httd_handle,	/* thread handle */ 
							httpd,			/* code */ 
							localip,		/* arg pointer for thread */
							NULL,			/* stack base */ 
							64*1024,		/* stack size */
							50				/* thread priority */ );
	} else {
		printf ("network configuration failed!\n");
	}

	while(1) {

		VIDEO_WaitVSync();
		PAD_ScanPads();

		int buttonsDown = PAD_ButtonsDown(0);
		
		if (buttonsDown & PAD_BUTTON_START) {
			exit(0);
		}
	}

	return 0;
}

extern void start_gc_http_server(void);
//---------------------------------------------------------------------------------
void *httpd (void *arg) {
//---------------------------------------------------------------------------------

	while(1) {
		start_gc_http_server();
	}
	return NULL;
}

//---------------------------------------------------------------------------------
void *initialise() {
//---------------------------------------------------------------------------------

	void *framebuffer;

	VIDEO_Init();
	PAD_Init();
	
	rmode = VIDEO_GetPreferredMode(NULL);
	framebuffer = SYS_AllocateFramebuffer(rmode);
	CON_Init(framebuffer,0,0,rmode->fbWidth,rmode->xfbHeight,rmode->fbWidth*VI_DISPLAY_PIX_SZ);
	CON_EnableGecko(EXI_CHANNEL_1, TRUE);
	VIDEO_Configure(rmode);
	VIDEO_SetNextFramebuffer(framebuffer);
	VIDEO_SetBlack(false);
	VIDEO_Flush();
	VIDEO_WaitForFlush();
	fatInitDefault();

	return framebuffer;

}