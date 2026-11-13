/*
 * Include this BEFORE darkplaces.h because it breaks wrapping
 * _Static_assert. Cloudwalk has no idea how or why so don't ask.
 */
#include <SDL.h>

#include "darkplaces.h"

#ifdef WIN32
#ifdef _MSC_VER
#pragma comment(lib, "sdl2.lib")
#pragma comment(lib, "sdl2main.lib")
#endif
#endif


#ifdef __ANDROID__
#include <dlfcn.h>
#include <unwind.h>

// Insanotic: something other than the engine's own error path can end the
// process with exit() (the system then reports only "exit status 1"). Log
// where every unexpected exit came from, as library + offset pairs that
// addr2line resolves against the unstripped libraries of the build.
static _Unwind_Reason_Code Sys_Android_LogFrame(struct _Unwind_Context *context, void *arg)
{
	int *depth = (int *)arg;
	uintptr_t pc = _Unwind_GetIP(context);
	Dl_info info;
	if (!pc)
		return _URC_END_OF_STACK;
	if (dladdr((void *)pc, &info) && info.dli_fname)
		__android_log_print(ANDROID_LOG_ERROR, "darkplaces", "  #%02d pc %08lx %s (%s)", *depth,
			(unsigned long)(pc - (uintptr_t)info.dli_fbase), info.dli_fname,
			info.dli_sname ? info.dli_sname : "?");
	else
		__android_log_print(ANDROID_LOG_ERROR, "darkplaces", "  #%02d pc %p", *depth, (void *)pc);
	return ++*depth < 48 ? _URC_NO_REASON : _URC_END_OF_STACK;
}

extern qbool host_shuttingdown;
static void Sys_Android_AtExit(void)
{
	int depth = 0;
	if (host_shuttingdown)
		return; // Sys_Quit: a normal quit
	__android_log_print(ANDROID_LOG_ERROR, "darkplaces", "unexpected exit, called from:");
	_Unwind_Backtrace(Sys_Android_LogFrame, &depth);
}
#endif

// =======================================================================
// General routines
// =======================================================================

void Sys_SDL_Shutdown(void)
{
	SDL_Quit();
}

// Sys_Error early in startup might screw with automated
// workflows or something if we show the dialog by default.
static qbool nocrashdialog = true;
void Sys_SDL_Dialog(const char *title, const char *string)
{
	if(!nocrashdialog)
		SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, string, NULL);
}

char *Sys_SDL_GetClipboardData (void)
{
	char *data = NULL;
	char *cliptext;

	cliptext = SDL_GetClipboardText();
	if (cliptext != NULL) {
		size_t allocsize;
		allocsize = min(MAX_INPUTLINE, strlen(cliptext) + 1);
		data = (char *)Z_Malloc (allocsize);
		dp_strlcpy (data, cliptext, allocsize);
		SDL_free(cliptext);
	}

	return data;
}

void Sys_SetClipboardData (const char *text)
{
	SDL_SetClipboardText(text);
}

void Sys_SDL_Init(void)
{
	// we don't know which systems we'll want to init, yet...
	if (SDL_Init(0) < 0)
		Sys_Error("SDL_Init failed: %s\n", SDL_GetError());

	// COMMANDLINEOPTION: sdl: -nocrashdialog disables "Engine Error" crash dialog boxes
	if(!Sys_CheckParm("-nocrashdialog"))
		nocrashdialog = false;
}

qbool sys_supportsdlgetticks = true;
unsigned int Sys_SDL_GetTicks (void)
{
	return SDL_GetTicks();
}
void Sys_SDL_Delay (unsigned int milliseconds)
{
	SDL_Delay(milliseconds);
}

int main(int argc, char *argv[])
{
#ifdef __ANDROID__
	atexit(Sys_Android_AtExit);
#endif
	return Sys_Main(argc, argv);
}
