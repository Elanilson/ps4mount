/* SPDX-License-Identifier: GPL-3.0-or-later
 * IMPORTANT: These are hypotheses, not verified DLC registration APIs.
 * Every variant calls the targeted AppInstallTitleDir service once; none calls
 * AppInstallAll, InstallByPackage, uninstall or SQLite write APIs.
 */
#include "dlc_probe.h"
#include <stdio.h>
#include <string.h>

const char *dlc_probe_name(int method) {
    switch (method) {
    case 1: return "BASE_APP_ROOT_CONTROL";
    case 2: return "BASE_ADDCONT_ROOT";
    case 3: return "DLC_TITLE_ADDCONT_ROOT";
    case 4: return "DLC_TITLE_SPECIFIC_ROOT";
    case 5: return "DLC_TITLE_LABEL_ROOT";
    default: return "UNKNOWN";
    }
}
static int valid_tid(const char *s) {
    if (!s || strlen(s) != 9 || strncmp(s, "CUSA", 4)) return 0;
    for (int i=0; i<9; i++)
        if (!((s[i]>='A' && s[i]<='Z') || (s[i]>='0' && s[i]<='9'))) return 0;
    return 1;
}
static int valid_label(const char *s) {
    if (!s || strlen(s)!=16) return 0;
    for (int i=0;i<16;i++)
        if (!((s[i]>='A' && s[i]<='Z') || (s[i]>='0' && s[i]<='9'))) return 0;
    return 1;
}
int dlc_probe_target(int method,const char *game,const char *dlc_title,
                     const char *label,dlc_probe_target_t *out) {
    if (!out || !valid_tid(game) || !valid_tid(dlc_title) || !valid_label(label)) return -1;
    memset(out,0,sizeof(*out));
    const char *tid = method==1 || method==2 ? game : dlc_title;
    if (method < 1 || method > DLC_PROBE_METHODS) return -1;
    memcpy(out->title, tid, 9);
    switch (method) {
    case 1: snprintf(out->root,sizeof(out->root),"/user/app/"); break;
    case 2: case 3: snprintf(out->root,sizeof(out->root),"/user/addcont/"); break;
    case 4: snprintf(out->root,sizeof(out->root),"/user/addcont/%s/",dlc_title); break;
    case 5: snprintf(out->root,sizeof(out->root),"/user/addcont/%s/%s/",dlc_title,label); break;
    }
    return 0;
}
