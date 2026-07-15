#include "linux/compiler.h"
#include "linux/cred.h"
#include "linux/jump_label.h"
#include "linux/printk.h"
#include "linux/string.h"
#include "linux/uaccess.h"
#include "selinux/selinux.h"
#include <asm/syscall.h>
#include <linux/ptrace.h>
#include <linux/static_key.h>

#include "arch.h"
#include "klog.h" // IWYU pragma: keep
#include "hook/tp_marker.h"
#include "feature/sucompat.h"
#include "hook/setuid_hook.h"
#include "policy/app_profile.h"
#include "runtime/ksud.h"
#include "sulog/event.h"
#include "hook/syscall_hook.h"
#include "hook/syscall_event_bridge.h"
#include "feature/adb_root.h"
#include "manager/manager_identity.h"
#include "supercall/supercall.h"

#ifdef CONFIG_KSU_LEGACY_4_19
#define KSU_MANAGER_EXEC_PATH_MAX 256
#define KSU_MANAGER_EXEC_ARG_MAX 16
#define KSU_LEGACY_USER_ADDR_MASK 0x00ffffffffffffffUL

static bool ksu_user_arg_equals(const char __user *const __user *argv,
                                int index, const char *expected)
{
    const char __user *arg;
    const void __user *arg_slot;
    unsigned long address;
    unsigned long limit;
    char value[KSU_MANAGER_EXEC_ARG_MAX];
    long copied;

    if (!argv)
        return false;

    address = (unsigned long)&argv[index] & KSU_LEGACY_USER_ADDR_MASK;
    limit = current_thread_info()->addr_limit;
    if (address > limit || sizeof(arg) - 1 > limit - address)
        return false;

    arg_slot = (const void __user *)address;
    if (__arch_copy_from_user(&arg, arg_slot, sizeof(arg)) || !arg)
        return false;

    copied = strncpy_from_user(value, arg, sizeof(value));
    if (copied <= 0 || copied >= sizeof(value))
        return false;

    value[sizeof(value) - 1] = '\0';
    return strcmp(value, expected) == 0;
}

static bool ksu_is_manager_ksud_su_exec(
    const char __user **filename_user,
    const char __user *const __user *argv)
{
    const char __user *filename;
    const char *basename;
    char path[KSU_MANAGER_EXEC_PATH_MAX];
    long copied;

    if (!is_manager() || !filename_user)
        return false;

    filename = (const char __user *)((unsigned long)*filename_user &
                                     KSU_LEGACY_USER_ADDR_MASK);
    copied = strncpy_from_user(path, filename, sizeof(path));
    if (copied <= 0 || copied >= sizeof(path))
        return false;

    path[sizeof(path) - 1] = '\0';
    basename = strrchr(path, '/');
    basename = basename ? basename + 1 : path;

    return strcmp(basename, "libksud.so") == 0 &&
           ksu_user_arg_equals(argv, 1, "debug") &&
           ksu_user_arg_equals(argv, 2, "su");
}
#endif

static int ksu_handle_init_mark_tracker(const char __user **filename_user)
{
    char path[64];
    unsigned long addr;
    const char __user *fn;
    long ret;

    if (unlikely(!filename_user))
        return 0;

#ifdef CONFIG_KSU_LEGACY_4_19
    addr = (unsigned long)*filename_user & KSU_LEGACY_USER_ADDR_MASK;
#else
    addr = untagged_addr((unsigned long)*filename_user);
#endif
    fn = (const char __user *)addr;
    ret = strncpy_from_user(path, fn, sizeof(path));
    if (ret < 0)
        return 0;

    path[sizeof(path) - 1] = '\0';
    if (unlikely(strcmp(path, KSUD_PATH) == 0)) {
        pr_info("hook_manager: escape to root for init executing ksud: %d\n", current->pid);
        escape_to_root_for_init();
    } else if (likely(strstr(path, "/app_process") == NULL && strstr(path, "/adbd") == NULL)) {
        pr_info("hook_manager: unmark %d exec %s\n", current->pid, path);
        ksu_clear_task_tracepoint_flag_if_needed(current);
    }

    return 0;
}

long __nocfi ksu_hook_newfstatat(int orig_nr, const struct pt_regs *regs)
{
    int *dfd;
    const char __user **filename_user;
    int *flags;

    if (!ksu_su_compat_enabled)
        return ksu_get_original_syscall(orig_nr)(regs);

    dfd = (int *)&PT_REGS_PARM1(regs);
    filename_user = (const char __user **)&PT_REGS_PARM2(regs);
    flags = (int *)&PT_REGS_SYSCALL_PARM4(regs);
    ksu_handle_stat(dfd, filename_user, flags);

    return ksu_get_original_syscall(orig_nr)(regs);
}

long __nocfi ksu_hook_faccessat(int orig_nr, const struct pt_regs *regs)
{
    int *dfd;
    const char __user **filename_user;
    int *mode;

    if (!ksu_su_compat_enabled)
        return ksu_get_original_syscall(orig_nr)(regs);

    dfd = (int *)&PT_REGS_PARM1(regs);
    filename_user = (const char __user **)&PT_REGS_PARM2(regs);
    mode = (int *)&PT_REGS_PARM3(regs);
    ksu_handle_faccessat(dfd, filename_user, mode, NULL);

    return ksu_get_original_syscall(orig_nr)(regs);
}

#ifdef CONFIG_KSU_LEGACY_4_19
static bool ksud_execve_enabled = true;
#else
DEFINE_STATIC_KEY_TRUE(ksud_execve_key);
#endif

void ksu_stop_ksud_execve_hook()
{
#ifdef CONFIG_KSU_LEGACY_4_19
    ksud_execve_enabled = false;
#else
    static_branch_disable(&ksud_execve_key);
#endif
}

long __nocfi ksu_hook_execve(int orig_nr, const struct pt_regs *regs)
{
    const char __user **filename_user = (const char __user **)&PT_REGS_PARM1(regs);
    const char __user *const __user *argv_user = (const char __user *const __user *)PT_REGS_PARM2(regs);
    bool current_is_init = is_init(current_cred());
    struct ksu_sulog_pending_event *pending_root_execve = NULL;
    long ret;

#ifdef CONFIG_KSU_LEGACY_4_19
    if (ksu_is_manager_ksud_su_exec(filename_user, argv_user)) {
        int fd = ksu_install_fd_for_exec();

        if (fd < 0)
            pr_err("legacy manager ksud bootstrap fd failed: %d\n", fd);
        else
            pr_info("legacy manager ksud bootstrap fd installed: %d for pid %d\n",
                    fd, current->pid);
    }
#endif

#ifdef CONFIG_KSU_LEGACY_4_19
    if (ksud_execve_enabled)
#else
    if (static_branch_unlikely(&ksud_execve_key))
#endif
        ksu_execve_hook_ksud(regs);

    if (current_euid().val == 0)
        pending_root_execve = ksu_sulog_capture_root_execve(*filename_user, argv_user, GFP_KERNEL);

    if (current->pid != 1 && current_is_init) {
        ksu_handle_init_mark_tracker(filename_user);
        ret = ksu_adb_root_handle_execve((struct pt_regs *)regs);
        if (ret) {
            pr_err("adb root failed: %ld\n", ret);
        }
    } else if (ksu_su_compat_enabled) {
        ret = ksu_handle_execve_sucompat(filename_user, orig_nr, regs);
        ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
        return ret;
    }

    ret = ksu_get_original_syscall(orig_nr)(regs);
    ksu_sulog_emit_pending(pending_root_execve, ret, GFP_KERNEL);
    return ret;
}

long __nocfi ksu_hook_setresuid(int orig_nr, const struct pt_regs *regs)
{
    uid_t old_uid = current_uid().val;
    long ret = ksu_get_original_syscall(orig_nr)(regs);

    if (ret < 0)
        return ret;

    ksu_handle_setresuid(old_uid, current_uid().val);
    return ret;
}
