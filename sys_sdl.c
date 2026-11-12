
#ifdef WIN32
#include <io.h>
#include "conio.h"
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#endif

#ifdef __ANDROID__
#include <android/log.h>

#ifndef FNDELAY
#define FNDELAY		O_NDELAY
#endif
#endif

#include <signal.h>

#include <SDL.h>

#ifdef WIN32
#ifdef _MSC_VER
#if SDL_MAJOR_VERSION == 1
#pragma comment(lib, "sdl.lib")
#pragma comment(lib, "sdlmain.lib")
#else
#pragma comment(lib, "sdl2.lib")
#pragma comment(lib, "sdl2main.lib")
#endif
#endif
#endif

#include "quakedef.h"

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

extern qboolean host_shuttingdown;
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

void Sys_Shutdown (void)
{
#ifdef __ANDROID__
	Sys_AllowProfiling(false);
#endif
#ifndef WIN32
	fcntl (0, F_SETFL, fcntl (0, F_GETFL, 0) & ~FNDELAY);
#endif
	fflush(stdout);
	SDL_Quit();
}


void Sys_Error (const char *error, ...)
{
	va_list argptr;
	char string[MAX_INPUTLINE];

// change stdin to non blocking
#ifndef WIN32
	fcntl (0, F_SETFL, fcntl (0, F_GETFL, 0) & ~FNDELAY);
#endif

	va_start (argptr,error);
	dpvsnprintf (string, sizeof (string), error, argptr);
	va_end (argptr);

	Con_Printf ("Engine error: %s\n", string);

#ifdef WIN32
	MessageBox(NULL, string, "Quake Error", MB_OK | MB_SETFOREGROUND | MB_ICONSTOP);
#endif
#ifdef __ANDROID__
	// the console line above can be lost (some phones rate limit logcat),
	// and a player sees only the app closing: log it as an error and show it
	__android_log_print(ANDROID_LOG_ERROR, "darkplaces", "Engine Error: %s", string);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Insanotic error", string, NULL);
#endif

	Host_Shutdown ();
	exit (1);
}

static int outfd = 1;
void Sys_PrintToTerminal(const char *text)
{
#ifdef __ANDROID__
	// always: without a terminal, logcat is the only place a failed start
	// (missing data, QC errors) can be read
	__android_log_write(ANDROID_LOG_INFO, "darkplaces", text);
#else
	if(outfd < 0)
		return;
#ifdef FNDELAY
	// BUG: for some reason, NDELAY also affects stdout (1) when used on stdin (0).
	// this is because both go to /dev/tty by default!
	{
		int origflags = fcntl (outfd, F_GETFL, 0);
		fcntl (outfd, F_SETFL, origflags & ~FNDELAY);
#endif
#ifdef WIN32
#define write _write
#endif
		while(*text)
		{
			fs_offset_t written = (fs_offset_t)write(outfd, text, (int)strlen(text));
			if(written <= 0)
				break; // sorry, I cannot do anything about this error - without an output
			text += written;
		}
#ifdef FNDELAY
		fcntl (outfd, F_SETFL, origflags);
	}
#endif
	//fprintf(stdout, "%s", text);
#endif
}

char *Sys_ConsoleInput(void)
{
//	if (cls.state == ca_dedicated)
	{
		static char text[MAX_INPUTLINE];
		int len = 0;
#ifdef WIN32
		int c;

		// read a line out
		while (_kbhit ())
		{
			c = _getch ();
			_putch (c);
			if (c == '\r')
			{
				text[len] = 0;
				_putch ('\n');
				len = 0;
				return text;
			}
			if (c == 8)
			{
				if (len)
				{
					_putch (' ');
					_putch (c);
					len--;
					text[len] = 0;
				}
				continue;
			}
			text[len] = c;
			len++;
			text[len] = 0;
			if (len == sizeof (text))
				len = 0;
		}
#else
		fd_set fdset;
		struct timeval timeout;
		FD_ZERO(&fdset);
		FD_SET(0, &fdset); // stdin
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		if (select (1, &fdset, NULL, NULL, &timeout) != -1 && FD_ISSET(0, &fdset))
		{
			len = read (0, text, sizeof(text));
			if (len >= 1)
			{
				// rip off the \n and terminate
				text[len-1] = 0;
				return text;
			}
		}
#endif
	}
	return NULL;
}

void Sys_SetClipboardData (const char *text)
{
#if SDL_MAJOR_VERSION != 1
	SDL_SetClipboardText(text);
#else
	(void)text;
#endif
}

char *Sys_GetClipboardData (void)
{
#if SDL_MAJOR_VERSION != 1
	char *data = NULL;
	char *cliptext;

	cliptext = SDL_GetClipboardText();
	if (cliptext != NULL) {
		size_t allocsize;
		allocsize = strlen(cliptext) + 1;
		data = (char *)Z_Malloc (allocsize);
		strlcpy (data, cliptext, allocsize);
		SDL_free(cliptext);
	}

	return data;
#elif defined(WIN32)
	char *data = NULL;
	char *cliptext;

	if (OpenClipboard (NULL) != 0)
	{
		HANDLE hClipboardData;

		if ((hClipboardData = GetClipboardData (CF_TEXT)) != 0)
		{
			if ((cliptext = (char *)GlobalLock (hClipboardData)) != 0)
			{
				size_t allocsize;
				allocsize = GlobalSize (hClipboardData) + 1;
				data = (char *)Z_Malloc (allocsize);
				strlcpy (data, cliptext, allocsize);
				GlobalUnlock (hClipboardData);
			}
		}
		CloseClipboard ();
	}
	return data;
#else
	return NULL;
#endif
}

void Sys_InitConsole (void)
{
}

int main (int argc, char *argv[])
{
	signal(SIGFPE, SIG_IGN);

#ifdef __ANDROID__
	Sys_AllowProfiling(true);
	atexit(Sys_Android_AtExit);
#endif

	com_argc = argc;
	com_argv = (const char **)argv;
	Sys_ProvideSelfFD();

	// COMMANDLINEOPTION: sdl: -noterminal disables console output on stdout
	if(COM_CheckParm("-noterminal"))
		outfd = -1;
	// COMMANDLINEOPTION: sdl: -stderr moves console output to stderr
	else if(COM_CheckParm("-stderr"))
		outfd = 2;
	else
		outfd = 1;

#ifndef WIN32
	fcntl(0, F_SETFL, fcntl (0, F_GETFL, 0) | FNDELAY);
#endif

	// we don't know which systems we'll want to init, yet...
	SDL_Init(0);

	Host_Main();

	return 0;
}

qboolean sys_supportsdlgetticks = true;
unsigned int Sys_SDL_GetTicks (void)
{
	return SDL_GetTicks();
}
void Sys_SDL_Delay (unsigned int milliseconds)
{
	SDL_Delay(milliseconds);
}
