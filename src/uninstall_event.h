/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef PS4MOUNT_UNINSTALL_EVENT_H
#define PS4MOUNT_UNINSTALL_EVENT_H

#include <stdio.h>
#include <string.h>

/* Extract the result from '[PlayGoCore][Uninstall] end (CONTENT_ID, 0xXXXXXXXX)'.
 * A missing or unparsable result is NOT evidence that deletion succeeded. */
static inline int ps4mount_uninstall_result(const char *tail, unsigned int *result) {
    if (!tail || !result) return 0;
    const char *comma = strchr(tail, ',');
    const char *close = strchr(tail, ')');
    if (!comma || !close || comma >= close) return 0;
    unsigned int code;
    if (sscanf(comma, ", 0x%x", &code) != 1) return 0;
    *result = code;
    return 1;
}

#endif
