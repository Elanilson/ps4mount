/* SPDX-License-Identifier: GPL-3.0-or-later
 * Isolated experimental AppInstallTitleDir request variants. No global installer. */
#pragma once
#include <stddef.h>
#define DLC_PROBE_METHODS 5
/* CUSA title, DLC title ID, 16-char add-on label: inputs verified by caller. */
typedef struct { char title[10]; char root[160]; } dlc_probe_target_t;
const char *dlc_probe_name(int method);
int dlc_probe_target(int method, const char *game, const char *dlc_title,
                     const char *label, dlc_probe_target_t *out);
