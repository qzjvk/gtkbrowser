/*
 * keepass.c — KeePass integration for GtkBrowser.
 *
 * keepassxc-cli (2.7.x) has no non-interactive password option, so the
 * command is run in a pseudo-terminal and its "Enter password to unlock
 * <db>:" prompt is answered with the password the user entered in the
 * browser. One `export -f csv` subprocess reads the whole database; the
 * CSV is parsed in-process. Passwords never touch the audit log or any
 * file other than the PTY itself.
 */
#include "keepass.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <pty.h>
#include <sys/wait.h>

#define KP_CLI_TIMEOUT_S 20

gboolean kp_have_backend(void)
{
    return g_find_program_in_path("keepassxc-cli") != NULL;
}

/* ---- PTY runner ---- */

/* Drop every CLI prompt line ("Enter password ...: "). */
static GString *strip_prompt_lines(const GString *in)
{
    GString *out = g_string_new(NULL);
    char *copy = g_strdup(in->str);
    char *saveptr = NULL;
    for (char *line = strtok_r(copy, "\n", &saveptr); line;
         line = strtok_r(NULL, "\n", &saveptr)) {
        if (g_str_has_prefix(g_strstrip(line), "Enter password"))
            continue;
        g_string_append(out, line);
        g_string_append_c(out, '\n');
    }
    g_free(copy);
    return out;
}

gboolean kp_run_cli(char *const argv[], const char *db_password,
                    char **out, int *exit_code)
{
    int master = -1;
    pid_t pid;
    GString *buf = g_string_new(NULL);
    int status = -1;
    gboolean sent_pw = FALSE, sent_repeat = FALSE, sent_keyfile = FALSE;
    time_t deadline = time(NULL) + KP_CLI_TIMEOUT_S;
    gboolean ok = TRUE;

    pid = forkpty(&master, NULL, NULL, NULL);
    if (pid < 0) {
        *out = g_strdup("could not create pseudo-terminal");
        *exit_code = -1;
        return FALSE;
    }
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }

    for (;;) {
        struct pollfd pfd = { .fd = master, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr > 0) {
            if (pfd.revents & POLLIN) {
                char c[4096];
                ssize_t n = read(master, c, sizeof(c));
                if (n > 0) {
                    g_string_append_len(buf, c, (gsize)n);
                    if (db_password) {
                        char *low = g_ascii_strdown(buf->str, -1);
                        if (!sent_pw && strstr(low, "password")) {
                            write(master, db_password, strlen(db_password));
                            write(master, "\n", 1);
                            sent_pw = TRUE;
                        } else if (!sent_repeat && strstr(low, "repeat")) {
                            write(master, db_password, strlen(db_password));
                            write(master, "\n", 1);
                            sent_repeat = TRUE;
                        } else if (!sent_keyfile && strstr(low, "key file")) {
                            write(master, "n\n", 2);
                            sent_keyfile = TRUE;
                        }
                        g_free(low);
                    }
                } else {
                    /* EOF: drain anything still buffered in the PTY */
                    int guard = 0;
                    for (;;) {
                        char d[4096];
                        ssize_t n2 = read(master, d, sizeof(d));
                        if (n2 > 0) {
                            g_string_append_len(buf, d, (gsize)n2);
                            continue;
                        }
                        if (n2 < 0 && errno == EIO && guard++ < 50)
                            continue; /* data may still be queued */
                        break;
                    }
                    break;
                }
            } else if (pfd.revents & (POLLHUP | POLLERR)) {
                break;
            }
        }
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid)
            break;
        if (time(NULL) > deadline) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            ok = FALSE;
            break;
        }
    }
    close(master);

    if (!ok) {
        *out = g_strdup("keepassxc-cli timed out");
        *exit_code = -1;
        g_string_free(buf, TRUE);
        return FALSE;
    }

    /* The child may exit a moment after the PTY hits EOF; reap it. */
    time_t reap_deadline = time(NULL) + 5;
    gboolean reaped = FALSE;
    while (time(NULL) < reap_deadline) {
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            reaped = TRUE;
            break;
        }
        usleep(50000);
    }
    if (!reaped) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
    *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    /* remove CR and prompt lines */
    for (gsize i = 0; i < buf->len; i++)
        if (buf->str[i] == '\r')
            buf->str[i] = ' ';
    GString *clean = strip_prompt_lines(buf);
    g_string_free(buf, TRUE);
    *out = g_string_free(clean, FALSE);
    return TRUE;
}

/* ---- CSV parsing (RFC 4180 subset: quoted fields, "" escape) ---- */

static char **csv_split(const char *line, int *n_out)
{
    GPtrArray *fields = g_ptr_array_new();
    GString *cur = g_string_new(NULL);
    gboolean in_q = FALSE;
    for (const char *p = line; ; p++) {
        char c = *p;
        if (in_q) {
            if (c == '"') {
                if (p[1] == '"') {
                    g_string_append_c(cur, '"');
                    p++;
                } else {
                    in_q = FALSE;
                }
            } else {
                g_string_append_c(cur, c);
            }
        } else if (c == '"') {
            in_q = TRUE;
        } else if (c == ',') {
            g_ptr_array_add(fields, g_string_free(cur, FALSE));
            cur = g_string_new(NULL);
        } else if (c == '\0') {
            break;
        } else {
            g_string_append_c(cur, c);
        }
    }
    g_ptr_array_add(fields, g_string_free(cur, FALSE));
    *n_out = fields->len;
    return (char **)g_ptr_array_free(fields, FALSE);
}

static int find_col(char **hdr, int n, const char *name)
{
    for (int i = 0; i < n; i++) {
        if (g_ascii_strcasecmp(g_strstrip(hdr[i]), name) == 0)
            return i;
    }
    return -1;
}

static char *field_at(char **f, int n, int i)
{
    if (i < 0 || i >= n)
        return g_strdup("");
    return g_strdup(f[i]);
}

void kp_entry_free(KpEntry *e)
{
    if (!e)
        return;
    g_free(e->group);
    g_free(e->title);
    g_free(e->username);
    g_free(e->password);
    g_free(e->url);
    g_free(e->totp_seed);
    g_free(e);
}

gboolean kp_load_entries(const char *db_path, const char *db_password,
                         GPtrArray *out, char **error)
{
    char *csv = NULL;
    int rc = -1;
    char *argv[] = {
        (char *)"keepassxc-cli", (char *)"export", (char *)"-f", (char *)"csv",
        (char *)db_path, NULL
    };

    if (!kp_have_backend()) {
        *error = g_strdup("keepassxc-cli not found in PATH. "
                          "Install KeePassXC (apt install keepassxc) to use "
                          "password filling.");
        return FALSE;
    }
    if (!kp_run_cli(argv, db_password, &csv, &rc)) {
        *error = csv ? g_strdup(csv) : g_strdup("keepassxc-cli failed");
        g_free(csv);
        return FALSE;
    }
    if (rc != 0) {
        if (strstr(csv,  "Invalid credentials"))
            *error = g_strdup("INVALID_PW: Invalid database password.");
        else if (strstr(csv,  "not found"))
            *error = g_strdup("Database file not found.");
        else
            *error = g_strdup(csv && *csv ? csv : "keepassxc-cli failed.");
        g_free(csv);
        return FALSE;
    }

    /* header */
    char *first = csv;
    char *eol = strchr(csv, '\n');
    if (eol)
        *eol = '\0';
    int ncols = 0;
    char **hdr = csv_split(first, &ncols);
    int i_group = find_col(hdr, ncols, "Group");
    int i_title = find_col(hdr, ncols, "Title");
    int i_user = find_col(hdr, ncols, "Username");
    int i_pass = find_col(hdr, ncols, "Password");
    int i_url = find_col(hdr, ncols, "URL");
    int i_totp = find_col(hdr, ncols, "TOTP");
    for (int i = 0; i < ncols; i++)
        g_free(hdr[i]);
    g_free(hdr);
    if (i_user < 0 || i_pass < 0) {
        *error = g_strdup("Unexpected keepassxc-cli CSV format.");
        g_free(csv);
        return FALSE;
    }

    for (char *line = eol ? eol + 1 : NULL; line && *line; ) {
        char *next = strchr(line, '\n');
        if (next)
            *next = '\0';
        int n = 0;
        char **f = csv_split(line, &n);
        if (n > 1) {
            KpEntry *e = g_new0(KpEntry, 1);
            e->group = field_at(f, n, i_group);
            e->title = field_at(f, n, i_title);
            e->username = field_at(f, n, i_user);
            e->password = field_at(f, n, i_pass);
            e->url = field_at(f, n, i_url);
            e->totp_seed = field_at(f, n, i_totp);
            g_ptr_array_add(out, e);
        }
        for (int i = 0; i < n; i++)
            g_free(f[i]);
        g_free(f);
        line = next ? next + 1 : NULL;
    }
    g_free(csv);
    return TRUE;
}

/* ---- host matching ---- */

static gboolean host_of_url(const char *url, char *host_out, size_t sz)
{
    const char *s = url;
    const char *scheme = strstr(url, "://");
    if (scheme)
        s = scheme + 3;
    const char *slash = strchr(s, '/');
    size_t len = slash ? (size_t)(slash - s) : strlen(s);
    const char *colon = memchr(s, ':', len); /* port */
    if (colon)
        len = (size_t)(colon - s);
    if (len == 0 || len >= sz)
        return FALSE;
    memcpy(host_out, s, len);
    host_out[len] = '\0';
    return TRUE;
}

static gboolean ends_with(const char *s, const char *suffix)
{
    size_t sl = strlen(s), fl = strlen(suffix);
    return sl >= fl && strcmp(s + sl - fl, suffix) == 0;
}

gboolean kp_entry_matches_host(const KpEntry *e, const char *host)
{
    if (!e->url || !*e->url || !host || !*host)
        return FALSE;
    char eh[512], ph[512];
    if (!host_of_url(e->url, eh, sizeof(eh)))
        return FALSE;
    g_ascii_strdown(eh, -1);
    g_strlcpy(ph, host, sizeof(ph));
    g_ascii_strdown(ph, -1);
    if (strcmp(eh, ph) == 0)
        return TRUE;
    char dot_eh[520], dot_ph[520];
    g_snprintf(dot_eh, sizeof(dot_eh), ".%s", eh);
    g_snprintf(dot_ph, sizeof(dot_ph), ".%s", ph);
    return ends_with(ph, dot_eh) || ends_with(eh, dot_ph);
}