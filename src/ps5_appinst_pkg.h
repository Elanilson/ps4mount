/* SPDX-License-Identifier: GPL-3.0-or-later
 * PS5 AppInstUtil ABI, based on OnionHEN docs/pkg-writeup.md. */
#pragma once
#include <stdint.h>

typedef struct {
    char content_id[48];
    int content_type, content_platform;
} ps5_pkg_info_t;
typedef struct {
    const char *uri, *ex_uri, *playgo_scenario_id;
    const char *content_id, *content_name, *icon_url;
} ps5_pkg_meta_t;
typedef struct {
    char languages[30][8];
    char playgo_scenario_ids[64][3];
    char content_ids[64][48];
    unsigned char reserved[6480];
} ps5_playgo_info_t;
typedef struct {
    int32_t error_code, version;
    char description[512];
    char type[9];
} ps5_install_error_t;
typedef struct {
    char status[16], src_type[8];
    uint32_t remain_time;
    uint64_t downloaded_size, initial_chunk_size, total_size;
    uint32_t promote_progress;
    ps5_install_error_t error_info;
    int32_t local_copy_percent;
    _Bool is_copy_only;
} ps5_install_status_t;
int sceAppInstUtilInstallByPackage(const ps5_pkg_meta_t *, ps5_pkg_info_t *, ps5_playgo_info_t *);
int sceAppInstUtilGetInstallStatus(const char *, ps5_install_status_t *);

_Static_assert(sizeof(ps5_pkg_info_t) == 56, "PS5 PKG info ABI mismatch");
_Static_assert(sizeof(ps5_pkg_meta_t) == 48, "PS5 PKG meta ABI mismatch");
_Static_assert(sizeof(ps5_playgo_info_t) == 9984, "PS5 PlayGo info ABI mismatch");
_Static_assert(sizeof(ps5_install_status_t) == 600, "PS5 status ABI mismatch");
