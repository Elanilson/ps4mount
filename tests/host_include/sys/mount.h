#ifndef MOCK_MOUNT_H
#define MOCK_MOUNT_H
#include <stddef.h>
#define MNT_RDONLY 1
#define MNT_FORCE 2
#define MNT_NOWAIT 3
struct statfs { char f_mntonname[1024]; char f_fstypename[32]; char f_mntfromname[1024]; unsigned long f_flags; };
int statfs(const char *, struct statfs *);
int getfsstat(struct statfs *, long, int);
int nmount(void *, unsigned int, int);
int unmount(const char *, int);
#endif
