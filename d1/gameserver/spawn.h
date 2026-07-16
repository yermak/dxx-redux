/* Minimal cross-platform child-process shim for the broker. */
#ifndef GS_SPAWN_H
#define GS_SPAWN_H

typedef struct gs_proc {
#ifdef _WIN32
	void *handle;               /* HANDLE, NULL when slot free */
	unsigned long pid;
#else
	int pid;                    /* >0 running, 0 free, <0 exited+reaped (awaiting gs_proc_close) */
#endif
} gs_proc;

/* argv is NULL-terminated, argv[0] = executable path.
 * Returns 0 on success. */
int gs_spawn(gs_proc *out, char *const argv[]);
int gs_proc_running(gs_proc *p);   /* 1 running, 0 exited/free */
void gs_proc_kill(gs_proc *p);     /* forceful terminate */
void gs_proc_close(gs_proc *p);    /* reap zombie / close handle, mark free */

#endif
