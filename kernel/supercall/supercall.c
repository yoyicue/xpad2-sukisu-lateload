#include <linux/anon_inodes.h>
#include <linux/err.h>
#include <linux/fdtable.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kprobes.h>
#include <linux/pid.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#ifndef TWA_RESUME
#define TWA_RESUME true
#endif

#include "uapi/supercall.h"
#include "kpm/kpm.h"
#include "supercall/internal.h"
#include "manager/manager_identity.h"
#include "arch.h"
#include "klog.h" // IWYU pragma: keep

struct ksu_install_fd_tw {
    struct callback_head cb;
    int __user *outp;
};

static int anon_ksu_release(struct inode *inode, struct file *filp)
{
    pr_info("ksu fd released\n");
    return 0;
}

static long anon_ksu_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    return ksu_supercall_handle_ioctl(cmd, (void __user *)arg);
}

static const struct file_operations anon_ksu_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = anon_ksu_ioctl,
    .compat_ioctl = anon_ksu_ioctl,
    .release = anon_ksu_release,
};

static int ksu_install_fd_with_flags(unsigned int flags)
{
    struct file *filp;
    int fd;

    fd = get_unused_fd_flags(flags);
    if (fd < 0) {
        pr_err("ksu_install_fd: failed to get unused fd\n");
        return fd;
    }

    filp = anon_inode_getfile("[ksu_driver]", &anon_ksu_fops, NULL,
                             O_RDWR | flags);
    if (IS_ERR(filp)) {
        pr_err("ksu_install_fd: failed to create anon inode file\n");
        put_unused_fd(fd);
        return PTR_ERR(filp);
    }

    fd_install(fd, filp);
    pr_info("ksu fd installed: %d for pid %d\n", fd, current->pid);
    return fd;
}

int ksu_install_fd(void)
{
    return ksu_install_fd_with_flags(O_CLOEXEC);
}

int ksu_install_fd_for_exec(void)
{
    if (!is_manager())
        return -EPERM;

    return ksu_install_fd_with_flags(0);
}

static void ksu_close_installed_fd(int fd)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 11, 0)
    close_fd(fd);
#else
    ksys_close(fd);
#endif
}

static void ksu_install_fd_tw_func(struct callback_head *cb)
{
    struct ksu_install_fd_tw *tw = container_of(cb, struct ksu_install_fd_tw, cb);
    int fd = ksu_install_fd();

    pr_info("[%d] install ksu fd: %d\n", current->pid, fd);
    if (copy_to_user(tw->outp, &fd, sizeof(fd))) {
        pr_err("install ksu fd reply err\n");
        ksu_close_installed_fd(fd);
    }

    kfree(tw);
}

void ksu_handle_reboot_supercall(const struct pt_regs *real_regs)
{
    int magic1 = (int)PT_REGS_PARM1(real_regs);
    int magic2 = (int)PT_REGS_PARM2(real_regs);

    if (magic1 == KSU_INSTALL_MAGIC1 && magic2 == KSU_INSTALL_MAGIC2) {
        struct ksu_install_fd_tw *tw;
        unsigned long arg4 = (unsigned long)PT_REGS_SYSCALL_PARM4(real_regs);

        tw = kzalloc(sizeof(*tw), GFP_ATOMIC);
        if (!tw)
            return;

        tw->outp = (int __user *)arg4;
        tw->cb.func = ksu_install_fd_tw_func;

        if (task_work_add(current, &tw->cb, TWA_RESUME)) {
            kfree(tw);
            pr_warn("install fd add task_work failed\n");
        }
    }
}

bool ksu_handle_reboot_supercall_direct(const struct pt_regs *regs)
{
    int magic1 = (int)PT_REGS_PARM1(regs);
    int magic2 = (int)PT_REGS_PARM2(regs);
    int __user *outp;
    int fd;

    if (magic1 != KSU_INSTALL_MAGIC1 || magic2 != KSU_INSTALL_MAGIC2)
        return false;

    outp = (int __user *)(unsigned long)PT_REGS_SYSCALL_PARM4(regs);
    fd = ksu_install_fd();
    if (fd >= 0 && copy_to_user(outp, &fd, sizeof(fd))) {
        ksu_close_installed_fd(fd);
        fd = -EFAULT;
    }
    pr_info("legacy reboot supercall installed fd: %d\n", fd);
    return true;
}

static int reboot_handler_pre(struct kprobe *p, struct pt_regs *regs)
{
    ksu_handle_reboot_supercall(PT_REAL_REGS(regs));

    return 0;
}

static struct kprobe reboot_kp = {
    .symbol_name = REBOOT_SYMBOL,
    .pre_handler = reboot_handler_pre,
};
static bool reboot_kp_registered;

void __init ksu_supercalls_init(void)
{
#ifndef CONFIG_KSU_LEGACY_4_19
    int rc;
#endif

    ksu_supercall_dump_commands();

#ifdef CONFIG_KSU_LEGACY_4_19
    pr_info("legacy reboot supercall uses direct syscall table adapter\n");
#else
    rc = register_kprobe(&reboot_kp);
    if (rc) {
        pr_err("reboot kprobe failed: %d\n", rc);
    } else {
        reboot_kp_registered = true;
        pr_info("reboot kprobe registered successfully\n");
    }
#endif
}

void __exit ksu_supercalls_exit(void)
{
    if (reboot_kp_registered)
        unregister_kprobe(&reboot_kp);
    ksu_supercall_cleanup_state();
}
