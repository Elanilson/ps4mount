#ifndef PS5_KERNEL_H
#define PS5_KERNEL_H
#include <stdint.h>
uint32_t kernel_get_fw_version(void);
#endif
/* Host-only signature declarations for PS5 loader/NID lookup. */
int kernel_dynlib_handle(int pid, const char *name, uint32_t *handle);
intptr_t kernel_dynlib_resolve(int pid, uint32_t handle, const char *nid);
