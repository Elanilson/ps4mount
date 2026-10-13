/* SPDX-License-Identifier: GPL-3.0-or-later
 * Recursive discovery of folders containing .pkg files on a USB volume.
 * Does not open, move, install, or modify any packages. */
#ifndef PS4MOUNT_SCAN_WALK_H
#define PS4MOUNT_SCAN_WALK_H

typedef int (*pkg_dir_visitor)(const char *dir, void *context);

/* Calls visit once per folder that directly contains one or more .pkg files.
 * Visits subfolders on the same filesystem without following symbolic links.
 * The managed ps4/CUSAxxxxx folders are excluded (they are handled separately).
 * When stop_on_first is set, returns on the first positive callback result.
 * Otherwise returns the sum of all nonnegative callback results. */
typedef void (*pkg_dir_progress)(const char *path, void *context);
/* Identical traversal with a callback for each directory considered.
 * Used only for displaying live progress during explicit USB search. */
int walk_pkg_dirs_progress(const char *start, const char *library, pkg_dir_visitor visit,
                           void *context, int stop_on_first,
                           pkg_dir_progress progress, void *progress_context);

int walk_pkg_dirs(const char *start, const char *library, pkg_dir_visitor visit,
                  void *context, int stop_on_first);

#endif
