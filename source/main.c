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
#ifdef HW_RVL
#include <wiiuse/wpad.h>
#endif

static void *xfb = NULL;
static GXRModeObj *rmode = NULL;

void *initialise();
void *httpd (void *arg);

static	lwp_t httpd_handle = LWP_THREAD_NULL;

extern void *gc_http_server(void *arg);
//---------------------------------------------------------------------------------
int main(int argc, char **argv) {
//---------------------------------------------------------------------------------
	s32 ret;

	char localip[16] = {0};
	char gateway[16] = {0};
	char netmask[16] = {0};
	
	xfb = initialise();
#ifdef HW_RVL
	printf ("\nWii HTTP Server 0.2 by emu_kidid\n");
#else
	printf ("\nGC HTTP Server 0.2 by emu_kidid\n");
#endif
	printf("Configuring network ...\n");

	// Configure the network interface
	ret = if_config ( localip, netmask, gateway, true);
	if (ret>=0) {
		printf ("network configured, ip: %s, gw: %s, mask %s\n", localip, gateway, netmask);

		LWP_CreateThread(	&httpd_handle,	/* thread handle */ 
							gc_http_server, /* code */ 
							localip,		/* arg pointer for thread */
							NULL,			/* stack base */ 
							128*1024,		/* stack size */
							LWP_PRIO_NORMAL	/* thread priority */ );
	} else {
		printf ("network configuration failed!\n");
	}

	while(1) {

		VIDEO_WaitVSync();
#ifdef HW_RVL
		WPAD_ScanPads();
		int wPadButtonsDown = WPAD_ButtonsDown(0);

		if (wPadButtonsDown & WPAD_BUTTON_HOME) {
			exit(0);
		}
#endif
		PAD_ScanPads();

		int buttonsDown = PAD_ButtonsDown(0);
		
		if (buttonsDown & PAD_BUTTON_START) {
			exit(0);
		}
	}

	return 0;
}

//---------------------------------------------------------------------------------
void *initialise() {
//---------------------------------------------------------------------------------

	void *framebuffer;

	VIDEO_Init();
	PAD_Init();
#ifdef HW_RVL
	WPAD_Init();
#endif
	
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