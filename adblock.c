/*
 * adblock.c — a minimal local HTTP/HTTPS proxy that blocks requests to
 * known ad/tracker hosts. See adblock.h for the rationale.
 *
 * Protocol handling:
 *   CONNECT host:port  -> block (403) if host is listed, else 200 + pipe
 *   GET/POST http://.. -> block (403) if host is listed, else forward + pipe
 */
#include "adblock.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netdb.h>
#include <time.h>
#include <stdarg.h>

/* ---------- blocklist ---------- */

/* Well-known ad networks, ad exchanges and tracking/telemetry hosts.
 * Matching is exact or subdomain (host == entry or host ends with "." + entry).
 *
 * This list is intentionally conservative: only hosts that are unambiguously
 * ad/tracker infrastructure. Extend at runtime with adb_proxy_load_file()
 * (CLI: --blocklist <file>). */
static const char *DEFAULT_BLOCKLIST[] = {
    /* --- ad networks & exchanges --- */
    "adcolony.com", "adform.com", "adgrx.com", "adhese.com", "adkernel.com",
    "adlure.net", "admarvel.com", "adnator.com", "adnetworld.com", "adnxs.com",
    "adpay.com", "adperium.com", "adprime.com", "adqueue.com",
    "adsafeprotected.com", "adscience.com", "adsrvr.org", "adthrive.com",
    "adzerk.net", "amazon-adsystem.com", "amzn-adsystem.com", "admob.com",
    "adap.tv", "adsttc.com", "adtrue.com", "adtrue.eu", "advertising.com",
    "criteo.com", "criteo.net", "criteo.co.uk",
    "doubleclick.com", "doubleclick.net",
    "pubmatic.com", "pubmatic.net",
    "rubiconproject.com",
    "openx.net", "openx.com",
    "indexww.com",
    "casalemedia.com",
    "media.net",
    "smartadserver.com",
    "sharethrough.com",
    "sonobi.com",
    "telaria.com",
    "triplelift.com",
    "xandr.com",
    "zeotap.com",
    "taboola.com",
    "outbrain.com",
    "mopub.com",
    "adswizz.com",
    "bidswitch.com", "bidswitch.net",
    "appnexus.com",
    "applovin.com",
    "fastclick.com", "fastclick.net",
    "exoclick.com", "exosrv.com", "exozest.com",
    "popads.net", "popcash.net", "popunder.ru",
    "pushads.com", "pushengage.com",
    "vntsm.com", "vntsm.io",
    "4tags.com",
    "adalyser.com", "adbalancer.com", "adcash.com", "adcell.com",
    "adchart.com", "adcomplete.com", "adconrad.com", "addelivery.net",
    "adengage.com", "adexchanger.com", "adinfuse.com", "adizio.com",
    "adloop.co", "admagnet.com", "admantx.com", "admedia.com",
    "adopen.com", "adoperator.com", "adparity.com", "adpiano.com",
    "adreactor.com", "adreela.com", "adrunnr.com", "adschoom.com",
    "adserver.co.za", "adserved.com", "adtech.de", "adtech.us",
    "adthis.com", "adtiger.de", "adtrade.net", "adtribon.com",
    "adtx.com", "adunited.com", "adup-tech.com", "adurl.com",
    "advalum.com", "adview.com", "adwhirl.com", "adwox.com",
    "adxcore.com", "adxpose.com",
    "clickadz.com", "clickagents.com", "clickhype.com", "clicknology.com",
    "clickresource.com", "clickstream.com", "clickx.eu",
    "csanadvertising.com", "directrev.com", "domdex.com",
    "dotmetrics.com", "dotmetrics.net",
    "evergent.com", "fwmrm.net", "getclicky.com",
    "grapeshot.co.uk", "grapeshot.com", "growhow.com",
    "hdmedia.com", "heia.com", "heiasc.com",
    "hlllongwang.com", "hotlog.ru", "hotrank.com.tw", "hypertracker.com",
    "ic010.com", "ic015.com", "ic025.com", "ictra.com",
    "improvable.com", "infzm.com", "intentiq.com",
    "interactivemedia.com.au", "ipredictive.com", "iredirect.org",
    "jansse.com", "jipop.com", "jsm1.com", "jsm2.com",
    "jufree.com", "juicyads.com", "k4ys.com", "keyzon.com",
    "kindads.net", "klip.cx", "landiannews.com", "letu.com",
    "lightningcast.net", "linkbuddr.com", "linkimpulse.com",
    "linkprice.com", "linkshrink.net", "linkxtra.com",
    "livehelp.com", "livesmarter.com", "livestack.com", "liverail.com",
    "lucydong.com", "marchex.com", "marketgid.com",
    "marketing.stream", "marketingsolutions.net",
    "maven.sm.cn", "mcdmond.com", "media6degrees.com",
    "mediadivpost.com", "medialytics.com", "medialytics.io",
    "mediamath.com", "mediaplex.com", "mediarithmics.com",
    "mediastatin.com", "mediawang.com", "medibix.com", "medyapoint.com",
    "meitustart.com", "meitutech.com", "melonbook.com", "mgstage.com",
    "microad-cn.com", "microad.jp", "microad.net", "micronet.com.hk",
    "microplanning.com", "midas.info", "midasrevenue.com",
    "midnightclicking.com", "miniserve.com", "mirando.com", "misspot.com",
    "mixpanel.com", "mixpanel.io", "mjres.com", "mktclarity.com",
    "mmstat.com", "moatads.com", "moatpixel.com", "moatimm.com",
    "morgdm.ru", "moxieops.com", "mpj123.com", "mrprnr.com",
    "mtrics.net", "mtrix.net", "mtrixlab.com", "muhanad.com",
    "musicstack.com", "mybestmv.com", "mybloglog.com", "mybloglogs.com",
    "mydal.com",

    /* --- analytics & tracking --- */
    "google-analytics.com", "googletagmanager.com", "googletagservices.com",
    "googleadservices.com", "googlesyndication.com",
    "scorecardresearch.com", "2o7.net",
    "quantserve.com", "quantcount.com",
    "chartbeat.com", "hotjar.com", "segment.com",
    "fbsbx.com", "connect.facebook.net",
    "analytics.twitter.com", "comscore.com",
    NULL
};

struct AdbProxy {
    GSocketService *service;
    GPtrArray *extra_hosts;
    int port;
    gint blocked; /* atomic: total requests refused so far */
};

/* Diagnostics: append one line per event to /tmp/gtkbrowser-audit.log so
 * proxy traffic and the blocked counter can be inspected while the browser
 * runs normally (no GUI needed to read it). Fresh file per process. */
static void audit(const char *fmt, ...)
{
    static FILE *f = NULL;
    static int open_failed = 0;
    if (!f && !open_failed) {
        f = fopen("/tmp/gtkbrowser-audit.log", "w");
        if (!f) {
            open_failed = 1;
            return;
        }
        setvbuf(f, NULL, _IOLBF, 0);
    }
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(f, "%02d:%02d:%02d ", t->tm_hour, t->tm_min, t->tm_sec);
    vfprintf(f, fmt, ap);
    fputc('\n', f);
    va_end(ap);
}

static gboolean host_matches(const char *host, const char *entry)
{
    size_t hl = strlen(host);
    size_t el = strlen(entry);
    if (hl == el)
        return g_ascii_strcasecmp(host, entry) == 0;
    if (hl > el && host[hl - el - 1] == '.')
        return g_ascii_strcasecmp(host + hl - el, entry) == 0;
    return FALSE;
}

static gboolean host_blocked(const AdbProxy *p, const char *host)
{
    if (!host || !*host)
        return FALSE;
    for (int i = 0; DEFAULT_BLOCKLIST[i]; i++)
        if (host_matches(host, DEFAULT_BLOCKLIST[i]))
            return TRUE;
    for (guint i = 0; i < p->extra_hosts->len; i++) {
        const char *e = g_ptr_array_index(p->extra_hosts, i);
        if (host_matches(host, e))
            return TRUE;
    }
    return FALSE;
}

/* ---------- plumbing ---------- */

static void send_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        off += (size_t)w;
    }
}

static void write_all_or_close(int fd, const char *buf, size_t len)
{
    ssize_t off = 0;
    while ((size_t)off < len) {
        ssize_t w = write(fd, buf + off, len - (size_t)off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return;
        }
        off += w;
    }
}

/* Bidirectionally pipe two fds until both directions are closed.
 * Does NOT close the fds; the caller is responsible for closing them
 * (e.g. by unreffing the owning GSocket). */
static void pipe_fds(int a, int b)
{
    char buf[65536];
    gboolean a_open = TRUE, b_open = TRUE;

    while (a_open || b_open) {
        struct pollfd fds[2];
        int nfds = 0;
        if (a_open) {
            fds[nfds].fd = a;
            fds[nfds].events = POLLIN;
            nfds++;
        }
        if (b_open) {
            fds[nfds].fd = b;
            fds[nfds].events = POLLIN;
            nfds++;
        }

        int n = poll(fds, nfds, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        int fi = 0;
        if (a_open) {
            if (fds[fi].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t r = read(a, buf, sizeof(buf));
                if (r <= 0) {
                    a_open = FALSE;
                    shutdown(b, SHUT_WR);
                } else {
                    ssize_t off = 0;
                    while (off < r) {
                        ssize_t w = write(b, buf + off, (size_t)(r - off));
                        if (w < 0) {
                            if (errno == EINTR)
                                continue;
                            a_open = FALSE;
                            break;
                        }
                        off += w;
                    }
                }
            }
            fi++;
        }
        if (b_open) {
            if (fds[fi].revents & (POLLIN | POLLHUP | POLLERR)) {
                ssize_t r = read(b, buf, sizeof(buf));
                if (r <= 0) {
                    b_open = FALSE;
                    shutdown(a, SHUT_WR);
                } else {
                    ssize_t off = 0;
                    while (off < r) {
                        ssize_t w = write(a, buf + off, (size_t)(r - off));
                        if (w < 0) {
                            if (errno == EINTR)
                                continue;
                            b_open = FALSE;
                            break;
                        }
                        off += w;
                    }
                }
            }
        }
    }
}

typedef struct {
    GSocket *sock;
    AdbProxy *proxy;
} ConnData;

static void *proxy_thread(gpointer data)
{
    ConnData *d = data;
    GSocket *client = d->sock;
    AdbProxy *p = d->proxy;
    int cfd = g_socket_get_fd(client);
    g_socket_set_blocking(client, TRUE);

    /* Read the request head (up to the end of headers). */
    char head[16384];
    gsize total = 0;
    while (total < sizeof(head) - 1) {
        ssize_t n = read(cfd, head + total, sizeof(head) - 1 - total);
        if (n <= 0)
            break;
        total += (gsize)n;
        head[total] = '\0';
        if (strstr(head, "\r\n\r\n"))
            break;
    }
    if (total == 0)
        goto out;
    head[total] = '\0';

    /* Parse the request line: METHOD target PROTOCOL */
    char line[2048] = {0};
    g_strlcpy(line, head, sizeof(line));
    char *eol = strstr(line, "\r\n");
    if (eol)
        *eol = '\0';

    char method[16] = {0}, target[2000] = {0};
    if (sscanf(line, "%15s %2000s", method, target) != 2)
        goto out;
    audit("REQ %s %s", method, target);

    char host[512] = {0};
    int port = 80; /* plain HTTP default */
    gboolean is_connect = (g_ascii_strcasecmp(method, "CONNECT") == 0);
    if (is_connect) {
        /* target = host:port; CONNECT defaults to 443 */
        port = 443;
        char *colon = strrchr(target, ':');
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        g_strlcpy(host, target, sizeof(host));
    } else if (g_str_has_prefix(target, "http://")) {
        /* target = http://host[:port]/path */
        char *h = target + strlen("http://");
        char *slash = strchr(h, '/');
        if (slash)
            *slash = '\0';
        char *colon = strrchr(h, ':');
        if (colon) {
            *colon = '\0';
            port = atoi(colon + 1);
        }
        g_strlcpy(host, h, sizeof(host));
    }

    if (host_blocked(p, host)) {
        g_atomic_int_inc(&p->blocked);
        audit("BLOCK %s %s:%d count=%d", method, host, port,
              g_atomic_int_get(&p->blocked));
        static const char r403[] =
            "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(cfd, r403, sizeof(r403) - 1);
        goto out;
    }

    /* Connect upstream. Resolve with getaddrinfo() directly: GLib's
     * g_inet_socket_address_new_from_string() fails to resolve in some
     * environments (e.g. GLib 2.84 on this box) even when the system
     * resolver works, which made every request 502 and pages blank. Try
     * each resolved address until one connects (IPv6/IPv4 happy path). */
    char portstr[16];
    g_snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int ufd = -1;
    if (getaddrinfo(host, portstr, &hints, &res) == 0 && res) {
        for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
            int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
            if (fd < 0)
                continue;
            if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
                ufd = fd;
                break;
            }
            close(fd);
        }
        freeaddrinfo(res);
    }
    if (ufd < 0) {
        audit("FAIL upstream connect %s:%d", host, port);
        static const char r502[] =
            "HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send_all(cfd, r502, sizeof(r502) - 1);
        goto out;
    }

    if (is_connect) {
        static const char ok[] = "HTTP/1.1 200 Connection Established\r\n\r\n";
        send_all(cfd, ok, sizeof(ok) - 1);
    } else {
        /* Forward the request head we already read; the pipe carries
         * any remaining body and the response. */
        write_all_or_close(ufd, head, total);
    }

    pipe_fds(cfd, ufd);
    close(ufd);

out:
    g_object_unref(client);
    g_free(d);
    return NULL;
}

static gboolean on_client_connected(GSocketService *service, GSocketConnection *conn,
                                    GObject *source, gpointer user_data)
{
    ConnData *d = g_new(ConnData, 1);
    /* The socket is owned by the connection (and thus the service); take a
     * reference so it stays alive for the worker thread. */
    d->sock = g_socket_connection_get_socket(conn);
    g_object_ref(d->sock);
    d->proxy = user_data;
    g_thread_new("adb-proxy", proxy_thread, d);
    return TRUE;
}

/* ---------- public API ---------- */

AdbProxy *adb_proxy_new(void)
{
    AdbProxy *p = g_new0(AdbProxy, 1);
    p->service = g_socket_service_new();
    p->extra_hosts = g_ptr_array_new_with_free_func(g_free);
    return p;
}

void adb_proxy_free(AdbProxy *p)
{
    if (!p)
        return;
    adb_proxy_stop(p);
    g_ptr_array_free(p->extra_hosts, TRUE);
    g_free(p);
}

gboolean adb_proxy_start(AdbProxy *p)
{
    if (p->port > 0)
        return TRUE;

    /* p->service is a GSocketService, which is itself a GSocketListener.
     * add_address() creates, binds and listens on 127.0.0.1:0 in one step;
     * the effective address reports the port the kernel actually assigned. */
    GSocketAddress *addr = g_inet_socket_address_new(
        g_inet_address_new_from_string("127.0.0.1"), 0);
    GSocketAddress *effective = NULL;
    if (!g_socket_listener_add_address(G_SOCKET_LISTENER(p->service), addr,
                                       G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP,
                                       NULL, &effective, NULL)) {
        g_object_unref(addr);
        return FALSE;
    }
    p->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(effective));
    g_object_unref(effective);
    g_object_unref(addr);
    audit("PROXY port=%d", p->port);

    g_signal_connect(p->service, "incoming",
                     G_CALLBACK(on_client_connected), p);
    g_socket_service_start(p->service);
    return TRUE;
}

void adb_proxy_stop(AdbProxy *p)
{
    if (p->port == 0)
        return;
    g_socket_service_stop(p->service);
    g_object_unref(p->service);
    p->service = NULL;
    p->port = 0;
}

int adb_proxy_port(const AdbProxy *p)
{
    return p->port;
}

guint64 adb_proxy_blocked_count(const AdbProxy *p)
{
    return p ? (guint64)g_atomic_int_get(&p->blocked) : 0;
}

void adb_proxy_load_file(AdbProxy *p, const char *path)
{
    gchar *contents = NULL;
    if (!g_file_get_contents(path, &contents, NULL, NULL))
        return;
    char **lines = g_strsplit(contents, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strchomp(lines[i]);
        if (!*line || line[0] == '#')
            continue;
        g_ptr_array_add(p->extra_hosts, g_strdup(line));
    }
    g_strfreev(lines);
    g_free(contents);
}