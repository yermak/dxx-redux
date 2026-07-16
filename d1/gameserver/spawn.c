#include <stdio.h>
#include <string.h>
#include "spawn.h"

#ifdef _WIN32
#include <windows.h>

int gs_spawn(gs_proc *out, char *const argv[])
{
	char cmdline[2048] = "";
	int i;
	STARTUPINFOA si;
	PROCESS_INFORMATION pi;

	for (i = 0; argv[i]; i++) {
		if (i) strncat(cmdline, " ", sizeof(cmdline) - strlen(cmdline) - 1);
		strncat(cmdline, "\"", sizeof(cmdline) - strlen(cmdline) - 1);
		strncat(cmdline, argv[i], sizeof(cmdline) - strlen(cmdline) - 1);
		strncat(cmdline, "\"", sizeof(cmdline) - strlen(cmdline) - 1);
	}
	memset(&si, 0, sizeof(si));
	si.cb = sizeof(si);
	memset(&pi, 0, sizeof(pi));
	if (!CreateProcessA(argv[0], cmdline, NULL, NULL, FALSE,
	                    CREATE_NEW_PROCESS_GROUP, NULL, NULL, &si, &pi))
		return -1;
	CloseHandle(pi.hThread);
	out->handle = pi.hProcess;
	out->pid = pi.dwProcessId;
	return 0;
}

int gs_proc_running(gs_proc *p)
{
	DWORD code;
	if (!p->handle)
		return 0;
	if (!GetExitCodeProcess(p->handle, &code))
		return 0;
	return code == STILL_ACTIVE;
}

void gs_proc_kill(gs_proc *p)
{
	if (p->handle)
		TerminateProcess(p->handle, 1);
}

void gs_proc_close(gs_proc *p)
{
	if (p->handle)
		CloseHandle(p->handle);
	p->handle = NULL;
	p->pid = 0;
}

#else /* POSIX */
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

int gs_spawn(gs_proc *out, char *const argv[])
{
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		execv(argv[0], argv);
		_exit(127);
	}
	out->pid = pid;
	return 0;
}

int gs_proc_running(gs_proc *p)
{
	int status;
	pid_t r;
	if (p->pid <= 0)
		return 0;
	r = waitpid(p->pid, &status, WNOHANG);
	if (r == 0)
		return 1;
	p->pid = -p->pid; /* remember reaped, gs_proc_close finishes cleanup */
	return 0;
}

void gs_proc_kill(gs_proc *p)
{
	if (p->pid > 0)
		kill(p->pid, SIGTERM);
}

void gs_proc_close(gs_proc *p)
{
	int status;
	if (p->pid > 0)
		waitpid(p->pid, &status, 0);
	p->pid = 0;
}
#endif
