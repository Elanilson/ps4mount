/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Web panel interface. */
#pragma once

#include "build.h"

#define WEB_PORT 8770

/* Implemented by main.c for the web panel. */
void app_games_json(buf_t *out);
int app_plan_json(const char *tid, buf_t *out);
int app_launch_check_json(const char *tid, buf_t *out);
int app_probe_preview_json(const char *tid, buf_t *out);
int app_probe_mount(const char *tid, const char *nonce, char *msg, size_t cap);
int app_probe_release(const char *tid, const char *nonce, char *msg, size_t cap);
int app_register_preview_json(const char *tid, buf_t *out);
int app_register_one(const char *tid, const char *nonce, char *msg, size_t cap);
int app_organize(const char *tid, const char *nonce, char *msg, size_t cap);
int app_recovery_json(int usb, buf_t *out);
int app_rollback(int usb, const char *nonce, char *msg, size_t cap);
int app_add(const char *tid, char *msg, size_t cap);
int app_remove(const char *tid, char *msg, size_t cap);
void app_rescan(void);
void app_stop(void);
/* Fills an icon0.png for tid; 0 on success. */
int app_icon(const char *tid, buf_t *out);

int web_listen(void);
/* Serves one connection on the listening socket. */
void web_serve(int listen_fd);

/* Phase 4H: daemon-owned sequential queue, not browser-controlled. */
int app_batch_preview_json(int limit, buf_t *out);
int app_batch_start(int limit,const char *token,char *msg,size_t cap);
void app_batch_status_json(buf_t *out);
int app_batch_active(void);

/* Safe catalog synchronization status; GET-only. */
void app_sync_status_json(buf_t *out);
