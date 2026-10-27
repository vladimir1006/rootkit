#ifndef RK2600_PROTOCOL_H
#define RK2600_PROTOCOL_H

/*
 * Shared userspace/kernel ABI for the rk2600 module.
 * Append new commands; never change existing ioctl numbers.
 */

#include <linux/types.h>

#ifdef __KERNEL__
#include <linux/ioctl.h>
#else
#include <sys/ioctl.h>
#endif

#define RK2600_API_VERSION  2U

/* Control device — dot-prefix hides it from casual ls */
#define RK2600_DEVICE_NAME  ".rk_ctl"
#define RK2600_DEVICE_PATH  "/dev/" RK2600_DEVICE_NAME

/* Proc interface — hidden by proc filldir hook */
#define RK2600_PROC_NAME    ".rk"
#define RK2600_PROC_PATH    "/proc/" RK2600_PROC_NAME

/* Persistence via /etc/local.d (Alpine openrc) */
#define RK2600_PERSIST_NAME "snd-compat.start"
#define RK2600_PERSIST_DIR  "/etc/local.d/"
#define RK2600_PERSIST_PATH RK2600_PERSIST_DIR RK2600_PERSIST_NAME
#define RK2600_MODULE_NAME  "rk2600"
#define RK2600_MODULE_FILE  RK2600_MODULE_NAME ".ko"

/* ioctl transport */
#define RK2600_IOCTL_MAGIC  0xA6
#define RK2600_NAME_MAX     255U

/* Status flags */
#define RK2600_FLAG_HIDDEN  (1U << 0)

struct rk2600_status {
	__u32 api_version;
	__u32 flags;
};

struct rk2600_name_request {
	char name[RK2600_NAME_MAX + 1U];
};

#define RK2600_IOC_ELEVATE  _IO(RK2600_IOCTL_MAGIC, 0x00)
#define RK2600_IOC_STATUS   _IOR(RK2600_IOCTL_MAGIC, 0x01, struct rk2600_status)
#define RK2600_IOC_HIDE     _IOW(RK2600_IOCTL_MAGIC, 0x02, struct rk2600_name_request)
#define RK2600_IOC_UNHIDE   _IOW(RK2600_IOCTL_MAGIC, 0x03, struct rk2600_name_request)
#define RK2600_IOC_MODHIDE  _IO(RK2600_IOCTL_MAGIC, 0x04)
#define RK2600_IOC_MODSHOW  _IO(RK2600_IOCTL_MAGIC, 0x05)

#endif /* RK2600_PROTOCOL_H */
