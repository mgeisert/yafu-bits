/*
 * safe_system.c
 * Thread-safe version of system() for platforms needing it, like Cygwin.
 *
 * Copyright © 2018 Mark A. Geisert.  Placed in public domain.
 *
 * POSIX states that system() need not be thread-safe.  On Cygwin it isn't.
 * POSIX also states that system() cannot return to its caller until the
 * task it has spawned has completed.
 *
 * If a Cygwin process uses system() simultaneously from multiple pthreads,
 * the Cygwin DLL reports internal errors and often hangs the process.  This
 * is easily worked around by wrapping system() calls in a critical section.
 * However, doing this has the effect of running the (potentially long
 * duration) task spawned by system() while holding the critical section the
 * entire time.  This behavior ends up serializing the pthreads that use
 * system() and the benefits of parallelization are lost.
 *
 * After investigation of various approaches, what is done here is to launch
 * the spawned task as a background task, obtain its pid, and wait for that
 * pid to become invalid (which indicates the task has completed or aborted).
 *
 * Unfortunately, Plan A: waiting for the task with the usual waitpid() or
 * wait() cannot be done because the task of interest is not a direct child
 * of the running process.  There is a shell process interposed, as required
 * by POSIX for system() usage.
 *
 * Implemented here is Plan C: a Windows handle to the spawned task/process
 * (not the shell) is waited upon.  On platforms other than Windows or Cygwin,
 * Plan B described below might be workable.
 */

#if defined(__CYGWIN__)

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/cygwin.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

//XXX: CODE WRITTEN AND TESTED ON x86_64 AND HAS NOT BEEN TESTED ON x86

volatile int initializing;   // 0: not yet, 1: in progress, 2: initialized
CRITICAL_SECTION systemcrit; // critical section to serialize system() use
volatile int usecount;       // something to keep tracking pidfiles separated

int
safe_system(const char *cmd)
{
    // Initialize in thread-safe manner
    if (0 == InterlockedCompareExchange(&initializing, 1, 0)) {
	InitializeCriticalSection(&systemcrit);
	initializing++;
    }
    else while (1 == initializing) {
	usleep(10000); // 10ms
    }

    // Allocate space for name of pidfile
    char *pidfilename = alloca(64);

    // Allocate space for command buf including glue for the tracking pidfile
    char *buf = alloca(strlen(cmd) + 128);

    // Wrap system() call in a critical section as it's not thread-safe
    EnterCriticalSection(&systemcrit);

    // Set up the complete augmented command string
    sprintf(pidfilename, "_yafu_system_.%d", usecount++);
    char *ptr = stpcpy(buf, cmd);
    // The '&' puts task in background; that task's pid is captured in pidfile
    sprintf(ptr, " & echo -n $! > %s", pidfilename);

    // Call Cygwin's system() with augmented command string
#if 1
    fprintf(stderr, "*SYSTEM* >>%s<<\n", buf);
#endif
    int ret = system(buf);
    LeaveCriticalSection(&systemcrit);

    // Open pidfile identifying spawned task then read it, close it, delete it
    FILE *f = fopen(pidfilename, "r");
    if (!f)
	return -1; // bail if unable to open pidfile
    fread(buf, 1, 80, f);
    fclose(f);
    remove(pidfilename);
    pid_t pid = atoi(buf);

    // Plan A: wait() for spawned task. Doesn't work as task is not a child.
    // Plan B: use kill(pid, 0) to test pid for validity. Works, but ugly.
    // Plan C: (Windows only) open a handle to task and sync on it. Perfect.
    DWORD winpid = cygwin_internal(CW_CYGWIN_PID_TO_WINPID, pid);
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, winpid);
    if (h == INVALID_HANDLE_VALUE)
	return -1; // bail if unable to open spawned task/process
    WaitForSingleObject(h, INFINITE);
    CloseHandle(h);

    return ret;
}

#else

int
safe_system(const char *cmd)
{
    return system(cmd);
}

#endif
