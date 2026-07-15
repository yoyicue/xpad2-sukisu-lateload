#ifdef __aarch64__

#include "../syscall_hook.h"

#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <asm/cacheflush.h>
#include "infra/symbol_resolver.h"
#include "../patch_memory.h"
#include "../syscall_event_bridge.h"
#include "arch.h"
#include "supercall/supercall.h"
#include "klog.h" // IWYU pragma: keep

syscall_fn_t *ksu_syscall_table = NULL;
int ksu_dispatcher_nr = -1;

// Hook registration table — read with READ_ONCE from tracepoint/dispatcher
// context, written with WRITE_ONCE from init/exit context.
static ksu_syscall_hook_fn syscall_hooks[__NR_syscalls];

// Track all hooked syscall entries for restoration.
// Protected by hooked_entries_lock.
struct syscall_hook_entry {
    int nr;
    syscall_fn_t orig;
};

static DEFINE_MUTEX(hooked_entries_lock);
static struct syscall_hook_entry hooked_entries[16];
static int hooked_count = 0;

#ifdef CONFIG_KSU_LEGACY_4_19
static atomic_t ksu_legacy_active = ATOMIC_INIT(0);
static DECLARE_WAIT_QUEUE_HEAD(ksu_legacy_drain_wait);
static bool ksu_legacy_exiting;

void ksu_syscall_hook_begin_exit(void)
{
    WRITE_ONCE(ksu_legacy_exiting, true);
    smp_mb();
}

static long ksu_legacy_dispatch(int nr, ksu_syscall_hook_fn fn, const struct pt_regs *regs)
{
    syscall_fn_t orig;
    long ret;

    atomic_inc(&ksu_legacy_active);
    smp_mb__after_atomic();
    orig = ksu_get_original_syscall(nr);
    if (unlikely(READ_ONCE(ksu_legacy_exiting)))
        ret = orig(regs);
    else
        ret = fn(nr, regs);
    if (atomic_dec_and_test(&ksu_legacy_active))
        wake_up_all(&ksu_legacy_drain_wait);
    return ret;
}
#else
void ksu_syscall_hook_begin_exit(void)
{
}
#endif

syscall_fn_t ksu_get_original_syscall(int nr)
{
    int i;

    if (!ksu_syscall_table || nr < 0 || nr >= __NR_syscalls)
        return NULL;
    for (i = 0; i < READ_ONCE(hooked_count); i++) {
        if (READ_ONCE(hooked_entries[i].nr) == nr)
            return READ_ONCE(hooked_entries[i].orig);
    }
    return READ_ONCE(ksu_syscall_table[nr]);
}

#ifdef CONFIG_KSU_LEGACY_4_19
static unsigned long ksu_decode_bl_target(unsigned long pc, u32 insn)
{
    s64 off;

    if ((insn & 0xfc000000) != 0x94000000)
        return 0;
    off = sign_extend64(insn & 0x03ffffff, 25) << 2;
    return pc + off;
}

static syscall_fn_t *ksu_legacy_find_syscall_table(void)
{
    unsigned long entry, handler = 0;
    u32 insn[32];
    int i;

    entry = find_kernel_symbol_exact("el0_svc");
    if (!entry || probe_kernel_read(insn, (void *)entry, 4 * sizeof(u32)))
        return NULL;

    for (i = 0; i < 4; i++) {
        handler = ksu_decode_bl_target(entry + i * sizeof(u32), insn[i]);
        if (handler)
            break;
    }
    if (!handler || probe_kernel_read(insn, (void *)handler, sizeof(insn)))
        return NULL;

    for (i = 0; i < ARRAY_SIZE(insn) - 1; i++) {
        u32 adrp = insn[i];
        u32 add = insn[i + 1];
        s64 page_off;
        unsigned long page, imm;

        if ((adrp & 0x9f00001f) != 0x90000002)
            continue;
        if ((add & 0xffc003ff) != 0x91000042)
            continue;

        page_off = sign_extend64((((adrp >> 5) & 0x7ffff) << 2) |
                                 ((adrp >> 29) & 3), 20) << 12;
        page = ((handler + i * sizeof(u32)) & PAGE_MASK) + page_off;
        imm = (add >> 10) & 0xfff;
        if (add & BIT(22))
            imm <<= 12;
        pr_info("legacy decoded sys_call_table from el0_svc handler: 0x%lx\n",
                page + imm);
        return (syscall_fn_t *)(page + imm);
    }

    return NULL;
}

static long __nocfi ksu_legacy_setresuid(const struct pt_regs *regs)
{
    return ksu_legacy_dispatch(__NR_setresuid, ksu_hook_setresuid, regs);
}

static long __nocfi ksu_legacy_execve(const struct pt_regs *regs)
{
    return ksu_legacy_dispatch(__NR_execve, ksu_hook_execve, regs);
}

static long __nocfi ksu_legacy_newfstatat(const struct pt_regs *regs)
{
    return ksu_legacy_dispatch(__NR_newfstatat, ksu_hook_newfstatat, regs);
}

static long __nocfi ksu_legacy_faccessat(const struct pt_regs *regs)
{
    return ksu_legacy_dispatch(__NR_faccessat, ksu_hook_faccessat, regs);
}

static syscall_fn_t ksu_legacy_orig_reboot;

static long __nocfi ksu_legacy_reboot(const struct pt_regs *regs)
{
    long ret;

    atomic_inc(&ksu_legacy_active);
    smp_mb__after_atomic();
    if (unlikely(READ_ONCE(ksu_legacy_exiting)))
        ret = ksu_legacy_orig_reboot(regs);
    else if (ksu_handle_reboot_supercall_direct(regs))
        ret = 0;
    else
        ret = ksu_legacy_orig_reboot(regs);
    if (atomic_dec_and_test(&ksu_legacy_active))
        wake_up_all(&ksu_legacy_drain_wait);
    return ret;
}

static syscall_fn_t ksu_legacy_adapter(int nr)
{
    switch (nr) {
    case __NR_setresuid:
        return ksu_legacy_setresuid;
    case __NR_execve:
        return ksu_legacy_execve;
    case __NR_newfstatat:
        return ksu_legacy_newfstatat;
    case __NR_faccessat:
        return ksu_legacy_faccessat;
    default:
        return NULL;
    }
}
#endif

static int patch_syscall_table(int nr, syscall_fn_t fn)
{
    if (ksu_syscall_table == NULL)
        return -ENOENT;
    if (nr < 0 || nr >= __NR_syscalls)
        return -EINVAL;

    pr_info("patch syscall %d, 0x%lx -> 0x%lx\n", nr, (unsigned long)READ_ONCE(ksu_syscall_table[nr]),
            (unsigned long)fn);

    if (ksu_patch_text(&ksu_syscall_table[nr], &fn, sizeof(fn), KSU_PATCH_TEXT_FLUSH_DCACHE)) {
        pr_err("patch syscall %d failed\n", nr);
        return -EIO;
    }

    return 0;
}

// Direct syscall table patching: overwrite syscall_table[nr] with fn,
// save original to *old, and record for restoration at module exit.
void ksu_syscall_table_hook(int nr, syscall_fn_t fn, syscall_fn_t *old)
{
    if (ksu_syscall_table == NULL)
        return;
    if (nr < 0 || nr >= __NR_syscalls) {
        pr_info("invalid nr: %d\n", nr);
        return;
    }

    mutex_lock(&hooked_entries_lock);

    syscall_fn_t orig = READ_ONCE(ksu_syscall_table[nr]);
    if (old)
        *old = orig;

    if (patch_syscall_table(nr, fn)) {
        mutex_unlock(&hooked_entries_lock);
        return;
    }

    // Record for later restoration
    int i;
    bool found = false;
    for (i = 0; i < hooked_count; i++) {
        if (hooked_entries[i].nr == nr) {
            found = true;
            break;
        }
    }
    if (!found) {
        if (hooked_count < ARRAY_SIZE(hooked_entries)) {
            hooked_entries[hooked_count].nr = nr;
            hooked_entries[hooked_count].orig = orig;
            hooked_count++;
        } else {
            pr_warn("hooked_entries full, cannot track syscall %d for restoration\n", nr);
        }
    }

    mutex_unlock(&hooked_entries_lock);
}

// Restore syscall_table[nr] to its original value and remove from tracking list.
void ksu_syscall_table_unhook(int nr)
{
    int i;

    if (ksu_syscall_table == NULL)
        return;
    if (nr < 0 || nr >= __NR_syscalls)
        return;

    mutex_lock(&hooked_entries_lock);

    for (i = 0; i < hooked_count; i++) {
        if (hooked_entries[i].nr == nr) {
            patch_syscall_table(nr, hooked_entries[i].orig);
            // Remove entry by swapping with last
            hooked_entries[i] = hooked_entries[--hooked_count];
            mutex_unlock(&hooked_entries_lock);
            pr_info("unhooked syscall %d\n", nr);
            return;
        }
    }

    mutex_unlock(&hooked_entries_lock);
    pr_warn("syscall %d not found in hooked entries\n", nr);
}

static int __init ksu_find_ni_syscall_slots(int *out_slots, int max_slots)
{
    unsigned long ni_syscall;
    int i, count = 0;

    if (!ksu_syscall_table || max_slots <= 0)
        return 0;

    ni_syscall = (unsigned long)ksu_resolve_symbol_for_functable_hook("__arm64_sys_ni_syscall");

    pr_info("sys_ni_syscall: 0x%lx\n", ni_syscall);

    if (!ni_syscall)
        return 0;

    for (i = 0; i < __NR_syscalls && count < max_slots; i++) {
        if ((unsigned long)ksu_syscall_table[i] == ni_syscall) {
            out_slots[count++] = i;
            pr_info("ni_syscall %d: %d\n", count, i);
        }
    }

    return count;
}

// Unified dispatcher: reads original NR from x8, dispatches to handler.
// Validates that syscallno matches our dispatcher slot (i.e. we redirected it),
// otherwise it's a spurious call — return -ENOSYS.
static long __nocfi ksu_syscall_dispatcher(const struct pt_regs *regs)
{
    if (regs->syscallno != ksu_dispatcher_nr)
        return -ENOSYS;

    int orig_nr = (int)PT_REGS_ORIG_SYSCALL(regs);

    if (regs->syscallno == orig_nr)
        return -ENOSYS;

    // Restore registers to original state before dispatching
    ((struct pt_regs *)regs)->syscallno = orig_nr;
    PT_REGS_ORIG_SYSCALL((struct pt_regs *)regs) = orig_nr;

    if (likely(orig_nr >= 0 && orig_nr < __NR_syscalls)) {
        ksu_syscall_hook_fn fn = READ_ONCE(syscall_hooks[orig_nr]);
        if (likely(fn))
            return fn(orig_nr, regs);
    }

    return -ENOSYS;
}

// Register a handler into the dispatcher's routing table.
// Does not modify the syscall table — the dispatcher slot is shared by all hooks.
int ksu_register_syscall_hook(int nr, ksu_syscall_hook_fn fn)
{
    if (nr < 0 || nr >= __NR_syscalls)
        return -EINVAL;
    if (READ_ONCE(syscall_hooks[nr])) {
        pr_warn("syscall hook for nr=%d already registered, skip\n", nr);
        return -EEXIST;
    }
    WRITE_ONCE(syscall_hooks[nr], fn);
#ifdef CONFIG_KSU_LEGACY_4_19
    {
        syscall_fn_t adapter = ksu_legacy_adapter(nr);
        if (!adapter) {
            WRITE_ONCE(syscall_hooks[nr], NULL);
            return -EINVAL;
        }
        ksu_syscall_table_hook(nr, adapter, NULL);
    }
#endif
    pr_info("registered syscall hook for nr=%d\n", nr);
    return 0;
}

// Remove a handler from the dispatcher's routing table.
// The syscall table is not touched — only the dispatcher stops routing this nr.
void ksu_unregister_syscall_hook(int nr)
{
    if (nr < 0 || nr >= __NR_syscalls)
        return;
#ifdef CONFIG_KSU_LEGACY_4_19
    ksu_syscall_table_unhook(nr);
#endif
    WRITE_ONCE(syscall_hooks[nr], NULL);
    pr_info("unregistered syscall hook for nr=%d\n", nr);
}

bool ksu_has_syscall_hook(int nr)
{
    if (nr < 0 || nr >= __NR_syscalls)
        return false;
    return READ_ONCE(syscall_hooks[nr]) != NULL;
}

void __init ksu_syscall_hook_init(void)
{
#ifndef CONFIG_KSU_LEGACY_4_19
    int ni_slot;
#endif

    memset(syscall_hooks, 0, sizeof(syscall_hooks));
#ifdef CONFIG_KSU_LEGACY_4_19
    WRITE_ONCE(ksu_legacy_exiting, false);
    atomic_set(&ksu_legacy_active, 0);
#endif

    ksu_syscall_table = (syscall_fn_t *)ksu_resolve_symbol_for_functable_hook("sys_call_table");
#ifdef CONFIG_KSU_LEGACY_4_19
    if (!ksu_syscall_table)
        ksu_syscall_table = ksu_legacy_find_syscall_table();
#endif
    pr_info("sys_call_table=0x%lx", (unsigned long)ksu_syscall_table);

    if (!ksu_syscall_table)
        return;

#ifndef CONFIG_KSU_LEGACY_4_19
    // Find one ni_syscall slot for the dispatcher
    if (ksu_find_ni_syscall_slots(&ni_slot, 1) < 1) {
        pr_err("failed to find ni_syscall slot for dispatcher\n");
        return;
    }

    ksu_dispatcher_nr = ni_slot;
    ksu_syscall_table_hook(ksu_dispatcher_nr, (syscall_fn_t)ksu_syscall_dispatcher, NULL);
    pr_info("dispatcher installed at slot %d\n", ksu_dispatcher_nr);
#else
    pr_info("legacy 4.19 direct syscall table mode\n");
    ksu_syscall_table_hook(__NR_reboot, ksu_legacy_reboot,
                           &ksu_legacy_orig_reboot);
#endif
}

void __exit ksu_syscall_hook_exit(void)
{
    int i;

    if (!ksu_syscall_table)
        goto clear_state;

    // First, restore all patched syscall table entries while the dispatcher
    // and hook table are still intact, so in-flight syscalls see valid state.
    mutex_lock(&hooked_entries_lock);
    for (i = 0; i < hooked_count; i++) {
        int nr = hooked_entries[i].nr;
        syscall_fn_t orig = hooked_entries[i].orig;

        pr_info("restore syscall %d to 0x%lx\n", nr, (unsigned long)orig);
        if (ksu_patch_text(&ksu_syscall_table[nr], &orig, sizeof(orig), KSU_PATCH_TEXT_FLUSH_DCACHE)) {
            pr_err("restore syscall %d failed\n", nr);
        }
    }
    hooked_count = 0;
    mutex_unlock(&hooked_entries_lock);

#ifdef CONFIG_KSU_LEGACY_4_19
    wait_event(ksu_legacy_drain_wait, atomic_read(&ksu_legacy_active) == 0);
    pr_info("legacy syscall adapters drained\n");
#endif

clear_state:
    // Now that the syscall table is restored, clear internal state.
    // At this point the tracepoint is already unregistered and synchronized
    // (done by ksu_syscall_hook_manager_exit before calling us), so no new
    // dispatches will occur.
    memset(syscall_hooks, 0, sizeof(syscall_hooks));
    ksu_dispatcher_nr = -1;

    pr_info("all syscall hooks restored\n");
}

#endif /* __aarch64__ */
