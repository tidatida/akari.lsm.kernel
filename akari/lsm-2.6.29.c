/*
 * lsm.c
 *
 * Copyright (C) 2010-2015  Tetsuo Handa <penguin-kernel@I-love.SAKURA.ne.jp>
 *
 * Version: 1.0.37   2017/09/17
 */

#include "internal.h"
#include "probe.h"
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0)
#define USE_UMODE_T
#else
#include "check_umode_t.h"
#endif

/* Prototype definition. */
static void ccs_task_security_gc(void);
static int ccs_copy_cred_security(const struct cred *new,
				  const struct cred *old, gfp_t gfp);
static struct ccs_security *ccs_find_cred_security(const struct cred *cred);
static DEFINE_SPINLOCK(ccs_task_security_list_lock);
static atomic_t ccs_in_execve_tasks = ATOMIC_INIT(0);
/*
 * List of "struct ccs_security" for "struct pid".
 *
 * All instances on this list is guaranteed that "struct ccs_security"->pid !=
 * NULL. Also, instances on this list that are in execve() are guaranteed that
 * "struct ccs_security"->cred remembers "struct linux_binprm"->cred with a
 * refcount on "struct linux_binprm"->cred.
 */
struct list_head ccs_task_security_list[CCS_MAX_TASK_SECURITY_HASH];
/*
 * List of "struct ccs_security" for "struct cred".
 *
 * Since the number of "struct cred" is nearly equals to the number of
 * "struct pid", we allocate hash tables like ccs_task_security_list.
 *
 * All instances on this list are guaranteed that "struct ccs_security"->pid ==
 * NULL and "struct ccs_security"->cred != NULL.
 */
static struct list_head ccs_cred_security_list[CCS_MAX_TASK_SECURITY_HASH];

/* Dummy security context for avoiding NULL pointer dereference. */
static struct ccs_security ccs_oom_security = {
	.ccs_domain_info = &ccs_kernel_domain
};

/* Dummy security context for avoiding NULL pointer dereference. */
static struct ccs_security ccs_default_security = {
	.ccs_domain_info = &ccs_kernel_domain
};

/* For exporting variables and functions. */
struct ccsecurity_exports ccsecurity_exports;
/* Members are updated by loadable kernel module. */
struct ccsecurity_operations ccsecurity_ops;

/* Function pointers originally registered by register_security(). */
static struct security_operations original_security_ops /* = *security_ops; */;

#ifdef CONFIG_AKARI_TRACE_EXECVE_COUNT

/**
 * ccs_update_ee_counter - Update "struct ccs_execve" counter.
 *
 * @count: Count to increment or decrement.
 *
 * Returns updated counter.
 */
static unsigned int ccs_update_ee_counter(int count)
{
	/* Debug counter for detecting "struct ccs_execve" memory leak. */
	static atomic_t ccs_ee_counter = ATOMIC_INIT(0);
	return atomic_add_return(count, &ccs_ee_counter);
}

/**
 * ccs_audit_alloc_execve - Audit allocation of "struct ccs_execve".
 *
 * @ee: Pointer to "struct ccs_execve".
 *
 * Returns nothing.
 */
void ccs_audit_alloc_execve(const struct ccs_execve * const ee)
{
	printk(KERN_INFO "AKARI: Allocated %p by pid=%u (count=%u)\n", ee,
	       current->pid, ccs_update_ee_counter(1) - 1);
}

/**
 * ccs_audit_free_execve - Audit release of "struct ccs_execve".
 *
 * @ee:   Pointer to "struct ccs_execve".
 * @task: True if released by current task, false otherwise.
 *
 * Returns nothing.
 */
void ccs_audit_free_execve(const struct ccs_execve * const ee,
			   const bool is_current)
{
	const unsigned int tmp = ccs_update_ee_counter(-1);
	if (is_current)
		printk(KERN_INFO "AKARI: Releasing %p by pid=%u (count=%u)\n",
		       ee, current->pid, tmp);
	else
		printk(KERN_INFO "AKARI: Releasing %p by kernel (count=%u)\n",
		       ee, tmp);
}

#endif

#if !defined(CONFIG_AKARI_DEBUG)
#define ccs_debug_trace(pos) do { } while (0)
#else
#define ccs_debug_trace(pos)						\
	do {								\
		static bool done;					\
		if (!done) {						\
			printk(KERN_INFO				\
			       "AKARI: Debug trace: " pos " of 4\n");	\
			done = true;					\
		}							\
	} while (0)
#endif

/**
 * ccs_clear_execve - Release memory used by do_execve().
 *
 * @ret:      0 if do_execve() succeeded, negative value otherwise.
 * @security: Pointer to "struct ccs_security".
 *
 * Returns nothing.
 */
static void ccs_clear_execve(int ret, struct ccs_security *security)
{
	struct ccs_execve *ee;
	if (security == &ccs_default_security || security == &ccs_oom_security)
		return;
	ee = security->ee;
	security->ee = NULL;
	if (!ee)
		return;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31)
	/*
	 * Drop refcount on "struct cred" in "struct linux_binprm" and forget
	 * it.
	 */
	put_cred(security->cred);
	security->cred = NULL;
#endif
	atomic_dec(&ccs_in_execve_tasks);
	ccs_finish_execve(ret, ee);
}

/**
 * ccs_rcu_free - RCU callback for releasing "struct ccs_security".
 *
 * @rcu: Pointer to "struct rcu_head".
 *
 * Returns nothing.
 */
static void ccs_rcu_free(struct rcu_head *rcu)
{
	struct ccs_security *ptr = container_of(rcu, typeof(*ptr), rcu);
	struct ccs_execve *ee = ptr->ee;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 31)
	/*
	 * If this security context was associated with "struct pid" and
	 * ptr->ccs_flags has CCS_TASK_IS_IN_EXECVE set, it indicates that a
	 * "struct task_struct" associated with this security context exited
	 * immediately after do_execve() has failed.
	 */
	if (ptr->pid && (ptr->ccs_flags & CCS_TASK_IS_IN_EXECVE)) {
		ccs_debug_trace("1");
		atomic_dec(&ccs_in_execve_tasks);
	}
#else
	/*
	 * If this security context was associated with "struct pid" and
	 * remembers "struct cred" in "struct linux_binprm", it indicates that
	 * a "struct task_struct" associated with this security context exited
	 * immediately after do_execve() has failed.
	 */
	if (ptr->pid && ptr->cred) {
		ccs_debug_trace("1");
		put_cred(ptr->cred);
		atomic_dec(&ccs_in_execve_tasks);
	}
#endif
	/*
	 * If this security context was associated with "struct pid",
	 * drop refcount obtained by get_pid() in ccs_find_task_security().
	 */
	if (ptr->pid) {
		ccs_debug_trace("2");
		put_pid(ptr->pid);
	}
	if (ee) {
		ccs_debug_trace("3");
		ccs_audit_free_execve(ee, false);
		kfree(ee->handler_path);
		kfree(ee);
	}
	kfree(ptr);
}

/**
 * ccs_del_security - Release "struct ccs_security".
 *
 * @ptr: Pointer to "struct ccs_security".
 *
 * Returns nothing.
 */
static void ccs_del_security(struct ccs_security *ptr)
{
	unsigned long flags;
	if (ptr == &ccs_default_security || ptr == &ccs_oom_security)
		return;
	spin_lock_irqsave(&ccs_task_security_list_lock, flags);
	list_del_rcu(&ptr->list);
	spin_unlock_irqrestore(&ccs_task_security_list_lock, flags);
	call_rcu(&ptr->rcu, ccs_rcu_free);
}

/**
 * ccs_add_cred_security - Add "struct ccs_security" to list.
 *
 * @ptr: Pointer to "struct ccs_security".
 *
 * Returns nothing.
 */
static void ccs_add_cred_security(struct ccs_security *ptr)
{
	unsigned long flags;
	struct list_head *list = &ccs_cred_security_list
		[hash_ptr((void *) ptr->cred, CCS_TASK_SECURITY_HASH_BITS)];
#ifdef CONFIG_AKARI_DEBUG
	if (ptr->pid)
		printk(KERN_INFO "AKARI: \"struct ccs_security\"->pid != NULL"
		       "\n");
#endif
	ptr->pid = NULL;
	spin_lock_irqsave(&ccs_task_security_list_lock, flags);
	list_add_rcu(&ptr->list, list);
	spin_unlock_irqrestore(&ccs_task_security_list_lock, flags);
}

/**
 * ccs_task_create - Make snapshot of security context for new task.
 *
 * @clone_flags: Flags passed to clone().
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_task_create(unsigned long clone_flags)
{
	int rc;
	struct ccs_security *old_security;
	struct ccs_security *new_security;
	struct cred *cred = prepare_creds();
	if (!cred)
		return -ENOMEM;
	while (!original_security_ops.task_create)
		smp_rmb();
	rc = original_security_ops.task_create(clone_flags);
	if (rc) {
		abort_creds(cred);
		return rc;
	}
	old_security = ccs_find_task_security(current);
	new_security = ccs_find_cred_security(cred);
	new_security->ccs_domain_info = old_security->ccs_domain_info;
	new_security->ccs_flags = old_security->ccs_flags;
	return commit_creds(cred);
}

/**
 * ccs_cred_prepare - Allocate memory for new credentials.
 *
 * @new: Pointer to "struct cred".
 * @old: Pointer to "struct cred".
 * @gfp: Memory allocation flags.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_cred_prepare(struct cred *new, const struct cred *old,
			    gfp_t gfp)
{
	int rc;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 31)
	/*
	 * For checking whether reverting domain transition is needed or not.
	 *
	 * See ccs_find_task_security() for reason.
	 */
	if (gfp == GFP_KERNEL)
		ccs_find_task_security(current);
#endif
	rc = ccs_copy_cred_security(new, old, gfp);
	if (rc)
		return rc;
	if (gfp == GFP_KERNEL)
		ccs_task_security_gc();
	while (!original_security_ops.cred_prepare)
		smp_rmb();
	rc = original_security_ops.cred_prepare(new, old, gfp);
	if (rc)
		ccs_del_security(ccs_find_cred_security(new));
	return rc;
}

/**
 * ccs_cred_free - Release memory used by credentials.
 *
 * @cred: Pointer to "struct cred".
 *
 * Returns nothing.
 */
static void ccs_cred_free(struct cred *cred)
{
	while (!original_security_ops.cred_free)
		smp_rmb();
	original_security_ops.cred_free(cred);
	ccs_del_security(ccs_find_cred_security(cred));
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)

/**
 * ccs_alloc_cred_security - Allocate memory for new credentials.
 *
 * @cred: Pointer to "struct cred".
 * @gfp:  Memory allocation flags.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_alloc_cred_security(const struct cred *cred, gfp_t gfp)
{
	struct ccs_security *new_security = kzalloc(sizeof(*new_security),
						    gfp);
	if (!new_security)
		return -ENOMEM;
	new_security->cred = cred;
	ccs_add_cred_security(new_security);
	return 0;
}

/**
 * ccs_cred_alloc_blank - Allocate memory for new credentials.
 *
 * @new: Pointer to "struct cred".
 * @gfp: Memory allocation flags.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_cred_alloc_blank(struct cred *new, gfp_t gfp)
{
	int rc = ccs_alloc_cred_security(new, gfp);
	if (rc)
		return rc;
	while (!original_security_ops.cred_alloc_blank)
		smp_rmb();
	rc = original_security_ops.cred_alloc_blank(new, gfp);
	if (rc)
		ccs_del_security(ccs_find_cred_security(new));
	return rc;
}

/**
 * ccs_cred_transfer - Transfer "struct ccs_security" between credentials.
 *
 * @new: Pointer to "struct cred".
 * @old: Pointer to "struct cred".
 *
 * Returns nothing.
 */
static void ccs_cred_transfer(struct cred *new, const struct cred *old)
{
	struct ccs_security *new_security;
	struct ccs_security *old_security;
	while (!original_security_ops.cred_transfer)
		smp_rmb();
	original_security_ops.cred_transfer(new, old);
	new_security = ccs_find_cred_security(new);
	old_security = ccs_find_cred_security(old);
	if (new_security == &ccs_default_security ||
	    new_security == &ccs_oom_security ||
	    old_security == &ccs_default_security ||
	    old_security == &ccs_oom_security)
		return;
	new_security->ccs_flags = old_security->ccs_flags;
	new_security->ccs_domain_info = old_security->ccs_domain_info;
}

#endif

/**
 * ccs_bprm_committing_creds - A hook which is called when do_execve() succeeded.
 *
 * @bprm: Pointer to "struct linux_binprm".
 *
 * Returns nothing.
 */
static void ccs_bprm_committing_creds(struct linux_binprm *bprm)
{
	struct ccs_security *old_security;
	struct ccs_security *new_security;
	while (!original_security_ops.bprm_committing_creds)
		smp_rmb();
	original_security_ops.bprm_committing_creds(bprm);
	old_security = ccs_current_security();
	if (old_security == &ccs_default_security ||
	    old_security == &ccs_oom_security)
		return;
	ccs_clear_execve(0, old_security);
	/* Update current task's cred's domain for future fork(). */
	new_security = ccs_find_cred_security(bprm->cred);
	new_security->ccs_flags = old_security->ccs_flags;
	new_security->ccs_domain_info = old_security->ccs_domain_info;
}

/**
 * ccs_bprm_check_security - Check permission for execve().
 *
 * @bprm: Pointer to "struct linux_binprm".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_bprm_check_security(struct linux_binprm *bprm)
{
	struct ccs_security *security = ccs_current_security();
	if (security == &ccs_default_security || security == &ccs_oom_security)
		return -ENOMEM;
	if (!security->ee) {
		int rc;
#ifndef CONFIG_CCSECURITY_OMIT_USERSPACE_LOADER
		if (!ccs_policy_loaded)
			ccs_load_policy(bprm->filename);
#endif
		rc = ccs_start_execve(bprm, &security->ee);
		if (security->ee) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 31)
			/*
			 * Get refcount on "struct cred" in
			 * "struct linux_binprm" and remember it.
			 */
			get_cred(bprm->cred);
			security->cred = bprm->cred;
#endif
			atomic_inc(&ccs_in_execve_tasks);
		}
		if (rc)
			return rc;
	}
	while (!original_security_ops.bprm_check_security)
		smp_rmb();
	return original_security_ops.bprm_check_security(bprm);
}

/**
 * ccs_open - Check permission for open().
 *
 * @f: Pointer to "struct file".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_open(struct file *f)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
	return ccs_open_permission(f);
#elif defined(RHEL_MAJOR) && RHEL_MAJOR == 6
	return ccs_open_permission(f->f_path.dentry, f->f_path.mnt,
				   f->f_flags);
#else
	return ccs_open_permission(f->f_path.dentry, f->f_path.mnt,
				   f->f_flags + 1);
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)

/**
 * ccs_file_open - Check permission for open().
 *
 * @f:    Pointer to "struct file".
 * @cred: Pointer to "struct cred".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_file_open(struct file *f, const struct cred *cred)
{
	int rc = ccs_open(f);
	if (rc)
		return rc;
	while (!original_security_ops.file_open)
		smp_rmb();
	return original_security_ops.file_open(f, cred);
}

#else

/**
 * ccs_dentry_open - Check permission for open().
 *
 * @f:    Pointer to "struct file".
 * @cred: Pointer to "struct cred".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_dentry_open(struct file *f, const struct cred *cred)
{
	int rc = ccs_open(f);
	if (rc)
		return rc;
	while (!original_security_ops.dentry_open)
		smp_rmb();
	return original_security_ops.dentry_open(f, cred);
}

#endif

#if defined(CONFIG_SECURITY_PATH)

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)

/**
 * ccs_path_chown - Check permission for chown()/chgrp().
 *
 * @path:  Pointer to "struct path".
 * @user:  User ID.
 * @group: Group ID.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_chown(struct path *path, kuid_t user, kgid_t group)
{
	int rc = ccs_chown_permission(path->dentry, path->mnt, user, group);
	if (rc)
		return rc;
	while (!original_security_ops.path_chown)
		smp_rmb();
	return original_security_ops.path_chown(path, user, group);
}

/**
 * ccs_path_chmod - Check permission for chmod().
 *
 * @path: Pointer to "struct path".
 * @mode: Mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_chmod(struct path *path, umode_t mode)
{
	int rc = ccs_chmod_permission(path->dentry, path->mnt, mode);
	if (rc)
		return rc;
	while (!original_security_ops.path_chmod)
		smp_rmb();
	return original_security_ops.path_chmod(path, mode);
}

/**
 * ccs_path_chroot - Check permission for chroot().
 *
 * @path: Pointer to "struct path".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_chroot(struct path *path)
{
	int rc = ccs_chroot_permission(path);
	if (rc)
		return rc;
	while (!original_security_ops.path_chroot)
		smp_rmb();
	return original_security_ops.path_chroot(path);
}

#elif LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)

/**
 * ccs_path_chown - Check permission for chown()/chgrp().
 *
 * @path:  Pointer to "struct path".
 * @user:  User ID.
 * @group: Group ID.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_chown(struct path *path, uid_t user, gid_t group)
{
	int rc = ccs_chown_permission(path->dentry, path->mnt, user, group);
	if (rc)
		return rc;
	while (!original_security_ops.path_chown)
		smp_rmb();
	return original_security_ops.path_chown(path, user, group);
}

#if defined(USE_UMODE_T)

/**
 * ccs_path_chmod - Check permission for chmod().
 *
 * @path: Pointer to "struct path".
 * @mode: Mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_chmod(struct path *path, umode_t mode)
{
	int rc = ccs_chmod_permission(path->dentry, path->mnt, mode);
	if (rc)
		return rc;
	while (!original_security_ops.path_chmod)
		smp_rmb();
	return original_security_ops.path_chmod(path, mode);
}

#else

/**
 * ccs_path_chmod - Check permission for chmod().
 *
 * @dentry: Pointer to "struct dentry".
 * @vfsmnt: Pointer to "struct vfsmount".
 * @mode:   Mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_chmod(struct dentry *dentry, struct vfsmount *vfsmnt,
			  mode_t mode)
{
	int rc = ccs_chmod_permission(dentry, vfsmnt, mode);
	if (rc)
		return rc;
	while (!original_security_ops.path_chmod)
		smp_rmb();
	return original_security_ops.path_chmod(dentry, vfsmnt, mode);
}

#endif

/**
 * ccs_path_chroot - Check permission for chroot().
 *
 * @path: Pointer to "struct path".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_chroot(struct path *path)
{
	int rc = ccs_chroot_permission(path);
	if (rc)
		return rc;
	while (!original_security_ops.path_chroot)
		smp_rmb();
	return original_security_ops.path_chroot(path);
}

#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 36)

/**
 * ccs_path_truncate - Check permission for truncate().
 *
 * @path: Pointer to "struct path".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_truncate(struct path *path)
{
	int rc = ccs_truncate_permission(path->dentry, path->mnt);
	if (rc)
		return rc;
	while (!original_security_ops.path_truncate)
		smp_rmb();
	return original_security_ops.path_truncate(path);
}

#else

/**
 * ccs_path_truncate - Check permission for truncate().
 *
 * @path:       Pointer to "struct path".
 * @length:     New length.
 * @time_attrs: New time attributes.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_truncate(struct path *path, loff_t length,
			     unsigned int time_attrs)
{
	int rc = ccs_truncate_permission(path->dentry, path->mnt);
	if (rc)
		return rc;
	while (!original_security_ops.path_truncate)
		smp_rmb();
	return original_security_ops.path_truncate(path, length, time_attrs);
}

#endif

#endif

/**
 * ccs_inode_setattr - Check permission for chown()/chgrp()/chmod()/truncate().
 *
 * @dentry: Pointer to "struct dentry".
 * @attr:   Pointer to "struct iattr".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_setattr(struct dentry *dentry, struct iattr *attr)
{
	int rc = 0;
#if !defined(CONFIG_SECURITY_PATH) || LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33)
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	if (attr->ia_valid & ATTR_UID)
		rc = ccs_chown_permission(dentry, NULL, attr->ia_uid,
					  INVALID_GID);
	if (!rc && (attr->ia_valid & ATTR_GID))
		rc = ccs_chown_permission(dentry, NULL, INVALID_UID,
					  attr->ia_gid);
#else
	if (attr->ia_valid & ATTR_UID)
		rc = ccs_chown_permission(dentry, NULL, attr->ia_uid, -1);
	if (!rc && (attr->ia_valid & ATTR_GID))
		rc = ccs_chown_permission(dentry, NULL, -1, attr->ia_gid);
#endif
	if (!rc && (attr->ia_valid & ATTR_MODE))
		rc = ccs_chmod_permission(dentry, NULL, attr->ia_mode);
#endif
#if !defined(CONFIG_SECURITY_PATH)
	if (!rc && (attr->ia_valid & ATTR_SIZE))
		rc = ccs_truncate_permission(dentry, NULL);
#endif
	if (rc)
		return rc;
	while (!original_security_ops.inode_setattr)
		smp_rmb();
	return original_security_ops.inode_setattr(dentry, attr);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 1, 0)

/**
 * ccs_inode_getattr - Check permission for stat().
 *
 * @path: Pointer to "struct path".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_getattr(const struct path *path)
{
	int rc = ccs_getattr_permission(path->mnt, path->dentry);
	if (rc)
		return rc;
	while (!original_security_ops.inode_getattr)
		smp_rmb();
	return original_security_ops.inode_getattr(path);
}

#else

/**
 * ccs_inode_getattr - Check permission for stat().
 *
 * @mnt:    Pointer to "struct vfsmount".
 * @dentry: Pointer to "struct dentry".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_getattr(struct vfsmount *mnt, struct dentry *dentry)
{
	int rc = ccs_getattr_permission(mnt, dentry);
	if (rc)
		return rc;
	while (!original_security_ops.inode_getattr)
		smp_rmb();
	return original_security_ops.inode_getattr(mnt, dentry);
}

#endif

#if defined(CONFIG_SECURITY_PATH)

#if defined(USE_UMODE_T)

/**
 * ccs_path_mknod - Check permission for mknod().
 *
 * @dir:    Pointer to "struct path".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 * @dev:    Device major/minor number.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_mknod(struct path *dir, struct dentry *dentry,
			  umode_t mode, unsigned int dev)
{
	int rc = ccs_mknod_permission(dentry, dir->mnt, mode, dev);
	if (rc)
		return rc;
	while (!original_security_ops.path_mknod)
		smp_rmb();
	return original_security_ops.path_mknod(dir, dentry, mode, dev);
}

/**
 * ccs_path_mkdir - Check permission for mkdir().
 *
 * @dir:    Pointer to "struct path".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_mkdir(struct path *dir, struct dentry *dentry,
			  umode_t mode)
{
	int rc = ccs_mkdir_permission(dentry, dir->mnt, mode);
	if (rc)
		return rc;
	while (!original_security_ops.path_mkdir)
		smp_rmb();
	return original_security_ops.path_mkdir(dir, dentry, mode);
}

#else

/**
 * ccs_path_mknod - Check permission for mknod().
 *
 * @dir:    Pointer to "struct path".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 * @dev:    Device major/minor number.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_mknod(struct path *dir, struct dentry *dentry, int mode,
			  unsigned int dev)
{
	int rc = ccs_mknod_permission(dentry, dir->mnt, mode, dev);
	if (rc)
		return rc;
	while (!original_security_ops.path_mknod)
		smp_rmb();
	return original_security_ops.path_mknod(dir, dentry, mode, dev);
}

/**
 * ccs_path_mkdir - Check permission for mkdir().
 *
 * @dir:    Pointer to "struct path".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_mkdir(struct path *dir, struct dentry *dentry, int mode)
{
	int rc = ccs_mkdir_permission(dentry, dir->mnt, mode);
	if (rc)
		return rc;
	while (!original_security_ops.path_mkdir)
		smp_rmb();
	return original_security_ops.path_mkdir(dir, dentry, mode);
}

#endif

/**
 * ccs_path_rmdir - Check permission for rmdir().
 *
 * @dir:    Pointer to "struct path".
 * @dentry: Pointer to "struct dentry".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_rmdir(struct path *dir, struct dentry *dentry)
{
	int rc = ccs_rmdir_permission(dentry, dir->mnt);
	if (rc)
		return rc;
	while (!original_security_ops.path_rmdir)
		smp_rmb();
	return original_security_ops.path_rmdir(dir, dentry);
}

/**
 * ccs_path_unlink - Check permission for unlink().
 *
 * @dir:    Pointer to "struct path".
 * @dentry: Pointer to "struct dentry".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_unlink(struct path *dir, struct dentry *dentry)
{
	int rc = ccs_unlink_permission(dentry, dir->mnt);
	if (rc)
		return rc;
	while (!original_security_ops.path_unlink)
		smp_rmb();
	return original_security_ops.path_unlink(dir, dentry);
}

/**
 * ccs_path_symlink - Check permission for symlink().
 *
 * @dir:      Pointer to "struct path".
 * @dentry:   Pointer to "struct dentry".
 * @old_name: Content of symbolic link.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_symlink(struct path *dir, struct dentry *dentry,
			    const char *old_name)
{
	int rc = ccs_symlink_permission(dentry, dir->mnt, old_name);
	if (rc)
		return rc;
	while (!original_security_ops.path_symlink)
		smp_rmb();
	return original_security_ops.path_symlink(dir, dentry, old_name);
}

/**
 * ccs_path_rename - Check permission for rename().
 *
 * @old_dir:    Pointer to "struct path".
 * @old_dentry: Pointer to "struct dentry".
 * @new_dir:    Pointer to "struct path".
 * @new_dentry: Pointer to "struct dentry".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_rename(struct path *old_dir, struct dentry *old_dentry,
			   struct path *new_dir, struct dentry *new_dentry)
{
	int rc = ccs_rename_permission(old_dentry, new_dentry, old_dir->mnt);
	if (rc)
		return rc;
	while (!original_security_ops.path_rename)
		smp_rmb();
	return original_security_ops.path_rename(old_dir, old_dentry, new_dir,
						 new_dentry);
}

/**
 * ccs_path_link - Check permission for link().
 *
 * @old_dentry: Pointer to "struct dentry".
 * @new_dir:    Pointer to "struct path".
 * @new_dentry: Pointer to "struct dentry".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_path_link(struct dentry *old_dentry, struct path *new_dir,
			 struct dentry *new_dentry)
{
	int rc = ccs_link_permission(old_dentry, new_dentry, new_dir->mnt);
	if (rc)
		return rc;
	while (!original_security_ops.path_link)
		smp_rmb();
	return original_security_ops.path_link(old_dentry, new_dir,
					       new_dentry);
}

#else

#if defined(USE_UMODE_T)

/**
 * ccs_inode_mknod - Check permission for mknod().
 *
 * @dir:    Pointer to "struct inode".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 * @dev:    Device major/minor number.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_mknod(struct inode *dir, struct dentry *dentry,
			   umode_t mode, dev_t dev)
{
	int rc = ccs_mknod_permission(dentry, NULL, mode, dev);
	if (rc)
		return rc;
	while (!original_security_ops.inode_mknod)
		smp_rmb();
	return original_security_ops.inode_mknod(dir, dentry, mode, dev);
}

/**
 * ccs_inode_mkdir - Check permission for mkdir().
 *
 * @dir:    Pointer to "struct inode".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_mkdir(struct inode *dir, struct dentry *dentry,
			   umode_t mode)
{
	int rc = ccs_mkdir_permission(dentry, NULL, mode);
	if (rc)
		return rc;
	while (!original_security_ops.inode_mkdir)
		smp_rmb();
	return original_security_ops.inode_mkdir(dir, dentry, mode);
}

#else

/**
 * ccs_inode_mknod - Check permission for mknod().
 *
 * @dir:    Pointer to "struct inode".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 * @dev:    Device major/minor number.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_mknod(struct inode *dir, struct dentry *dentry, int mode,
			   dev_t dev)
{
	int rc = ccs_mknod_permission(dentry, NULL, mode, dev);
	if (rc)
		return rc;
	while (!original_security_ops.inode_mknod)
		smp_rmb();
	return original_security_ops.inode_mknod(dir, dentry, mode, dev);
}

/**
 * ccs_inode_mkdir - Check permission for mkdir().
 *
 * @dir:    Pointer to "struct inode".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_mkdir(struct inode *dir, struct dentry *dentry, int mode)
{
	int rc = ccs_mkdir_permission(dentry, NULL, mode);
	if (rc)
		return rc;
	while (!original_security_ops.inode_mkdir)
		smp_rmb();
	return original_security_ops.inode_mkdir(dir, dentry, mode);
}

#endif

/**
 * ccs_inode_rmdir - Check permission for rmdir().
 *
 * @dir:    Pointer to "struct inode".
 * @dentry: Pointer to "struct dentry".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_rmdir(struct inode *dir, struct dentry *dentry)
{
	int rc = ccs_rmdir_permission(dentry, NULL);
	if (rc)
		return rc;
	while (!original_security_ops.inode_rmdir)
		smp_rmb();
	return original_security_ops.inode_rmdir(dir, dentry);
}

/**
 * ccs_inode_unlink - Check permission for unlink().
 *
 * @dir:    Pointer to "struct inode".
 * @dentry: Pointer to "struct dentry".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_unlink(struct inode *dir, struct dentry *dentry)
{
	int rc = ccs_unlink_permission(dentry, NULL);
	if (rc)
		return rc;
	while (!original_security_ops.inode_unlink)
		smp_rmb();
	return original_security_ops.inode_unlink(dir, dentry);
}

/**
 * ccs_inode_symlink - Check permission for symlink().
 *
 * @dir:      Pointer to "struct inode".
 * @dentry:   Pointer to "struct dentry".
 * @old_name: Content of symbolic link.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_symlink(struct inode *dir, struct dentry *dentry,
			     const char *old_name)
{
	int rc = ccs_symlink_permission(dentry, NULL, old_name);
	if (rc)
		return rc;
	while (!original_security_ops.inode_symlink)
		smp_rmb();
	return original_security_ops.inode_symlink(dir, dentry, old_name);
}

/**
 * ccs_inode_rename - Check permission for rename().
 *
 * @old_dir:    Pointer to "struct inode".
 * @old_dentry: Pointer to "struct dentry".
 * @new_dir:    Pointer to "struct inode".
 * @new_dentry: Pointer to "struct dentry".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_rename(struct inode *old_dir, struct dentry *old_dentry,
			    struct inode *new_dir, struct dentry *new_dentry)
{
	int rc = ccs_rename_permission(old_dentry, new_dentry, NULL);
	if (rc)
		return rc;
	while (!original_security_ops.inode_rename)
		smp_rmb();
	return original_security_ops.inode_rename(old_dir, old_dentry, new_dir,
						  new_dentry);
}

/**
 * ccs_inode_link - Check permission for link().
 *
 * @old_dentry: Pointer to "struct dentry".
 * @dir:        Pointer to "struct inode".
 * @new_dentry: Pointer to "struct dentry".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_link(struct dentry *old_dentry, struct inode *dir,
			  struct dentry *new_dentry)
{
	int rc = ccs_link_permission(old_dentry, new_dentry, NULL);
	if (rc)
		return rc;
	while (!original_security_ops.inode_link)
		smp_rmb();
	return original_security_ops.inode_link(old_dentry, dir, new_dentry);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 3, 0)

/**
 * ccs_inode_create - Check permission for creat().
 *
 * @dir:    Pointer to "struct inode".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_create(struct inode *dir, struct dentry *dentry,
			    umode_t mode)
{
	int rc = ccs_mknod_permission(dentry, NULL, mode, 0);
	if (rc)
		return rc;
	while (!original_security_ops.inode_create)
		smp_rmb();
	return original_security_ops.inode_create(dir, dentry, mode);
}

#else

/**
 * ccs_inode_create - Check permission for creat().
 *
 * @dir:    Pointer to "struct inode".
 * @dentry: Pointer to "struct dentry".
 * @mode:   Create mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_inode_create(struct inode *dir, struct dentry *dentry,
			    int mode)
{
	int rc = ccs_mknod_permission(dentry, NULL, mode, 0);
	if (rc)
		return rc;
	while (!original_security_ops.inode_create)
		smp_rmb();
	return original_security_ops.inode_create(dir, dentry, mode);
}

#endif

#endif

#ifdef CONFIG_SECURITY_NETWORK

#include <net/sock.h>

/* Structure for remembering an accept()ed socket's status. */
struct ccs_socket_tag {
	struct list_head list;
	struct inode *inode;
	int status;
	struct rcu_head rcu;
};

/*
 * List for managing accept()ed sockets.
 * Since we don't need to keep an accept()ed socket into this list after
 * once the permission was granted, the number of entries in this list is
 * likely small. Therefore, we don't use hash tables.
 */
static LIST_HEAD(ccs_accepted_socket_list);
/* Lock for protecting ccs_accepted_socket_list . */
static DEFINE_SPINLOCK(ccs_accepted_socket_list_lock);

/**
 * ccs_socket_rcu_free - RCU callback for releasing "struct ccs_socket_tag".
 *
 * @rcu: Pointer to "struct rcu_head".
 *
 * Returns nothing.
 */
static void ccs_socket_rcu_free(struct rcu_head *rcu)
{
	struct ccs_socket_tag *ptr = container_of(rcu, typeof(*ptr), rcu);
	kfree(ptr);
}

/**
 * ccs_update_socket_tag - Update tag associated with accept()ed sockets.
 *
 * @inode:  Pointer to "struct inode".
 * @status: New status.
 *
 * Returns nothing.
 *
 * If @status == 0, memory for that socket will be released after RCU grace
 * period.
 */
static void ccs_update_socket_tag(struct inode *inode, int status)
{
	struct ccs_socket_tag *ptr;
	/*
	 * Protect whole section because multiple threads may call this
	 * function with same "sock" via ccs_validate_socket().
	 */
	spin_lock(&ccs_accepted_socket_list_lock);
	rcu_read_lock();
	list_for_each_entry_rcu(ptr, &ccs_accepted_socket_list, list) {
		if (ptr->inode != inode)
			continue;
		ptr->status = status;
		if (status)
			break;
		list_del_rcu(&ptr->list);
		call_rcu(&ptr->rcu, ccs_socket_rcu_free);
		break;
	}
	rcu_read_unlock();
	spin_unlock(&ccs_accepted_socket_list_lock);
}

/**
 * ccs_validate_socket - Check post accept() permission if needed.
 *
 * @sock: Pointer to "struct socket".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_validate_socket(struct socket *sock)
{
	struct inode *inode = SOCK_INODE(sock);
	struct ccs_socket_tag *ptr;
	int ret = 0;
	rcu_read_lock();
	list_for_each_entry_rcu(ptr, &ccs_accepted_socket_list, list) {
		if (ptr->inode != inode)
			continue;
		ret = ptr->status;
		break;
	}
	rcu_read_unlock();
	if (ret <= 0)
		/*
		 * This socket is not an accept()ed socket or this socket is
		 * an accept()ed socket and post accept() permission is done.
		 */
		return ret;
	/*
	 * Check post accept() permission now.
	 *
	 * Strictly speaking, we need to pass both listen()ing socket and
	 * accept()ed socket to __ccs_socket_post_accept_permission().
	 * But since socket's family and type are same for both sockets,
	 * passing the accept()ed socket in place for the listen()ing socket
	 * will work.
	 */
	ret = ccs_socket_post_accept_permission(sock, sock);
	/*
	 * If permission was granted, we forget that this is an accept()ed
	 * socket. Otherwise, we remember that this socket needs to return
	 * error for subsequent socketcalls.
	 */
	ccs_update_socket_tag(inode, ret);
	return ret;
}

/**
 * ccs_socket_accept - Check permission for accept().
 *
 * @sock:    Pointer to "struct socket".
 * @newsock: Pointer to "struct socket".
 *
 * Returns 0 on success, negative value otherwise.
 *
 * This hook is used for setting up environment for doing post accept()
 * permission check. If dereferencing sock->ops->something() were ordered by
 * rcu_dereference(), we could replace sock->ops with "a copy of original
 * sock->ops with modified sock->ops->accept()" using rcu_assign_pointer()
 * in order to do post accept() permission check before returning to userspace.
 * If we make the copy in security_socket_post_create(), it would be possible
 * to safely replace sock->ops here, but we don't do so because we don't want
 * to allocate memory for sockets which do not call sock->ops->accept().
 * Therefore, we do post accept() permission check upon next socket syscalls
 * rather than between sock->ops->accept() and returning to userspace.
 * This means that if a socket was close()d before calling some socket
 * syscalls, post accept() permission check will not be done.
 */
static int ccs_socket_accept(struct socket *sock, struct socket *newsock)
{
	struct ccs_socket_tag *ptr;
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	ptr = kzalloc(sizeof(*ptr), GFP_KERNEL);
	if (!ptr)
		return -ENOMEM;
	while (!original_security_ops.socket_accept)
		smp_rmb();
	rc = original_security_ops.socket_accept(sock, newsock);
	if (rc) {
		kfree(ptr);
		return rc;
	}
	/*
	 * Subsequent LSM hooks will receive "newsock". Therefore, I mark
	 * "newsock" as "an accept()ed socket but post accept() permission
	 * check is not done yet" by allocating memory using inode of the
	 * "newsock" as a search key.
	 */
	ptr->inode = SOCK_INODE(newsock);
	ptr->status = 1; /* Check post accept() permission later. */
	spin_lock(&ccs_accepted_socket_list_lock);
	list_add_tail_rcu(&ptr->list, &ccs_accepted_socket_list);
	spin_unlock(&ccs_accepted_socket_list_lock);
	return 0;
}

/**
 * ccs_socket_listen - Check permission for listen().
 *
 * @sock:    Pointer to "struct socket".
 * @backlog: Backlog parameter.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_listen(struct socket *sock, int backlog)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	rc = ccs_socket_listen_permission(sock);
	if (rc)
		return rc;
	while (!original_security_ops.socket_listen)
		smp_rmb();
	return original_security_ops.socket_listen(sock, backlog);
}

/**
 * ccs_socket_connect - Check permission for connect().
 *
 * @sock:     Pointer to "struct socket".
 * @addr:     Pointer to "struct sockaddr".
 * @addr_len: Size of @addr.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_connect(struct socket *sock, struct sockaddr *addr,
			      int addr_len)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	rc = ccs_socket_connect_permission(sock, addr, addr_len);
	if (rc)
		return rc;
	while (!original_security_ops.socket_connect)
		smp_rmb();
	return original_security_ops.socket_connect(sock, addr, addr_len);
}

/**
 * ccs_socket_bind - Check permission for bind().
 *
 * @sock:     Pointer to "struct socket".
 * @addr:     Pointer to "struct sockaddr".
 * @addr_len: Size of @addr.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_bind(struct socket *sock, struct sockaddr *addr,
			   int addr_len)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	rc = ccs_socket_bind_permission(sock, addr, addr_len);
	if (rc)
		return rc;
	while (!original_security_ops.socket_bind)
		smp_rmb();
	return original_security_ops.socket_bind(sock, addr, addr_len);
}

/**
 * ccs_socket_sendmsg - Check permission for sendmsg().
 *
 * @sock: Pointer to "struct socket".
 * @msg:  Pointer to "struct msghdr".
 * @size: Size of message.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_sendmsg(struct socket *sock, struct msghdr *msg,
			      int size)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	rc = ccs_socket_sendmsg_permission(sock, msg, size);
	if (rc)
		return rc;
	while (!original_security_ops.socket_sendmsg)
		smp_rmb();
	return original_security_ops.socket_sendmsg(sock, msg, size);
}

/**
 * ccs_socket_recvmsg - Check permission for recvmsg().
 *
 * @sock:  Pointer to "struct socket".
 * @msg:   Pointer to "struct msghdr".
 * @size:  Size of message.
 * @flags: Flags.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_recvmsg(struct socket *sock, struct msghdr *msg,
			      int size, int flags)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	while (!original_security_ops.socket_recvmsg)
		smp_rmb();
	return original_security_ops.socket_recvmsg(sock, msg, size, flags);
}

/**
 * ccs_socket_getsockname - Check permission for getsockname().
 *
 * @sock: Pointer to "struct socket".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_getsockname(struct socket *sock)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	while (!original_security_ops.socket_getsockname)
		smp_rmb();
	return original_security_ops.socket_getsockname(sock);
}

/**
 * ccs_socket_getpeername - Check permission for getpeername().
 *
 * @sock: Pointer to "struct socket".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_getpeername(struct socket *sock)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	while (!original_security_ops.socket_getpeername)
		smp_rmb();
	return original_security_ops.socket_getpeername(sock);
}

/**
 * ccs_socket_getsockopt - Check permission for getsockopt().
 *
 * @sock:    Pointer to "struct socket".
 * @level:   Level.
 * @optname: Option's name,
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_getsockopt(struct socket *sock, int level, int optname)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	while (!original_security_ops.socket_getsockopt)
		smp_rmb();
	return original_security_ops.socket_getsockopt(sock, level, optname);
}

/**
 * ccs_socket_setsockopt - Check permission for setsockopt().
 *
 * @sock:    Pointer to "struct socket".
 * @level:   Level.
 * @optname: Option's name,
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_setsockopt(struct socket *sock, int level, int optname)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	while (!original_security_ops.socket_setsockopt)
		smp_rmb();
	return original_security_ops.socket_setsockopt(sock, level, optname);
}

/**
 * ccs_socket_shutdown - Check permission for shutdown().
 *
 * @sock: Pointer to "struct socket".
 * @how:  Shutdown mode.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_socket_shutdown(struct socket *sock, int how)
{
	int rc = ccs_validate_socket(sock);
	if (rc < 0)
		return rc;
	while (!original_security_ops.socket_shutdown)
		smp_rmb();
	return original_security_ops.socket_shutdown(sock, how);
}

#define SOCKFS_MAGIC 0x534F434B

/**
 * ccs_inode_free_security - Release memory associated with an inode.
 *
 * @inode: Pointer to "struct inode".
 *
 * Returns nothing.
 *
 * We use this hook for releasing memory associated with an accept()ed socket.
 */
static void ccs_inode_free_security(struct inode *inode)
{
	while (!original_security_ops.inode_free_security)
		smp_rmb();
	original_security_ops.inode_free_security(inode);
	if (inode->i_sb && inode->i_sb->s_magic == SOCKFS_MAGIC)
		ccs_update_socket_tag(inode, 0);
}

#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(3, 7, 0)

/**
 * ccs_sb_pivotroot - Check permission for pivot_root().
 *
 * @old_path: Pointer to "struct path".
 * @new_path: Pointer to "struct path".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_sb_pivotroot(struct path *old_path, struct path *new_path)
{
	int rc = ccs_pivot_root_permission(old_path, new_path);
	if (rc)
		return rc;
	while (!original_security_ops.sb_pivotroot)
		smp_rmb();
	return original_security_ops.sb_pivotroot(old_path, new_path);
}

/**
 * ccs_sb_mount - Check permission for mount().
 *
 * @dev_name:  Name of device file.
 * @path:      Pointer to "struct path".
 * @type:      Name of filesystem type. Maybe NULL.
 * @flags:     Mount options.
 * @data_page: Optional data. Maybe NULL.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_sb_mount(char *dev_name, struct path *path, char *type,
			unsigned long flags, void *data_page)
{
	int rc = ccs_mount_permission(dev_name, path, type, flags, data_page);
	if (rc)
		return rc;
	while (!original_security_ops.sb_mount)
		smp_rmb();
	return original_security_ops.sb_mount(dev_name, path, type, flags,
					      data_page);
}

#else

/**
 * ccs_sb_pivotroot - Check permission for pivot_root().
 *
 * @old_path: Pointer to "struct path".
 * @new_path: Pointer to "struct path".
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_sb_pivotroot(struct path *old_path, struct path *new_path)
{
	int rc = ccs_pivot_root_permission(old_path, new_path);
	if (rc)
		return rc;
	while (!original_security_ops.sb_pivotroot)
		smp_rmb();
	return original_security_ops.sb_pivotroot(old_path, new_path);
}

/**
 * ccs_sb_mount - Check permission for mount().
 *
 * @dev_name:  Name of device file.
 * @path:      Pointer to "struct path".
 * @type:      Name of filesystem type. Maybe NULL.
 * @flags:     Mount options.
 * @data_page: Optional data. Maybe NULL.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_sb_mount(const char *dev_name, struct path *path,
			const char *type, unsigned long flags, void *data_page)
{
	int rc = ccs_mount_permission(dev_name, path, type, flags, data_page);
	if (rc)
		return rc;
	while (!original_security_ops.sb_mount)
		smp_rmb();
	return original_security_ops.sb_mount(dev_name, path, type, flags,
					      data_page);
}

#endif

/**
 * ccs_sb_umount - Check permission for umount().
 *
 * @mnt:   Pointer to "struct vfsmount".
 * @flags: Unmount flags.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_sb_umount(struct vfsmount *mnt, int flags)
{
	int rc = ccs_umount_permission(mnt, flags);
	if (rc)
		return rc;
	while (!original_security_ops.sb_umount)
		smp_rmb();
	return original_security_ops.sb_umount(mnt, flags);
}

/**
 * ccs_file_fcntl - Check permission for fcntl().
 *
 * @file: Pointer to "struct file".
 * @cmd:  Command number.
 * @arg:  Value for @cmd.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_file_fcntl(struct file *file, unsigned int cmd,
			  unsigned long arg)
{
	int rc = ccs_fcntl_permission(file, cmd, arg);
	if (rc)
		return rc;
	while (!original_security_ops.file_fcntl)
		smp_rmb();
	return original_security_ops.file_fcntl(file, cmd, arg);
}

/**
 * ccs_file_ioctl - Check permission for ioctl().
 *
 * @filp: Pointer to "struct file".
 * @cmd:  Command number.
 * @arg:  Value for @cmd.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_file_ioctl(struct file *filp, unsigned int cmd,
			  unsigned long arg)
{
	int rc = ccs_ioctl_permission(filp, cmd, arg);
	if (rc)
		return rc;
	while (!original_security_ops.file_ioctl)
		smp_rmb();
	return original_security_ops.file_ioctl(filp, cmd, arg);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33) && defined(CONFIG_SYSCTL_SYSCALL)
int ccs_path_permission(struct ccs_request_info *r, u8 operation,
			const struct ccs_path_info *filename);

/**
 * ccs_prepend - Copy of prepend() in fs/dcache.c.
 *
 * @buffer: Pointer to "struct char *".
 * @buflen: Pointer to int which holds size of @buffer.
 * @str:    String to copy.
 *
 * Returns 0 on success, negative value otherwise.
 *
 * @buffer and @buflen are updated upon success.
 */
static int ccs_prepend(char **buffer, int *buflen, const char *str)
{
	int namelen = strlen(str);
	if (*buflen < namelen)
		return -ENOMEM;
	*buflen -= namelen;
	*buffer -= namelen;
	memcpy(*buffer, str, namelen);
	return 0;
}

/**
 * ccs_sysctl_permission - Check permission for sysctl().
 *
 * @table: Pointer to "struct ctl_table".
 * @op:    Operation. (MAY_READ and/or MAY_WRITE)
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_sysctl(struct ctl_table *table, int op)
{
	int error;
	struct ccs_path_info buf;
	struct ccs_request_info r;
	int buflen;
	char *buffer;
	int idx;
	while (!original_security_ops.sysctl)
		smp_rmb();
	error = original_security_ops.sysctl(table, op);
	if (error)
		return error;
	op &= MAY_READ | MAY_WRITE;
	if (!op)
		return 0;
	buffer = NULL;
	buf.name = NULL;
	idx = ccs_read_lock();
	if (ccs_init_request_info(&r, CCS_MAC_FILE_OPEN)
	    == CCS_CONFIG_DISABLED)
		goto out;
	error = -ENOMEM;
	buflen = 4096;
	buffer = kmalloc(buflen, CCS_GFP_FLAGS);
	if (buffer) {
		char *end = buffer + buflen;
		*--end = '\0';
		buflen--;
		while (table) {
			char num[32];
			const char *sp = table->procname;
			if (!sp) {
				memset(num, 0, sizeof(num));
				snprintf(num, sizeof(num) - 1, "=%d=",
					 table->ctl_name);
				sp = num;
			}
			if (ccs_prepend(&end, &buflen, sp) ||
			    ccs_prepend(&end, &buflen, "/"))
				goto out;
			table = table->parent;
		}
		if (ccs_prepend(&end, &buflen, "proc:/sys"))
			goto out;
		buf.name = ccs_encode(end);
	}
	if (buf.name) {
		ccs_fill_path_info(&buf);
		if (op & MAY_READ)
			error = ccs_path_permission(&r, CCS_TYPE_READ, &buf);
		else
			error = 0;
		if (!error && (op & MAY_WRITE))
			error = ccs_path_permission(&r, CCS_TYPE_WRITE, &buf);
	}
out:
	ccs_read_unlock(idx);
	kfree(buf.name);
	kfree(buffer);
	return error;
}

#endif

/*
 * Why not to copy all operations by "original_security_ops = *ops" ?
 * Because copying byte array is not atomic. Reader checks
 * original_security_ops.op != NULL before doing original_security_ops.op().
 * Thus, modifying original_security_ops.op has to be atomic.
 */
#define swap_security_ops(op)						\
	original_security_ops.op = ops->op; smp_wmb(); ops->op = ccs_##op;

/**
 * ccs_update_security_ops - Overwrite original "struct security_operations".
 *
 * @ops: Pointer to "struct security_operations".
 *
 * Returns nothing.
 */
static void __init ccs_update_security_ops(struct security_operations *ops)
{
	/* Security context allocator. */
	swap_security_ops(task_create);
	swap_security_ops(cred_prepare);
	swap_security_ops(cred_free);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	swap_security_ops(cred_alloc_blank);
	swap_security_ops(cred_transfer);
#endif
	/* Security context updater for successful execve(). */
	swap_security_ops(bprm_check_security);
	swap_security_ops(bprm_committing_creds);
	/* Various permission checker. */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 5, 0)
	swap_security_ops(file_open);
#else
	swap_security_ops(dentry_open);
#endif
	swap_security_ops(file_fcntl);
	swap_security_ops(file_ioctl);
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 33) && defined(CONFIG_SYSCTL_SYSCALL)
	swap_security_ops(sysctl);
#endif
	swap_security_ops(sb_pivotroot);
	swap_security_ops(sb_mount);
	swap_security_ops(sb_umount);
#if defined(CONFIG_SECURITY_PATH)
	swap_security_ops(path_mknod);
	swap_security_ops(path_mkdir);
	swap_security_ops(path_rmdir);
	swap_security_ops(path_unlink);
	swap_security_ops(path_symlink);
	swap_security_ops(path_rename);
	swap_security_ops(path_link);
	swap_security_ops(path_truncate);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 33)
	swap_security_ops(path_chmod);
	swap_security_ops(path_chown);
	swap_security_ops(path_chroot);
#endif
#else
	swap_security_ops(inode_mknod);
	swap_security_ops(inode_mkdir);
	swap_security_ops(inode_rmdir);
	swap_security_ops(inode_unlink);
	swap_security_ops(inode_symlink);
	swap_security_ops(inode_rename);
	swap_security_ops(inode_link);
	swap_security_ops(inode_create);
#endif
	swap_security_ops(inode_setattr);
	swap_security_ops(inode_getattr);
#ifdef CONFIG_SECURITY_NETWORK
	swap_security_ops(socket_bind);
	swap_security_ops(socket_connect);
	swap_security_ops(socket_listen);
	swap_security_ops(socket_sendmsg);
	swap_security_ops(socket_recvmsg);
	swap_security_ops(socket_getsockname);
	swap_security_ops(socket_getpeername);
	swap_security_ops(socket_getsockopt);
	swap_security_ops(socket_setsockopt);
	swap_security_ops(socket_shutdown);
	swap_security_ops(socket_accept);
	swap_security_ops(inode_free_security);
#endif
}

#undef swap_security_ops

/**
 * ccs_init - Initialize this module.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int __init ccs_init(void)
{
	struct security_operations *ops = probe_security_ops();
	if (!ops)
		goto out;
	ccsecurity_exports.find_task_by_vpid = probe_find_task_by_vpid();
	if (!ccsecurity_exports.find_task_by_vpid)
		goto out;
	ccsecurity_exports.find_task_by_pid_ns = probe_find_task_by_pid_ns();
	if (!ccsecurity_exports.find_task_by_pid_ns)
		goto out;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
	ccsecurity_exports.vfsmount_lock = probe_vfsmount_lock();
	if (!ccsecurity_exports.vfsmount_lock)
		goto out;
#elif LINUX_VERSION_CODE < KERNEL_VERSION(3, 2, 0)
	ccsecurity_exports.__d_path = probe___d_path();
	if (!ccsecurity_exports.__d_path)
		goto out;
#else
	ccsecurity_exports.d_absolute_path = probe_d_absolute_path();
	if (!ccsecurity_exports.d_absolute_path)
		goto out;
#endif
	{
		int idx;
		for (idx = 0; idx < CCS_MAX_TASK_SECURITY_HASH; idx++) {
			INIT_LIST_HEAD(&ccs_cred_security_list[idx]);
			INIT_LIST_HEAD(&ccs_task_security_list[idx]);
		}
	}
	ccs_main_init();
	ccs_update_security_ops(ops);
	printk(KERN_INFO "AKARI: 1.0.37   2017/09/17\n");
	printk(KERN_INFO
	       "Access Keeping And Regulating Instrument registered.\n");
	return 0;
out:
	return -EINVAL;
}

module_init(ccs_init);
MODULE_LICENSE("GPL");

/**
 * ccs_used_by_cred - Check whether the given domain is in use or not.
 *
 * @domain: Pointer to "struct ccs_domain_info".
 *
 * Returns true if @domain is in use, false otherwise.
 *
 * Caller holds rcu_read_lock().
 */
bool ccs_used_by_cred(const struct ccs_domain_info *domain)
{
	int idx;
	struct ccs_security *ptr;
	for (idx = 0; idx < CCS_MAX_TASK_SECURITY_HASH; idx++) {
		struct list_head *list = &ccs_cred_security_list[idx];
		list_for_each_entry_rcu(ptr, list, list) {
			struct ccs_execve *ee = ptr->ee;
			if (ptr->ccs_domain_info == domain ||
			    (ee && ee->previous_domain == domain)) {
				return true;
			}
		}
	}
	return false;
}

/**
 * ccs_add_task_security - Add "struct ccs_security" to list.
 *
 * @ptr:  Pointer to "struct ccs_security".
 * @list: Pointer to "struct list_head".
 *
 * Returns nothing.
 */
static void ccs_add_task_security(struct ccs_security *ptr,
				  struct list_head *list)
{
	unsigned long flags;
	spin_lock_irqsave(&ccs_task_security_list_lock, flags);
	list_add_rcu(&ptr->list, list);
	spin_unlock_irqrestore(&ccs_task_security_list_lock, flags);
}

/**
 * ccs_find_task_security - Find "struct ccs_security" for given task.
 *
 * @task: Pointer to "struct task_struct".
 *
 * Returns pointer to "struct ccs_security" on success, &ccs_oom_security on
 * out of memory, &ccs_default_security otherwise.
 *
 * If @task is current thread and "struct ccs_security" for current thread was
 * not found, I try to allocate it. But if allocation failed, current thread
 * will be killed by SIGKILL. Note that if current->pid == 1, sending SIGKILL
 * won't work.
 */
struct ccs_security *ccs_find_task_security(const struct task_struct *task)
{
	struct ccs_security *ptr;
	struct list_head *list = &ccs_task_security_list
		[hash_ptr((void *) task, CCS_TASK_SECURITY_HASH_BITS)];
	/* Make sure INIT_LIST_HEAD() in ccs_mm_init() takes effect. */
	while (!list->next);
	rcu_read_lock();
	list_for_each_entry_rcu(ptr, list, list) {
		if (ptr->pid != task->pids[PIDTYPE_PID].pid)
			continue;
		rcu_read_unlock();
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 31)
		/*
		 * Current thread needs to transit from old domain to new
		 * domain before do_execve() succeeds in order to check
		 * permission for interpreters and environment variables using
		 * new domain's ACL rules. The domain transition has to be
		 * visible from other CPU in order to allow interactive
		 * enforcing mode. Also, the domain transition has to be
		 * reverted if do_execve() failed. However, an LSM hook for
		 * reverting domain transition is missing.
		 *
		 * security_prepare_creds() is called from prepare_creds() from
		 * prepare_bprm_creds() from do_execve() before setting
		 * current->in_execve flag, and current->in_execve flag is
		 * cleared by the time next do_execve() request starts.
		 * This means that we can emulate the missing LSM hook for
		 * reverting domain transition, by calling this function from
		 * security_prepare_creds().
		 *
		 * If current->in_execve is not set but ptr->ccs_flags has
		 * CCS_TASK_IS_IN_EXECVE set, it indicates that do_execve()
		 * has failed and reverting domain transition is needed.
		 */
		if (task == current &&
		    (ptr->ccs_flags & CCS_TASK_IS_IN_EXECVE) &&
		    !current->in_execve) {
			ccs_debug_trace("4");
			ccs_clear_execve(-1, ptr);
		}
#else
		/*
		 * Current thread needs to transit from old domain to new
		 * domain before do_execve() succeeds in order to check
		 * permission for interpreters and environment variables using
		 * new domain's ACL rules. The domain transition has to be
		 * visible from other CPU in order to allow interactive
		 * enforcing mode. Also, the domain transition has to be
		 * reverted if do_execve() failed. However, an LSM hook for
		 * reverting domain transition is missing.
		 *
		 * When do_execve() failed, "struct cred" in
		 * "struct linux_binprm" is scheduled for destruction.
		 * But current thread returns to userspace without waiting for
		 * destruction. The security_cred_free() LSM hook is called
		 * after an RCU grace period has elapsed. Since some CPU may be
		 * doing long long RCU read side critical section, there is
		 * no guarantee that security_cred_free() is called before
		 * current thread again calls do_execve().
		 *
		 * To be able to revert domain transition before processing
		 * next do_execve() request, current thread gets a refcount on
		 * "struct cred" in "struct linux_binprm" and memorizes it.
		 * Current thread drops the refcount and forgets it when
		 * do_execve() succeeded.
		 *
		 * Therefore, if current thread hasn't forgotten it and
		 * current thread is the last one using that "struct cred",
		 * it indicates that do_execve() has failed and reverting
		 * domain transition is needed.
		 */
		if (task == current && ptr->cred &&
		    atomic_read(&ptr->cred->usage) == 1) {
			ccs_debug_trace("4");
			ccs_clear_execve(-1, ptr);
		}
#endif
		return ptr;
	}
	rcu_read_unlock();
	if (task != current) {
		/*
		 * If a thread does nothing after fork(), caller will reach
		 * here because "struct ccs_security" for that thread is not
		 * yet allocated. But that thread is keeping a snapshot of
		 * "struct ccs_security" taken as of ccs_task_create()
		 * associated with that thread's "struct cred".
		 *
		 * Since that snapshot will be used as initial data when that
		 * thread allocates "struct ccs_security" for that thread, we
		 * can return that snapshot rather than &ccs_default_security.
		 *
		 * Since this function is called by only ccs_select_one() and
		 * ccs_read_pid() (via ccs_task_domain() and ccs_task_flags()),
		 * it is guaranteed that caller has called rcu_read_lock()
		 * (via ccs_tasklist_lock()) before finding this thread and
		 * this thread is valid. Therefore, we can do __task_cred(task)
		 * like get_robust_list() does.
		 */
		return ccs_find_cred_security(__task_cred(task));
	}
	/* Use GFP_ATOMIC because caller may have called rcu_read_lock(). */
	ptr = kzalloc(sizeof(*ptr), GFP_ATOMIC);
	if (!ptr) {
		printk(KERN_WARNING "Unable to allocate memory for pid=%u\n",
		       task->pid);
		send_sig(SIGKILL, current, 0);
		return &ccs_oom_security;
	}
	*ptr = *ccs_find_cred_security(task->cred);
	/* We can shortcut because task == current. */
	ptr->pid = get_pid(((struct task_struct *) task)->
			   pids[PIDTYPE_PID].pid);
	ptr->cred = NULL;
	ccs_add_task_security(ptr, list);
	return ptr;
}

/**
 * ccs_copy_cred_security - Allocate memory for new credentials.
 *
 * @new: Pointer to "struct cred".
 * @old: Pointer to "struct cred".
 * @gfp: Memory allocation flags.
 *
 * Returns 0 on success, negative value otherwise.
 */
static int ccs_copy_cred_security(const struct cred *new,
				  const struct cred *old, gfp_t gfp)
{
	struct ccs_security *old_security = ccs_find_cred_security(old);
	struct ccs_security *new_security =
		kzalloc(sizeof(*new_security), gfp);
	if (!new_security)
		return -ENOMEM;
	*new_security = *old_security;
	new_security->cred = new;
	ccs_add_cred_security(new_security);
	return 0;
}

/**
 * ccs_find_cred_security - Find "struct ccs_security" for given credential.
 *
 * @cred: Pointer to "struct cred".
 *
 * Returns pointer to "struct ccs_security" on success, &ccs_default_security
 * otherwise.
 */
static struct ccs_security *ccs_find_cred_security(const struct cred *cred)
{
	struct ccs_security *ptr;
	struct list_head *list = &ccs_cred_security_list
		[hash_ptr((void *) cred, CCS_TASK_SECURITY_HASH_BITS)];
	rcu_read_lock();
	list_for_each_entry_rcu(ptr, list, list) {
		if (ptr->cred != cred)
			continue;
		rcu_read_unlock();
		return ptr;
	}
	rcu_read_unlock();
	return &ccs_default_security;
}

/**
 * ccs_task_security_gc - Do garbage collection for "struct task_struct".
 *
 * Returns nothing.
 *
 * Since security_task_free() is missing, I can't release memory associated
 * with "struct task_struct" when a task dies. Therefore, I hold a reference on
 * "struct pid" and runs garbage collection when associated
 * "struct task_struct" has gone.
 */
static void ccs_task_security_gc(void)
{
	static DEFINE_SPINLOCK(lock);
	static atomic_t gc_counter = ATOMIC_INIT(0);
	unsigned int idx;
	/*
	 * If some process is doing execve(), try to garbage collection now.
	 * We should kfree() memory associated with "struct ccs_security"->ee
	 * as soon as execve() has completed in order to compensate for lack of
	 * security_bprm_free() and security_task_free() hooks.
	 *
	 * Otherwise, reduce frequency for performance reason.
	 */
	if (!atomic_read(&ccs_in_execve_tasks) &&
	    atomic_inc_return(&gc_counter) < 1024)
		return;
	if (!spin_trylock(&lock))
		return;
	atomic_set(&gc_counter, 0);
	rcu_read_lock();
	for (idx = 0; idx < CCS_MAX_TASK_SECURITY_HASH; idx++) {
		struct ccs_security *ptr;
		struct list_head *list = &ccs_task_security_list[idx];
		list_for_each_entry_rcu(ptr, list, list) {
			if (pid_task(ptr->pid, PIDTYPE_PID))
				continue;
			ccs_del_security(ptr);
		}
	}
	rcu_read_unlock();
	spin_unlock(&lock);
}
