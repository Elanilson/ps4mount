#pragma once
#include <stddef.h>
#include <sys/types.h>
struct statfs {
    char f_mntonname[1024];
    char f_mntfromname[1024];
    char f_fstypename[64];
    unsigned long f_flags;
};
struct iovec;
#define MNT_NOWAIT 2
#define MNT_FORCE 0x80000
#define MNT_RDONLY 1
int statfs(const char *path, struct statfs *buf);
int getfsstat(struct statfs *buf, long bufsize, int flags);
int nmount(struct iovec *iov, unsigned int niov, int flags);
int unmount(const char *path, int flags);
