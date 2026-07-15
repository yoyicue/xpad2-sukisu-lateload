#ifndef __KSU_H_KERNEL_UMOUNT
#define __KSU_H_KERNEL_UMOUNT

#include <linux/types.h>
#include <linux/list.h>
#include <linux/rwsem.h>

#ifdef CONFIG_KSU_LEGACY_4_19
static inline void ksu_kernel_umount_init(void) {}
static inline void ksu_kernel_umount_exit(void) {}
static inline int ksu_handle_umount(uid_t old_uid, uid_t new_uid) { return 0; }
#else
void ksu_kernel_umount_init(void);
void ksu_kernel_umount_exit(void);

// Handler function to be called from setresuid hook
int ksu_handle_umount(uid_t old_uid, uid_t new_uid);
#endif

// for the umount list
struct mount_entry {
    char *umountable;
    unsigned int flags;
    struct list_head list;
};
extern struct list_head mount_list;
extern struct rw_semaphore mount_list_lock;

#endif
