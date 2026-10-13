/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "scan_walk.h"

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* Avoid cycles/huge directory trees on external disks. This is a limit on
 * nested subdirectories, not on the number of .pkg files or games. */
#define MAX_PKG_SCAN_DEPTH 16

static int is_pkg(const char *name) {
    size_t n = strlen(name);
    return n > 4 && !strcasecmp(name + n - 4, ".pkg");
}

static int managed_title(const char *name) {
    if (strlen(name) != 9 || strncmp(name, "CUSA", 4))
        return 0;
    for (int i = 4; i < 9; i++)
        if (name[i] < '0' || name[i] > '9')
            return 0;
    return 1;
}

static int is_library(const char *path, const struct stat *dir, const char *library) {
    struct stat st;
    if (stat(library, &st) || st.st_dev != dir->st_dev)
        return 0;
    /* exFAT implementations may report zero inode numbers; then compare the
     * directory path case-insensitively (as exFAT itself does). */
    if (st.st_ino && dir->st_ino)
        return st.st_ino == dir->st_ino;
    return !strcasecmp(path, library);
}

static int walk(const char *dir, const char *library, dev_t device,
                pkg_dir_visitor visit, void *context, int stop_on_first, int depth) {
    struct stat here;
    /* lstat prevents links to other folders (including parent loops). */
    if (lstat(dir, &here) || !S_ISDIR(here.st_mode) || here.st_dev != device)
        return 0;
    DIR *d = opendir(dir);
    if (!d)
        return 0;

    int result = 0, found_pkg = 0;
    struct dirent *e;
    char path[PATH_MAX];
    /* Check actual files, not merely names ending in .pkg on directories. */
    while ((e = readdir(d))) {
        if (!is_pkg(e->d_name))
            continue;
        int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        struct stat file;
        if (n > 0 && (size_t)n < sizeof(path) && !lstat(path, &file) && S_ISREG(file.st_mode)) {
            found_pkg = 1;
            break;
        }
    }
    if (found_pkg) {
        int n = visit(dir, context);
        if (n > 0)
            result += n;
        if (stop_on_first && result) {
            closedir(d);
            return result;
        }
    }

    if (depth < MAX_PKG_SCAN_DEPTH) {
        int in_library = is_library(dir, &here, library);
        rewinddir(d);
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            /* Directories created by Windows, not game packages. */
            if (!strcasecmp(e->d_name, "System Volume Information") ||
                !strcasecmp(e->d_name, "$RECYCLE.BIN"))
                continue;
            /* The canonical game folders already have the expected layout.
             * Do not re-adopt their app, patch, DLC or replaced packages. */
            if (in_library && managed_title(e->d_name))
                continue;
            int n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            if (n <= 0 || (size_t)n >= sizeof(path))
                continue;
            struct stat child;
            if (lstat(path, &child) || !S_ISDIR(child.st_mode) || child.st_dev != device)
                continue;
            result += walk(path, library, device, visit, context, stop_on_first, depth + 1);
            if (stop_on_first && result)
                break;
        }
    }
    closedir(d);
    return result;
}

int walk_pkg_dirs(const char *start, const char *library, pkg_dir_visitor visit,
                  void *context, int stop_on_first) {
    struct stat st;
    if (!visit || lstat(start, &st) || !S_ISDIR(st.st_mode))
        return 0;
    return walk(start, library, st.st_dev, visit, context, stop_on_first, 0);
}
