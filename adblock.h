/*
 * adblock — local blocking proxy for uBlock-like content filtering.
 *
 * WebKitGTK 2.54 (webkit2gtk-4.1) has no request-interception API
 * (WebKitContentFilter was removed upstream), so ad/tracker blocking is
 * done by routing all web traffic through a local proxy that refuses
 * connections to known ad/tracker hosts.
 */
#pragma once

#include <glib.h>

typedef struct AdbProxy AdbProxy;

AdbProxy *adb_proxy_new(void);
void      adb_proxy_free(AdbProxy *p);

/* Bind 127.0.0.1:0 and start listening. Returns TRUE on success. */
gboolean  adb_proxy_start(AdbProxy *p);
void      adb_proxy_stop(AdbProxy *p);
int       adb_proxy_port(const AdbProxy *p);

/* Load extra blocklist domains from a file (one per line, # comments). */
void      adb_proxy_load_file(AdbProxy *p, const char *path);

/* Total number of requests blocked so far (thread-safe; read from the UI). */
guint64   adb_proxy_blocked_count(const AdbProxy *p);