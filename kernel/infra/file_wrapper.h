#ifndef KSU_FILE_WRAPPER_H
#define KSU_FILE_WRAPPER_H

#include <linux/file.h>
#include <linux/fs.h>

#ifdef CONFIG_KSU_LEGACY_4_19
static inline int ksu_install_file_wrapper(int fd)
{
    return -EOPNOTSUPP;
}
static inline void ksu_file_wrapper_init(void) {}
#else
int ksu_install_file_wrapper(int fd);
void ksu_file_wrapper_init(void);
#endif

#endif // KSU_FILE_WRAPPER_H
