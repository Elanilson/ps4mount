#pragma once
#include <stdint.h>
#include <sys/types.h>
uint32_t kernel_get_fw_version(void);
int kernel_dynlib_handle(pid_t pid, const char *name, uint32_t *handle);
void *kernel_dynlib_dlsym(pid_t pid, uint32_t handle, const char *symbol);

void *kernel_dynlib_resolve(pid_t pid, uint32_t handle, const char *symbol);
