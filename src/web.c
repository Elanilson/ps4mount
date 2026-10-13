/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 gasb07
 * Minimal HTTP server for the PS4Mount panel. */
/* Minimal HTTP server for the PS4Mount panel (one request per connection). */
#include "web.h"

#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "page_data.h"

#define LOG_PATH "/data/ps4mount/log.txt"

int web_listen(void) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons(WEB_PORT);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 8)) {
        if (s >= 0)
            close(s);
        return -1;
    }
    return s;
}

static void send_all(int fd, const void *p, size_t n) {
    const char *c = p;
    while (n) {
        ssize_t w = write(fd, c, n);
        if (w <= 0)
            return;
        c += w;
        n -= (size_t)w;
    }
}

static void reply(int fd, const char *status, const char *type, const void *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, type, len);
    send_all(fd, head, (size_t)n);
    send_all(fd, body, len);
}

static void json_msg(int fd, int ok, const char *msg) {
    buf_t b = {0};
    buf_printf(&b, "{\"ok\":%s,\"%s\":\"", ok ? "true" : "false", ok ? "message" : "error");
    for (const char *p = msg; *p; p++) {
        if (*p == '"' || *p == '\\')
            buf_put(&b, "\\", 1);
        if ((unsigned char)*p >= 0x20)
            buf_put(&b, p, 1);
    }
    buf_printf(&b, "\"}");
    reply(fd, ok ? "200 OK" : "400 Bad Request", "application/json; charset=utf-8", b.data, b.len);
    buf_free(&b);
}

static int title_param(const char *path, char *tid) {
    const char *p = strstr(path, "title_id=");
    if (!p)
        return -1;
    p += 9;
    for (int i = 0; i < 9; i++) {
        char c = p[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
            return -1;
        tid[i] = c;
    }
    tid[9] = 0;
    return strncmp(tid, "CUSA", 4) ? -1 : 0;
}

/* Only same-origin JS with an unpredictable short-lived token can issue a
 * write request. Browsers cannot set this custom header cross-site without
 * CORS preflight, and this server never opts in to cross-origin requests. */
static int confirmation_header(const char *req, char token[33]) {
    const char header[] = "X-PS4Mount-Token:";
    const char *line = strstr(req, "\r\n");
    while (line) {
        line += 2;
        const char *end = strstr(line, "\r\n");
        if (!end) return -1;
        if (!strncasecmp(line, header, sizeof(header) - 1)) {
            const char *p = line + sizeof(header) - 1;
            if (p < end && *p == ' ') p++;
            if (end - p != 32) return -1;
            for (int i = 0; i < 32; i++) {
                char c = p[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
                token[i] = c;
            }
            token[32] = 0;
            return 0;
        }
        line = end;
    }
    return -1;
}
static int usb_param(const char *path) {
    const char *p = strstr(path, "?usb=");
    if (!p || p[5] < '0' || p[5] > '7' || (p[6] && p[6] != '&')) return -1;
    return p[5] - '0';
}

static int batch_limit_param(const char *path) {
    const char *p = strstr(path,"?limit=");
    if (!p) return -1;
    if (!strncmp(p,"?limit=3",8) && (!p[8] || p[8]=='&')) return 3;
    if (!strncmp(p,"?limit=5",8) && (!p[8] || p[8]=='&')) return 5;
    return -1;
}

static void send_log(int fd) {
    buf_t b = {0}, j = {0};
    int f = open(LOG_PATH, O_RDONLY);
    if (f >= 0) {
        off_t end = lseek(f, 0, SEEK_END), start = end > 16384 ? end - 16384 : 0;
        lseek(f, start, SEEK_SET);
        char tmp[4096];
        ssize_t n;
        while ((n = read(f, tmp, sizeof(tmp))) > 0)
            buf_put(&b, tmp, (size_t)n);
        close(f);
    }
    buf_printf(&j, "{\"log\":\"");
    for (size_t i = 0; i < b.len; i++) {
        char c = (char)b.data[i];
        if (c == '\n')
            buf_put(&j, "\\n", 2);
        else if (c == '"' || c == '\\') {
            buf_put(&j, "\\", 1);
            buf_put(&j, &c, 1);
        } else if ((unsigned char)c >= 0x20)
            buf_put(&j, &c, 1);
    }
    buf_printf(&j, "\"}");
    reply(fd, "200 OK", "application/json; charset=utf-8", j.data, j.len);
    buf_free(&b);
    buf_free(&j);
}

void web_serve(int listen_fd) {
    int fd = accept(listen_fd, NULL, NULL);
    if (fd < 0)
        return;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char req[4096];
    ssize_t n = read(fd, req, sizeof(req) - 1);
    if (n <= 0) {
        close(fd);
        return;
    }
    req[n] = 0;
    char method[8] = "", path[256] = "", tid[10], msg[256];
    sscanf(req, "%7s %255s", method, path);
    int post = !strcmp(method, "POST");

    /* During a daemon batch, reject unrelated write endpoints from other tabs.
     * GETs, batch status and the existing PS5 game launch monitor remain usable. */
    if (post && app_batch_active() && strncmp(path,"/api/batch-",11)) {
        json_msg(fd,0,"Backend batch is running. Other write operations are blocked");
    } else if (!post && !strcmp(path,"/api/sync-status")) {
        buf_t b={0};app_sync_status_json(&b);
        reply(fd,"200 OK","application/json; charset=utf-8",b.data,b.len);buf_free(&b);
    } else if (!post && !strcmp(path,"/api/batch-status")) {
        buf_t b={0};app_batch_status_json(&b);
        reply(fd,"200 OK","application/json; charset=utf-8",b.data,b.len);buf_free(&b);
    } else if (!post && !strncmp(path,"/api/batch-preview?",19)) {
        int limit=batch_limit_param(path);
        if(limit<0) json_msg(fd,0,"Batch limit must be 3 or 5");
        else {buf_t b={0};app_batch_preview_json(limit,&b);
              reply(fd,"200 OK","application/json; charset=utf-8",b.data,b.len);buf_free(&b);}
    } else if (post && !strncmp(path,"/api/batch-start?",17)) {
        int limit=batch_limit_param(path);char token[33];
        if(limit<0 || confirmation_header(req,token))json_msg(fd,0,"Invalid batch confirmation");
        else {int rc=app_batch_start(limit,token,msg,sizeof(msg));json_msg(fd,!rc,msg);}
    } else if (!strcmp(path, "/") || !strcmp(path, "/index.html")) {
        reply(fd, "200 OK", "text/html; charset=utf-8", index_html, sizeof(index_html));
    } else if (!strcmp(path, "/api/games")) {
        buf_t b = {0};
        app_games_json(&b);
        reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
        buf_free(&b);
    } else if (!strncmp(path, "/api/plan?", 10)) {
        if (title_param(path, tid)) {
            json_msg(fd, 0, "invalid title_id");
        } else {
            buf_t b = {0};
            int rc = app_plan_json(tid, &b);
            reply(fd, rc ? "400 Bad Request" : "200 OK", "application/json; charset=utf-8", b.data, b.len);
            buf_free(&b);
        }
    } else if (!post && !strncmp(path, "/api/launch-check?", 18)) {
        if (title_param(path, tid)) json_msg(fd, 0, "invalid title_id");
        else {
            buf_t b = {0};
            app_launch_check_json(tid, &b);
            reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
            buf_free(&b);
        }
    } else if (!post && !strncmp(path, "/api/probe-preview?", 19)) {
        if (title_param(path, tid)) json_msg(fd, 0, "invalid title_id");
        else {
            buf_t b = {0};
            app_probe_preview_json(tid, &b);
            reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
            buf_free(&b);
        }
    } else if (!post && !strncmp(path, "/api/register-preview?", 22)) {
        if (title_param(path, tid)) json_msg(fd, 0, "invalid title_id");
        else {
            buf_t b = {0};
            app_register_preview_json(tid, &b);
            reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
            buf_free(&b);
        }
    } else if (post && !strncmp(path, "/api/register-one?", 18)) {
        char token[33];
        if (title_param(path, tid) || confirmation_header(req, token))
            json_msg(fd, 0, "Missing/invalid single-title confirmation");
        else {
            int rc = app_register_one(tid, token, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && (!strncmp(path, "/api/probe-mount?", 17) ||
                        !strncmp(path, "/api/probe-release?", 19))) {
        char token[33];
        if (title_param(path, tid) || confirmation_header(req, token))
            json_msg(fd, 0, "Missing/invalid CUSA or single-use confirmation token");
        else {
            int rc = !strncmp(path, "/api/probe-mount?", 17) ?
                     app_probe_mount(tid, token, msg, sizeof(msg)) :
                     app_probe_release(tid, token, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/organize?", 14)) {
        char token[33];
        /* Log the incoming action even when confirmation is malformed or expired.
         * Never log the secret single-use token. */
        int valid_tid = !title_param(path, tid);
        printf("[safe][organize-http] POST %s (token not logged)\n", valid_tid ? tid : "invalid-title");
        if (!valid_tid || confirmation_header(req, token))
            json_msg(fd, 0, "Missing/invalid title id or confirmation token");
        else {
            int rc = app_organize(tid, token, msg, sizeof(msg));
            printf("[safe][organize-http] %s %s: %s\n", tid, rc ? "REJECTED" : "ACCEPTED", msg);
            json_msg(fd, !rc, msg);
        }
    } else if (!post && !strncmp(path, "/api/recovery?", 14)) {
        int usb = usb_param(path);
        if (usb < 0) json_msg(fd, 0, "Invalid USB index");
        else {
            buf_t b = {0};
            int rc = app_recovery_json(usb, &b);
            if (rc < 0) json_msg(fd, 0, "Recovery journal cannot be inspected safely or USB missing");
            else reply(fd, "200 OK", "application/json; charset=utf-8", b.data, b.len);
            buf_free(&b);
        }
    } else if (post && !strncmp(path, "/api/rollback?", 14)) {
        int usb = usb_param(path);
        char token[33];
        if (usb < 0 || confirmation_header(req, token)) json_msg(fd, 0, "Invalid USB index or confirmation token");
        else {
            int rc = app_rollback(usb, token, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (!strncmp(path, "/icon/", 6)) {
        buf_t b = {0};
        snprintf(tid, sizeof(tid), "%.9s", path + 6);
        if (!app_icon(tid, &b))
            reply(fd, "200 OK", "image/png", b.data, b.len);
        else
            reply(fd, "404 Not Found", "text/plain", "no icon\n", 8);
        buf_free(&b);
    } else if (post && !strncmp(path, "/api/mount", 10)) {
        if (title_param(path, tid))
            json_msg(fd, 0, "invalid title_id");
        else {
            int rc = app_add(tid, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strncmp(path, "/api/unmount", 12)) {
        if (title_param(path, tid))
            json_msg(fd, 0, "invalid title_id");
        else {
            int rc = app_remove(tid, msg, sizeof(msg));
            json_msg(fd, !rc, msg);
        }
    } else if (post && !strcmp(path, "/api/rescan")) {
        app_rescan();
        json_msg(fd, 1, "Scanning USB drives for games");
    } else if (!strcmp(path, "/api/log")) {
        send_log(fd);
    } else if (post && !strcmp(path, "/api/stop")) {
        json_msg(fd, 1, "PS4Mount is stopping");
        app_stop();
    } else {
        reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
    }
    close(fd);
}
