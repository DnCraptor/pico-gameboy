#include "platform_linux.h"
#include "f_util.h"
#include "audio_linux.h"
#include "input_linux.h"
#include "video_drm.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static volatile sig_atomic_t pending = PLATFORM_RUN;
static char **saved_argv;

void platform_set_argv(char **argv)
{
	saved_argv = argv;
}

void platform_request(enum platform_request request)
{
	/* A quit outranks a reboot that is still pending. */
	if ((int)request > pending)
		pending = request;
}

/* Release everything the process holds outside itself: input grabs, the
 * CRTC (original console mode restored) and the PCM device. */
static void host_shutdown(void)
{
	input_shutdown();
	video_drm_close();
	alsa_close();
	fflush(NULL);
}

void platform_service(void)
{
	const int request = pending;
	if (request == PLATFORM_RUN)
		return;
	host_shutdown();
	if (request == PLATFORM_REBOOT && saved_argv) {
		/* RP2350: watchdog reset of the whole MCU.  Here: a fresh process,
		 * so every part of the emulator starts from its power-on state. */
		execv("/proc/self/exe", saved_argv);
		perror("reboot: execv");
		_exit(EXIT_FAILURE);
	}
	_exit(EXIT_SUCCESS);
}

const char *FRESULT_str(FRESULT i)
{
	switch (i) {
	case FR_OK: return "Succeeded";
	case FR_DISK_ERR: return "A hard error occurred in the low level disk I/O layer";
	case FR_INT_ERR: return "Assertion failed";
	case FR_NOT_READY: return "The physical drive cannot work";
	case FR_NO_FILE: return "Could not find the file";
	case FR_NO_PATH: return "Could not find the path";
	case FR_INVALID_NAME: return "The path name format is invalid";
	case FR_DENIED: return "Access denied due to prohibited access or directory full";
	case FR_EXIST: return "Access denied due to prohibited access (exists)";
	case FR_INVALID_OBJECT: return "The file/directory object is invalid";
	case FR_WRITE_PROTECTED: return "The physical drive is write protected";
	case FR_INVALID_DRIVE: return "The logical drive number is invalid";
	case FR_NOT_ENABLED: return "The volume has no work area (mount)";
	case FR_NO_FILESYSTEM: return "There is no valid FAT volume";
	case FR_MKFS_ABORTED: return "The f_mkfs() aborted due to any problem";
	case FR_TIMEOUT: return "Could not get a grant to access the volume within defined period";
	case FR_LOCKED: return "The operation is rejected according to the file sharing policy";
	case FR_NOT_ENOUGH_CORE: return "LFN working buffer could not be allocated";
	case FR_TOO_MANY_OPEN_FILES: return "Number of open files > FF_FS_LOCK";
	case FR_INVALID_PARAMETER: return "Given parameter is invalid";
	default: return "Unknown";
	}
}
