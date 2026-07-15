#ifndef __KSU_H_ADB_ROOT
#define __KSU_H_ADB_ROOT
#include <asm/ptrace.h>

#ifdef CONFIG_KSU_LEGACY_4_19
static inline long ksu_adb_root_handle_execve(struct pt_regs *regs) { return 0; }
static inline void ksu_adb_root_init(void) {}
static inline void ksu_adb_root_exit(void) {}
#else
long ksu_adb_root_handle_execve(struct pt_regs *regs);

void ksu_adb_root_init(void);

void ksu_adb_root_exit(void);
#endif

#endif
