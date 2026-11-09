/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include "mesh11sd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mesh_uci_get(const char *tuple, char *buf, size_t buflen)
{
	char *argv[] = { "uci", "-q", "get", (char *)tuple, NULL };

	if (!tuple || !buf || buflen == 0)
		return -1;
	buf[0] = '\0';
	return mesh_run_capture(argv, buf, buflen);
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
	return mesh_run_capture(argv, NULL, 0);
}

int mesh_uci_batch_line(const char *line)
{
	char *argv[] = { "uci", "batch", NULL };
	int pipefd[2];
	pid_t pid;
	int status;

	if (!line)
		return -1;
	if (pipe(pipefd) < 0)
		return -1;
	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		return -1;
	}
	if (pid == 0) {
		close(pipefd[1]);
		dup2(pipefd[0], STDIN_FILENO);
		close(pipefd[0]);
		int dn = open("/dev/null", O_WRONLY);
		if (dn >= 0) {
			dup2(dn, STDOUT_FILENO);
			dup2(dn, STDERR_FILENO);
			close(dn);
		}
		execvp("uci", argv);
		_exit(127);
	}
	close(pipefd[0]);
	dprintf(pipefd[1], "%s\n", line);
	close(pipefd[1]);
	if (waitpid(pid, &status, 0) < 0)
		return -1;
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		return -1;
	return 0;
}

int mesh_uci_commit(const char *package)
{
	char *argv_pkg[] = { "uci", "commit", (char *)package, NULL };
	char *argv_all[] = { "uci", "commit", NULL };

	if (package && package[0])
		return mesh_run_capture(argv_pkg, NULL, 0);
	return mesh_run_capture(argv_all, NULL, 0);
}

int mesh_uci_revert(const char *package)
{
	char *argv_pkg[] = { "uci", "revert", (char *)package, NULL };
	char *argv_all[] = { "uci", "revert", NULL };

	if (package && package[0])
		return mesh_run_capture(argv_pkg, NULL, 0);
	return mesh_run_capture(argv_all, NULL, 0);
}

int mesh_uci_changes(const char *package, char *buf, size_t buflen)
{
	char *argv_pkg[] = { "uci", "changes", (char *)package, NULL };
	char *argv_all[] = { "uci", "changes", NULL };

	if (!buf || buflen == 0)
		return -1;
	if (package && package[0])
		return mesh_run_capture(argv_pkg, buf, buflen);
	return mesh_run_capture(argv_all, buf, buflen);
}

int mesh_uci_show_filter(const char *package, const char *grep_pat,
			 char *buf, size_t buflen)
{
	char out[8192];
	char *argv[] = { "uci", "show", (char *)package, NULL };
	char *p, *line;

	if (!package || !buf || buflen == 0)
		return -1;
	buf[0] = '\0';
	if (mesh_run_capture(argv, out, sizeof(out)) != 0)
		return -1;
	if (!grep_pat) {
		snprintf(buf, buflen, "%s", out);
		return 0;
	}
	p = out;
	while ((line = strsep(&p, "\n")) != NULL) {
		if (strstr(line, grep_pat)) {
			size_t used = strlen(buf);
			snprintf(buf + used, buflen - used, "%s%s",
				 used ? " " : "", line);
		}
	}
	return 0;
}
