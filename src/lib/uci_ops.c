/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * UCI access for mesh11sd. Wraps the uci CLI so behaviour matches v7
 * (uncommitted set/batch by default). Swap the backend later without
 * touching callers. Never logs option values that may hold secrets.
 */
#include "mesh11sd.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run_capture(char *const argv[], char *out, size_t outlen)
{
	int pipefd[2];
	pid_t pid;
	ssize_t n;
	size_t total = 0;
	int status;

	if (pipe(pipefd) < 0)
		return -1;

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}

	if (pid == 0) {
		close(pipefd[0]);
		if (dup2(pipefd[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(pipefd[1]);
		dup2(STDOUT_FILENO, STDERR_FILENO);
		execvp(argv[0], argv);
		_exit(127);
	}

	close(pipefd[1]);
	if (out && outlen) {
		out[0] = '\0';
		while (total + 1 < outlen &&
		       (n = read(pipefd[0], out + total, outlen - 1 - total)) > 0)
			total += (size_t)n;
		out[total] = '\0';
		while (total > 0 && (out[total - 1] == '\n' || out[total - 1] == '\r'))
			out[--total] = '\0';
	} else {
		char sink[256];
		while (read(pipefd[0], sink, sizeof(sink)) > 0)
			;
	}
	close(pipefd[0]);

	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

int mesh_uci_get(const char *tuple, char *buf, size_t buflen)
{
	char *argv[] = { "uci", "-q", "get", (char *)tuple, NULL };

	if (!tuple || !buf || buflen == 0)
		return -1;
	buf[0] = '\0';
	return run_capture(argv, buf, buflen);
}

int mesh_uci_set(const char *tuple, const char *value)
{
	char assignment[512];
	char *argv[] = { "uci", "set", assignment, NULL };

	if (!tuple || !value)
		return -1;
	if (snprintf(assignment, sizeof(assignment), "%s=%s", tuple, value) >=
	    (int)sizeof(assignment))
		return -1;
	return run_capture(argv, NULL, 0);
}

int mesh_uci_commit(const char *package)
{
	char *argv_pkg[] = { "uci", "commit", (char *)package, NULL };
	char *argv_all[] = { "uci", "commit", NULL };

	if (package && package[0])
		return run_capture(argv_pkg, NULL, 0);
	return run_capture(argv_all, NULL, 0);
}

int mesh_uci_revert(const char *package)
{
	char *argv_pkg[] = { "uci", "revert", (char *)package, NULL };
	char *argv_all[] = { "uci", "revert", NULL };

	if (package && package[0])
		return run_capture(argv_pkg, NULL, 0);
	return run_capture(argv_all, NULL, 0);
}

int mesh_uci_changes(const char *package, char *buf, size_t buflen)
{
	char *argv_pkg[] = { "uci", "changes", (char *)package, NULL };
	char *argv_all[] = { "uci", "changes", NULL };

	if (!buf || buflen == 0)
		return -1;
	if (package && package[0])
		return run_capture(argv_pkg, buf, buflen);
	return run_capture(argv_all, buf, buflen);
}
