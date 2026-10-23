/*
 * keepass.h — native KeePass integration via the keepassxc-cli backend.
 */
#ifndef GTKBROWSER_KEEPASS_H
#define GTKBROWSER_KEEPASS_H

#include <glib.h>

typedef struct {
    char *group;    /* group path, e.g. "Passwords/Web" */
    char *title;
    char *username;
    char *password;
    char *url;
    char *totp_seed;
} KpEntry;

/* TRUE if keepassxc-cli is available in PATH. */
gboolean kp_have_backend(void);

/* Run a keepassxc-cli command in a pseudo-terminal, answering its
 * "Enter password to unlock <db>:" prompt with db_password (may be
 * NULL, in which case prompts are left unanswered and the command
 * will time out). Returns TRUE if the process ran to completion;
 * *out receives the CLI output (prompt lines stripped), *exit_code
 * its exit status. Both *out and *exit_code are always set. */
gboolean kp_run_cli(char *const argv[], const char *db_password,
                    char **out, int *exit_code);

/* Export the database with `keepassxc-cli export -f csv` and parse the
 * result. Adds KpEntry items (freed with kp_entry_free) to *out.
 * On failure returns FALSE and sets *error (heap string). */
gboolean kp_load_entries(const char *db_path, const char *db_password,
                         GPtrArray *out, char **error);

void kp_entry_free(KpEntry *e);

/* TRUE if the entry's URL host matches *host (case-insensitive,
 * subdomains in either direction count). */
gboolean kp_entry_matches_host(const KpEntry *e, const char *host);

#endif