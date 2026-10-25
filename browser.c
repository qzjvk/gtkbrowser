/*
 * gtkbrowser — a minimal tabbed web browser
 * GTK3 + WebKitGTK (webkit2gtk-4.1)
 *
 * Build:  meson setup build && ninja -C build
 * Run:    gtkbrowser [url]
 *
 * Agent-friendly CLI (talks to a running instance over a Unix socket):
 *   gtkbrowser <url>        open URL in the running instance
 *   gtkbrowser --list       list tabs (index<TAB>url<TAB>title)
 *   gtkbrowser --url        print the current tab's URL
 *   gtkbrowser --title      print the current tab's title
 *   gtkbrowser --blocked    print how many tracker requests have been blocked
 *   gtkbrowser --focus N    switch to tab N
 *   gtkbrowser --close [N]  close tab N (default: current)
 *   gtkbrowser --quit       quit the running instance
 *
 * Other flags:
 *   --no-sandbox            disable the WebKit sandbox
 *   --blocklist <file>      extra ad/tracker hosts to block (one per line)
 */
#include <glib.h>
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include "adblock.h"
#include "keepass.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#define APP_NAME "GtkBrowser"
#define HOME_URL "https://duckduckgo.com"
/* Cap on the visible tab title length (the tooltip keeps the full text).
 * Bounding the natural label width keeps the tab strip predictable. */
#define TAB_LABEL_MAX_CHARS 24
/* Maximum number of URLs kept in the history. */
#define HISTORY_MAX 500

typedef struct { const char *name; const char *base; } SearchEngine;
static const SearchEngine SEARCH_ENGINES[] = {
    { "DuckDuckGo", "https://duckduckgo.com/?q=" },
    { "Google", "https://www.google.com/search?q=" },
    { "Startpage", "https://www.startpage.com/sp/search?query=" },
    { "Bing", "https://www.bing.com/search?q=" },
    { "Brave", "https://search.brave.com/search?q=" },
    { "Custom", NULL }, /* template lives in settings; see search_template() */
};
#define N_SEARCH_ENGINES ((int)G_N_ELEMENTS(SEARCH_ENGINES))

/* drag & drop target atoms, interned once at startup */
static GdkAtom atom_text_uri_list;
static GdkAtom atom_text_plain;
static GdkAtom atom_text_utf8;
static GdkAtom atom_text_string;
static GdkAtom atom_text_text;
static GdkAtom atom_text_html;
static GdkAtom atom_netscape_url;
/* how long a drag may sit over the web view before the shell takes a
 * drop the page refused; long enough for WebKit's async page query to
 * settle, short enough that a held drag feels instant */
#define DROP_SHELL_DELAY_MS 250
/* after the shell accepted, re-assert the COPY status this long after the
 * last motion so it outlives WebKit's async status re-send for that motion */
#define DROP_STATUS_KEEP_MS 50
/* cap on tabs opened by a single drop (a folder drag can carry many) */
#define DROP_MAX_TABS 8

typedef struct Tab Tab;
typedef struct Browser Browser;
typedef struct Win Win;

/* Configurable keyboard shortcut: the keyval plus the modifier mask
 * (Ctrl / Alt / Shift). keyval 0 means the shortcut is disabled. */
typedef struct { guint keyval; GdkModifierType mods; } ScKey;

/* Actions offered in the shortcut manager (Settings → Manage shortcuts).
 * Order matters: on a conflict the earlier action wins. */
typedef enum {
    SC_BACK, SC_FORWARD, SC_RELOAD, SC_FULLSCREEN,
    SC_NEW_TAB, SC_CLOSE_TAB, SC_REOPEN_TAB, SC_FOCUS_URL,
    SC_HOME, SC_FIND, SC_BOOKMARK, SC_SOURCE, SC_SAVE_PAGE,
    SC_ZOOM_IN, SC_ZOOM_OUT, SC_ZOOM_RESET,
    SC_FILL_LOGIN, SC_CYCLE_NEXT, SC_CYCLE_PREV,
    SC_N_ACTIONS
} ShortcutAction;

/* App-level state, one per process: shared across every window. */
struct Browser {
    GPtrArray *windows;      /* Win*, most recent last */
    Win *active;             /* window with keyboard focus, or NULL */
    AdbProxy *proxy;
    gboolean adblock_enabled;
    gboolean devtools_enabled;
    gchar *blocklist_file;
    gchar *kp_db_path;   /* configured .kdbx path (persisted) */
    gchar *kp_password;  /* session cache of the db password */
    GSocketService *control_service;
    gchar *control_socket_path;
    GPtrArray *history;      /* char* URIs, most recent first */
    GPtrArray *bookmarks;    /* Bookmark* */
    GtkListStore *url_model; /* autocomplete suggestions */
    int search_engine;       /* index into SEARCH_ENGINES */
    gchar *custom_search_base; /* "Custom" engine URL template */
    gchar *home_url;         /* Ctrl+H target (settings) */
    gchar *home_file;        /* custom home page: local HTML file (settings);
                               re-read from disk on every home visit */
    gchar *new_tab_url;      /* page for new tabs (settings) */
    int default_zoom;        /* percent, applied to new tabs */
    gchar *user_agent;       /* custom user agent (settings) */
    GPtrArray *closed_tabs;  /* most-recent-closed first (Ctrl+Shift+T) */
    gboolean restore_session; /* reopen last session's tabs at start */
    gboolean hide_tabbar;    /* hide the tab strip in single-tab windows */
    gboolean block_third_party; /* reject cookies set by third-party sites */
    GHashTable *visited_hosts;  /* top-level hosts visited; flags shared cookies */
    /* downloads manager */
    GPtrArray *downloads;       /* DlItem*, newest last */
    GtkListStore *downloads_model; /* rows for the downloads window */
    GtkWidget *downloads_window;   /* NULL until first opened */
    gchar *download_dir;          /* auto-save folder; NULL = XDG default */
    gboolean download_ask;        /* ask for a destination for each file */
    /* site permissions ("origin|name" -> "granted" / "denied") */
    GHashTable *permissions;
    /* keyboard shortcuts (one ScKey per ShortcutAction) */
    ScKey sc[SC_N_ACTIONS];
};

/* Per-window state: the toplevel and everything that lives inside it. */
struct Win {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *url_entry;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *reload_button;
    GtkWidget *home_button;
    GtkWidget *progress;
    GtkWidget *blocked_button;
    GtkWidget *blocked_label;
    GtkWidget *kp_fill_button;
    GtkWidget *bookmarks_button;
    GtkWidget *downloads_button;
    Tab *find_tab;           /* tab whose find controller is active */
    GtkWidget *find_bar;     /* Ctrl+F bar, hidden by default */
    GtkWidget *find_entry;
    GtkWidget *find_label;   /* "N/M" match label */
    guint find_pos;
    guint find_count;
    gboolean fullscreen;     /* F11 state */
    GtkWidget *empty_tab_label; /* 0x0 label that hides the tab strip */
    Browser *browser;        /* app back-pointer */
};

typedef struct Tab {
    Win *win;                /* window the tab lives in */
    WebKitWebView *view;
    GtkWidget *icon_image;
    GtkWidget *title_label;
    GtkWidget *tab_label;    /* the tab's label box (icon + title + close) */
    GtkWidget *start_scroll; /* scrolled window around the start page */
    GtkWidget *start_page;   /* built-in GTK start page (home) */
    gboolean on_start_page;  /* start page is currently shown in this tab */
    gchar *source_file;      /* temp file behind a Ctrl+U view-source load */
    /* pending drag & drop state; see the "drag & drop" section */
    GdkDragContext *drop_ctx;     /* drag currently over the view (ref) */
    gboolean drop_shell_accepted; /* shell took the drop (page refused) */
    GPtrArray *drop_uris;         /* char* URIs from text/uri-list */
    gchar *drop_text;             /* text from the text targets */
    guint drop_timer;             /* fallback-delay source id, 0 = none */
    guint drop_leave_timer;       /* leave-vs-drop resolver id, 0 = none */
    guint drop_status_timer;      /* COPY re-assert id, 0 = none */
    /* set by the "query-permission-state" handler when WebKit asks to
     * prompt the user: the canonical name and origin the question is
     * about, so the later "permission-request" can store the decision
     * under the same key WebKit will query again */
    gchar *perm_pending_name;
    gchar *perm_pending_origin;
} Tab;

typedef struct {
    char *uri;
    char *title;
} Bookmark;

typedef struct {
    char *uri;
    char *title;
} ClosedTab;

/* forward declarations */
static Win *new_window(Browser *b);
static void close_window(Win *win);
static void on_window_destroy(GtkWidget *w, Win *win);
static void on_window_focus_in(GtkWidget *w, GdkEvent *e, Win *win);
static Win *active_window(Browser *b);
static GtkWidget *win_toplevel(Browser *b);
static void detach_tab(Win *win, GtkWidget *page);
static void update_tabbar(Win *win);
static void tab_destroy(Tab *tab);
static GtkWidget *new_tab(Win *win, const char *uri);
static void close_tab(Win *win, GtkWidget *page);
static GtkWidget *start_page_build(Tab *tab);
static void start_page_show(Tab *tab);
static void start_page_hide(Tab *tab);
static void tab_fix_focus(Tab *tab);
static void home_goto(Win *win);
static void update_url_entry(Win *win, Tab *tab);
static void update_nav_buttons(Win *win, Tab *tab);
static void update_window_title(Win *win, Tab *tab);
static char *normalize_uri(const char *s);
static void set_adblock_enabled(Browser *b, gboolean enabled);
static gboolean update_blocked_label(gpointer data);
static void fill_login(Browser *b);
static void choose_kp_database(GtkButton *btn, Browser *b);
static void kp_build_settings_section(Browser *b, GtkWidget *content);
static void on_blocked_clicked(GtkButton *btn, Browser *b);
static gboolean is_web_uri(const char *uri);
static void history_add(Browser *b, const char *uri);
static void refresh_url_model(Browser *b);
static gboolean on_bookmark_row_press(GtkWidget *row, GdkEventButton *e,
                                      Browser *b);
static void bookmark_toggle(Browser *b, const char *uri, const char *title);
static void cycle_tab(Win *win, int delta);
static void on_engine_changed(GtkComboBox *combo, Browser *b);
static void on_custom_search_changed(GtkEntry *entry, Browser *b);
static void on_home_changed(GtkEntry *entry, Browser *b);
static void on_new_tab_url_changed(GtkEntry *entry, Browser *b);
static void home_update_labels(GtkWidget *from, Browser *b);
static void on_zoom_changed(GtkSpinButton *spin, Browser *b);
static void on_user_agent_changed(GtkEntry *entry, Browser *b);
static void on_blocklist_browse(GtkButton *btn, Browser *b);
static GtkWidget *find_named(GtkWidget *w, const char *name);
static void save_settings(Browser *b);
static void drop_source_file(Tab *tab, const char *uri);
static gboolean on_view_context_menu(WebKitWebView *view, GtkWidget *menu,
                                     GdkEvent *event, WebKitHitTestResult *hit,
                                     gpointer user_data);
static void find_open(Win *win);
static void find_close(Win *win);
static void on_find_found_text(GtkWidget *controller, guint pos, Win *win);
static void on_find_counted_matches(GtkWidget *controller, guint count, Win *win);
static void on_find_failed(GtkWidget *controller, Win *win);
static void on_find_changed(GtkEntry *entry, Win *win);
static void on_find_next_clicked(GtkButton *btn, Win *win);
static void on_find_prev_clicked(GtkButton *btn, Win *win);
static void on_find_close_clicked(GtkButton *btn, Win *win);
static gboolean on_find_key_press(GtkWidget *w, GdkEventKey *e, Win *win);
static void on_restore_toggled(GtkToggleButton *btn, Browser *b);
static void on_hide_tabbar_toggled(GtkToggleButton *btn, Browser *b);
static void on_manage_history_clicked(GtkButton *btn, Browser *b);
static void on_manage_cookies_clicked(GtkButton *btn, Browser *b);
static void on_manage_permissions_clicked(GtkButton *btn, Browser *b);
static void on_manage_shortcuts_clicked(GtkButton *btn, Browser *b);
static void on_download_ask_toggled(GtkToggleButton *btn, Browser *b);
static void on_download_dir_changed(GtkEntry *entry, Browser *b);
static void on_download_dir_browse(GtkButton *btn, Browser *b);
static void sc_load(Browser *b, GKeyFile *kf);
static void sc_save(Browser *b, GKeyFile *kf);
static void on_block_third_party_toggled(GtkToggleButton *btn, Browser *b);
static void apply_cookie_policy(Browser *b);
static void history_save(Browser *b);
static void bookmarks_save(Browser *b);
static void session_save(Browser *b);
static gboolean session_restore(Browser *b, Win *win);

/* ---------- helpers ---------- */

static Tab *current_tab(Win *win)
{
    GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
    GtkWidget *page = gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb));
    return page ? g_object_get_data(G_OBJECT(page), "tab") : NULL;
}

static gboolean has_known_scheme(const char *s)
{
    static const char *schemes[] = {
        "http://", "https://", "file://", "about:", "data:", "ftp://", NULL
    };
    for (int i = 0; schemes[i]; i++)
        if (g_ascii_strncasecmp(s, schemes[i], strlen(schemes[i])) == 0)
            return TRUE;
    return FALSE;
}

/* Prepend https:// when the input looks like a URL but has no scheme, so
 * typing "example.com" works without the user having to add "https://". */
static char *normalize_uri(const char *s)
{
    if (has_known_scheme(s))
        return g_strdup(s);
    return g_strdup_printf("https://%s", s);
}

/* What "home" resolves to. Precedence: a custom local HTML file (the
 * user's own page, Settings), then the home page URL, then the built-in
 * GTK start page (search box + bookmarks + recent sites). */
typedef enum { HOME_KIND_BUILTIN, HOME_KIND_URL, HOME_KIND_FILE } HomeKind;

static HomeKind home_kind(Browser *b)
{
    if (b->home_file && *b->home_file)
        return HOME_KIND_FILE;
    if (b->home_url && *b->home_url)
        return HOME_KIND_URL;
    return HOME_KIND_BUILTIN;
}

/* The custom home file as a file:// URI (the path is absolute, so this
 * yields the correct "file:///path" form). */
static char *home_file_uri(Browser *b)
{
    return g_strdup_printf("file://%s", b->home_file);
}

/* The URI home navigation loads for HOME_URL / HOME_FILE kinds. */
static char *home_target_uri(Browser *b)
{
    if (b->home_file && *b->home_file)
        return home_file_uri(b);
    const char *u = b->home_url && *b->home_url ? b->home_url : HOME_URL;
    return normalize_uri(u);
}

static gboolean home_file_matches_uri(Browser *b, const char *uri)
{
    if (!b->home_file || !uri)
        return FALSE;
    char *expected = home_file_uri(b);
    gboolean m = g_strcmp0(expected, uri) == 0;
    g_free(expected);
    return m;
}

static gboolean looks_like_url(const char *s)
{
    if (has_known_scheme(s))
        return TRUE;
    if (strchr(s, ' '))
        return FALSE;
    return strchr(s, '.') != NULL;
}

/* Padlock in the URL bar: green for HTTPS, red for plain HTTP. This
 * WebKit build has no TLS-status API, so the scheme is the indicator. */
static void update_security_icon(Win *win, const char *uri)
{
    const char *icon = NULL;
    const char *tip = NULL;
    if (uri && g_ascii_strncasecmp(uri, "https://", 8) == 0) {
        icon = "security-high";
        tip = "Secure connection (HTTPS)";
    } else if (uri && g_ascii_strncasecmp(uri, "http://", 7) == 0) {
        icon = "security-low";
        tip = "Insecure connection (HTTP)";
    }
    gtk_entry_set_icon_from_icon_name(GTK_ENTRY(win->url_entry), GTK_ENTRY_ICON_PRIMARY, icon);
    gtk_entry_set_icon_tooltip_text(GTK_ENTRY(win->url_entry), GTK_ENTRY_ICON_PRIMARY, tip);
}

static void update_url_entry(Win *win, Tab *tab)
{
    if (!tab)
        return;
    const char *uri = webkit_web_view_get_uri(tab->view);
    gtk_entry_set_text(GTK_ENTRY(win->url_entry), uri ? uri : "");
    update_security_icon(win, uri);
}

/* Cap the visible tab title at TAB_LABEL_MAX_CHARS (the tooltip keeps the
 * full text). Bounding each label's natural width keeps the tab strip
 * predictable; GTK ellipsizes further when tabs are squeezed. */
static char *truncate_tab_title(const char *s)
{
    gsize len = g_utf8_strlen(s, -1);
    if (len <= TAB_LABEL_MAX_CHARS)
        return g_strdup(s);
    const char *cut = g_utf8_offset_to_pointer(s, TAB_LABEL_MAX_CHARS);
    char *trunc = g_strndup(s, (gsize)(cut - s));
    char *out = g_strconcat(trunc, "…", NULL);
    g_free(trunc);
    return out;
}

static void update_nav_buttons(Win *win, Tab *tab)
{
    if (!tab)
        return;
    gtk_widget_set_sensitive(win->back_button, webkit_web_view_can_go_back(tab->view));
    gtk_widget_set_sensitive(win->forward_button, webkit_web_view_can_go_forward(tab->view));
    const char *icon = webkit_web_view_is_loading(tab->view) ? "process-stop" : "view-refresh";
    gtk_button_set_image(GTK_BUTTON(win->reload_button),
                         gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR));
}

static void update_window_title(Win *win, Tab *tab)
{
    if (!tab) {
        gtk_window_set_title(GTK_WINDOW(win->window), APP_NAME);
        return;
    }
    const char *title = webkit_web_view_get_title(tab->view);
    if (title && *title) {
        char *t = g_strdup_printf("%s — %s", title, APP_NAME);
        gtk_window_set_title(GTK_WINDOW(win->window), t);
        g_free(t);
    } else {
        gtk_window_set_title(GTK_WINDOW(win->window), APP_NAME);
    }
}

static void load_uri(Win *win, const char *uri)
{
    Tab *tab = current_tab(win);
    if (tab)
        webkit_web_view_load_uri(tab->view, uri);
}

/* Resolve the active engine's URL template: the built-in prefix, or the
 * user's saved custom template (DuckDuckGo if that one is empty). */
static const char *search_template(Browser *b)
{
    const SearchEngine *se = &SEARCH_ENGINES[b->search_engine];
    if (se->base)
        return se->base;
    if (b->custom_search_base && *b->custom_search_base)
        return b->custom_search_base;
    return SEARCH_ENGINES[0].base;
}

/* Build the search URL. The template may carry the query as a %s or
 * {query} placeholder; otherwise it is a prefix the query appends to. */
static char *build_search_url(const char *tmpl, const char *escaped_query)
{
    const char *ph = strstr(tmpl, "%s");
    const char *alt = strstr(tmpl, "{query}");
    if (alt && (!ph || alt < ph))
        ph = alt;
    if (ph) {
        size_t skip = (ph[0] == '{') ? 7 : 2;
        return g_strdup_printf("%.*s%s%s", (int)(ph - tmpl), tmpl,
                               escaped_query, ph + skip);
    }
    return g_strdup_printf("%s%s", tmpl, escaped_query);
}

static void navigate_from_entry(Win *win)
{
    const char *text = gtk_entry_get_text(GTK_ENTRY(win->url_entry));
    if (!text || !*text)
        return;
    if (looks_like_url(text)) {
        char *uri = normalize_uri(text);
        load_uri(win, uri);
        g_free(uri);
    } else {
        char *q = g_uri_escape_string(text, NULL, TRUE);
        char *url = build_search_url(search_template(win->browser), q);
        g_free(q);
        load_uri(win, url);
        g_free(url);
    }
}

/* ---------- drag & drop ----------
 *
 * WebKit registers the web view itself as a drag destination and
 * connects its own handlers with g_signal_connect_after. On a drag it
 * pre-loads the data, asks the page whether it accepts, and on drop
 * delivers the data to the web content and finishes the drag. That is
 * what lets a page's own drop zone (e.g. Discord's file upload)
 * receive the dropped files.
 *
 * This layer is a shell-level fallback for drops the page refuses:
 * URLs and files then open in new tabs. These handlers connect with
 * g_signal_connect (not connect_after), so they run before WebKit's
 * and can veto the drop by returning TRUE from "drag-drop".
 *
 * The decision:
 *   drag-motion  -> note the drag, arm a short fallback timer
 *   (async)      -> WebKit's page query answers; an accepted drag
 *                   becomes a non-zero selected action, a refused one
 *                   stays zero
 *   fallback     -> ~250ms later, if the page refused and the data is
 *                   usable, the shell accepts the drag itself (status
 *                   COPY) so the cursor shows a valid target, and keeps
 *                   re-asserting COPY after each motion (WebKit's async
 *                   "refused" status would otherwise cancel the drag)
 *   drag-leave   -> GTK emits this both on a real leave and just
 *                   before drag-drop; a 0ms resolver timer defers the
 *                   cleanup so a drop is not lost (WebKit does the same)
 *   drag-drop    -> if the page never accepted and the data is usable,
 *                   the shell opens the URIs in new tabs, finishes the
 *                   drag, and vetoes WebKit; otherwise WebKit delivers
 *                   the drop to the web content
 *
 * GDK internals relied on here (gtk+ 3.24):
 *   - the drag source sends XdndDrop only if its last received
 *     XdndStatus names a valid action; WebKit re-sends the page's
 *     decision (asynchronously, after every drag motion), so once the
 *     shell accepts it must keep re-asserting COPY (drop_status_keep_cb)
 *     or the source cancels the drag instead of dropping.
 *   - gtk emits "drag-leave" before "drag-drop" on a drop, so leave
 *     must not destroy drop state synchronously.
 *   - gdk_drag_context_list_targets() returns the context's internal
 *     list and must never be freed, so this code does not call it. */

static gboolean drop_is_text_atom(GdkAtom atom)
{
    return atom == atom_text_plain || atom == atom_text_utf8 ||
           atom == atom_text_string || atom == atom_text_text ||
           atom == atom_netscape_url;
}

static void tab_drop_free_data(Tab *tab)
{
    if (tab->drop_uris) {
        g_ptr_array_free(tab->drop_uris, TRUE);
        tab->drop_uris = NULL;
    }
    g_free(tab->drop_text);
    tab->drop_text = NULL;
}

/* Reset all pending drop state (timers, data, context ref). */
static void tab_drop_cancel(Tab *tab)
{
    if (tab->drop_timer) {
        g_source_remove(tab->drop_timer);
        tab->drop_timer = 0;
    }
    if (tab->drop_leave_timer) {
        g_source_remove(tab->drop_leave_timer);
        tab->drop_leave_timer = 0;
    }
    if (tab->drop_status_timer) {
        g_source_remove(tab->drop_status_timer);
        tab->drop_status_timer = 0;
    }
    tab->drop_shell_accepted = FALSE;
    tab_drop_free_data(tab);
    g_clear_object(&tab->drop_ctx);
}

/* The tab is being destroyed with a drag pending. */
static void tab_drop_cleanup(Tab *tab)
{
    if (tab->drop_ctx)
        tab_drop_cancel(tab);
}

static gboolean drop_data_actionable(Tab *tab)
{
    if (tab->drop_uris)
        for (guint i = 0; i < tab->drop_uris->len; i++) {
            const char *uri = g_ptr_array_index(tab->drop_uris, i);
            if (g_str_has_prefix(uri, "file://") ||
                g_str_has_prefix(uri, "http://") ||
                g_str_has_prefix(uri, "https://"))
                return TRUE;
        }
    return tab->drop_text != NULL && looks_like_url(tab->drop_text);
}

static gboolean drop_status_keep_cb(gpointer data);

/* Fires ~250ms after a drag started over the view, once WebKit's async
 * page query has had a chance to answer. If the page refused (no
 * selected action) and the data is usable, accept the drop at the shell
 * level so the cursor shows a valid target. */
static gboolean drop_fallback_cb(gpointer data)
{
    Tab *tab = data;
    if (!tab->drop_ctx || tab->drop_shell_accepted)
        return G_SOURCE_REMOVE;
    if (gdk_drag_context_get_selected_action(tab->drop_ctx) != 0)
        return G_SOURCE_REMOVE; /* the page accepted: WebKit owns it */
    if (!drop_data_actionable(tab))
        return G_SOURCE_REMOVE;
    gdk_drag_status(tab->drop_ctx, GDK_CURRENT_TIME, GDK_ACTION_COPY);
    tab->drop_shell_accepted = TRUE;
    /* keep asserting the status; see drop_status_keep_cb */
    tab->drop_status_timer = g_timeout_add(DROP_STATUS_KEEP_MS,
                                           drop_status_keep_cb, tab);
    return G_SOURCE_REMOVE;
}

/* WebKit re-sends the page's decision (here: "refused") asynchronously
 * after every drag motion, and that late status would reach the drag
 * source before a release, making it cancel the drag (X sends Leave
 * instead of Drop). Once the shell has accepted, our COPY status must be
 * the last one the source sees, so re-assert it shortly after the last
 * motion. Armed by drop_fallback_cb and by on_view_drag_motion. */
static gboolean drop_status_keep_cb(gpointer data)
{
    Tab *tab = data;
    tab->drop_status_timer = 0;
    if (tab->drop_ctx && tab->drop_shell_accepted)
        gdk_drag_status(tab->drop_ctx, GDK_CURRENT_TIME, GDK_ACTION_COPY);
    return G_SOURCE_REMOVE;
}

/* 0ms resolver for drag-leave: if this fires, the drag left without a
 * drop (the pointer left or it was cancelled), so clear the state. On a
 * real drop, drag-drop runs first and cancels this timer. */
static gboolean drop_leave_cb(gpointer data)
{
    Tab *tab = data;
    tab->drop_leave_timer = 0;
    tab_drop_cancel(tab);
    return G_SOURCE_REMOVE;
}

/* Open what the shell accepted: file:// and http(s):// URIs and
 * plain-text URLs, each in a new tab. */
static void tab_drop_open(Tab *tab)
{
    Win *win = tab->win;
    int opened = 0;
    if (tab->drop_uris) {
        for (guint i = 0; i < tab->drop_uris->len && opened < DROP_MAX_TABS; i++) {
            const char *uri = g_ptr_array_index(tab->drop_uris, i);
            if (g_str_has_prefix(uri, "file://") ||
                g_str_has_prefix(uri, "http://") ||
                g_str_has_prefix(uri, "https://")) {
                new_tab(win, uri);
                opened++;
            }
        }
    }
    if (!opened && tab->drop_text) {
        /* _NETSCAPE_URL payloads are "URL\nname": use the first line */
        gchar *line = g_strstrip(g_strdup(tab->drop_text));
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        if (looks_like_url(line)) {
            char *uri = normalize_uri(line);
            new_tab(win, uri);
            g_free(uri);
        }
        g_free(line);
    }
}

static gboolean on_view_drag_motion(GtkWidget *w, GdkDragContext *ctx, gint x,
                                    gint y, guint time, Tab *tab)
{
    (void)w; (void)x; (void)y; (void)time;
    if (tab->drop_ctx != ctx) {
        tab_drop_cancel(tab);
        tab->drop_ctx = g_object_ref(ctx);
        tab->drop_timer = g_timeout_add(DROP_SHELL_DELAY_MS, drop_fallback_cb, tab);
    } else if (tab->drop_shell_accepted) {
        /* WebKit's connect-after handler re-queries the page for this
         * motion and will re-send the "refused" status; re-arm the
         * re-assert so our COPY is the status the source last sees. */
        if (!tab->drop_status_timer)
            tab->drop_status_timer = g_timeout_add(DROP_STATUS_KEEP_MS,
                                                   drop_status_keep_cb, tab);
    }
    return FALSE;
}

static void on_view_drag_leave(GtkWidget *w, GdkDragContext *ctx, guint time, Tab *tab)
{
    (void)w; (void)time;
    /* GTK emits drag-leave on a drop too (before drag-drop), so defer
     * the cleanup to a 0ms timer; drag-drop cancels it when a drop
     * actually happens. */
    if (tab->drop_ctx == ctx && !tab->drop_leave_timer)
        tab->drop_leave_timer = g_timeout_add(0, drop_leave_cb, tab);
}

/* WebKit pre-loads the drag data for its supported targets; it arrives
 * on this signal. Collect it (WebKit's connect-after handler gets it
 * too). */
static gboolean on_view_drag_data_received(GtkWidget *w, GdkDragContext *ctx, gint x,
                                           gint y, GtkSelectionData *sd, guint info,
                                           guint time, Tab *tab)
{
    (void)w; (void)x; (void)y; (void)info; (void)time;
    if (tab->drop_ctx != ctx || !sd)
        return FALSE;
    if (gtk_selection_data_get_target(sd) == atom_text_uri_list) {
        gchar **uris = gtk_selection_data_get_uris(sd);
        if (uris) {
            if (!tab->drop_uris)
                tab->drop_uris = g_ptr_array_new();
            for (int i = 0; uris[i]; i++)
                g_ptr_array_add(tab->drop_uris, g_strdup(uris[i]));
            g_free(uris);
        }
    } else if (drop_is_text_atom(gtk_selection_data_get_target(sd))) {
        gchar *text = (gchar *)gtk_selection_data_get_text(sd);
        if (text) {
            g_free(tab->drop_text);
            tab->drop_text = g_strdup(text);
            g_free(text);
        }
    }
    return FALSE;
}

static gboolean on_view_drag_drop(GtkWidget *w, GdkDragContext *ctx, gint x,
                                  gint y, guint time, Tab *tab)
{
    (void)w; (void)x; (void)y;
    if (tab->drop_ctx != ctx)
        return FALSE;
    /* A drop is happening: the pending drag-leave was the pre-drop one. */
    if (tab->drop_leave_timer) {
        g_source_remove(tab->drop_leave_timer);
        tab->drop_leave_timer = 0;
    }

    /* The page accepted it (WebKit set a selected action) unless the
     * shell already accepted in the fallback. */
    gboolean page_accepted =
        (gdk_drag_context_get_selected_action(ctx) != 0) && !tab->drop_shell_accepted;
    if (page_accepted)
        return FALSE; /* the page wants it: WebKit delivers to the content */
    if (!drop_data_actionable(tab))
        return FALSE; /* nothing the shell can do with it */

    /* The shell takes this drop (page refused, or a fast drop before the
     * fallback timer fired): open the URIs, finish the drag, and veto
     * WebKit's own finish so the drop is handled exactly once. */
    tab_drop_open(tab);
    gtk_drag_finish(ctx, TRUE, FALSE, time);
    tab_drop_cancel(tab);
    return TRUE;
}

/* Drop target on the window chrome (toolbar, tab strip): the web view
 * is the innermost destination, so this only fires for drops outside
 * it. */
static const GtkTargetEntry WINDOW_DROP_TARGETS[] = {
    { (gchar *)"text/uri-list", 0, 1 },
    { (gchar *)"text/plain", 0, 2 },
    { (gchar *)"text/html", 0, 3 },
};

static gboolean on_window_drag_data_received(GtkWidget *w, GdkDragContext *ctx, gint x,
                                             gint y, GtkSelectionData *sd, guint info,
                                             guint time, Win *win)
{
    (void)w; (void)ctx; (void)x; (void)y; (void)info; (void)time;
    if (!sd)
        return TRUE;
    int opened = 0;
    if (gtk_selection_data_get_target(sd) == atom_text_uri_list) {
        gchar **uris = gtk_selection_data_get_uris(sd);
        if (uris) {
            for (int i = 0; uris[i] && opened < DROP_MAX_TABS; i++) {
                if (g_str_has_prefix(uris[i], "file://") ||
                    g_str_has_prefix(uris[i], "http://") ||
                    g_str_has_prefix(uris[i], "https://")) {
                    new_tab(win, uris[i]);
                    opened++;
                }
            }
            g_free(uris);
        }
    } else if (drop_is_text_atom(gtk_selection_data_get_target(sd))) {
        gchar *text = (gchar *)gtk_selection_data_get_text(sd);
        if (text) {
            gchar *line = g_strstrip(text);
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = '\0';
            if (looks_like_url(line)) {
                char *uri = normalize_uri(line);
                new_tab(win, uri);
                g_free(uri);
            }
            g_free(text);
        }
    }
    return TRUE;
}

/* ---------- tabs ---------- */

static void close_tab(Win *win, GtkWidget *page)
{
    GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
    Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    if (tab) {
        tab_drop_cleanup(tab);
        drop_source_file(tab, NULL);
        if (tab == win->find_tab) {
            webkit_find_controller_search_finish(
                webkit_web_view_get_find_controller(tab->view));
            win->find_tab = NULL;
        }
        const char *uri = webkit_web_view_get_uri(tab->view);
        if (uri) {
            ClosedTab *ct = g_new0(ClosedTab, 1);
            ct->uri = g_strdup(uri);
            ct->title = g_strdup(webkit_web_view_get_title(tab->view));
            if (!win->browser->closed_tabs)
                win->browser->closed_tabs = g_ptr_array_new();
            g_ptr_array_add(win->browser->closed_tabs, ct);
            while (win->browser->closed_tabs->len > 32) {
                ClosedTab *old = g_ptr_array_index(win->browser->closed_tabs, 0);
                g_free(old->uri);
                g_free(old->title);
                g_free(old);
                g_ptr_array_remove_index(win->browser->closed_tabs, 0);
            }
        }
    }
    gtk_notebook_remove_page(nb, gtk_notebook_page_num(nb, page));
    if (gtk_notebook_get_n_pages(nb) == 0) {
        close_window(win);
        return;
    }
    update_tabbar(win);
}

/* Move a tab out of its window into a brand-new one (the "detach" action).
 * The page widget and its web view are preserved, so no reload happens. */
static void detach_tab(Win *win, GtkWidget *page)
{
    GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
    if (!page || gtk_notebook_get_n_pages(nb) <= 1)
        return; /* nothing to split off */
    Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    if (!tab || !tab->tab_label)
        return;
    Browser *b = win->browser;

    if (tab == win->find_tab) {
        webkit_find_controller_search_finish(
            webkit_web_view_get_find_controller(tab->view));
        win->find_tab = NULL;
        win->find_pos = 0;
        win->find_count = 0;
        gtk_label_set_text(GTK_LABEL(win->find_label), "");
        gtk_widget_hide(win->find_bar);
    }

    /* tab->tab_label holds a ref, so the label survives remove_page and
     * can be re-attached in the new window. GTK3 also destroys the page
     * itself on removal if the notebook is its only ref, so take a
     * temporary page ref to keep it alive across the move. */
    g_object_ref(page);
    gtk_notebook_remove_page(nb, gtk_notebook_page_num(nb, page));
    Win *nw = new_window(b);
    gtk_notebook_append_page(GTK_NOTEBOOK(nw->notebook), page, tab->tab_label);
    g_object_unref(page);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(nw->notebook), page, TRUE);
    tab->win = nw;

    /* refresh both windows' chrome and tab-strip visibility */
    update_tabbar(win);
    update_url_entry(win, current_tab(win));
    update_nav_buttons(win, current_tab(win));
    update_window_title(win, current_tab(win));
    update_tabbar(nw);
    update_url_entry(nw, current_tab(nw));
    update_nav_buttons(nw, current_tab(nw));
    update_window_title(nw, current_tab(nw));
}

static void detach_tab_activate(gpointer page)
{
    Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    if (tab)
        detach_tab(tab->win, page);
}

static void on_tab_close_clicked(GtkWidget *page, gpointer unused)
{
    (void)unused;
    Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    close_tab(tab->win, page);
}

static gboolean on_tab_button_press(GtkWidget *w, GdkEventButton *e, gpointer page)
{
    (void)w;
    Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    if (e->type == GDK_2BUTTON_PRESS ||
        (e->type == GDK_BUTTON_PRESS && e->button == GDK_BUTTON_MIDDLE)) {
        close_tab(tab->win, page);
        return TRUE;
    }
    if (e->type == GDK_BUTTON_PRESS && e->button == GDK_BUTTON_SECONDARY) {
        /* right-click: offer to move the tab into its own window */
        GtkWidget *menu = gtk_menu_new();
        GtkWidget *item = gtk_menu_item_new_with_label("Move to New Window");
        g_signal_connect_swapped(item, "activate",
                                 G_CALLBACK(detach_tab_activate), page);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
        gtk_widget_show_all(menu);
        gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)e);
        return TRUE;
    }
    return FALSE;
}

/* Show the tab strip only when a window has 2+ tabs, or when the user has
 * disabled single-tab auto-hide. For a single tab the notebook is given an
 * empty 0x0 label so the whole strip (title + close button) collapses away;
 * the page itself is untouched. */
static void update_tabbar(Win *win)
{
    GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
    int n = gtk_notebook_get_n_pages(nb);
    gboolean show = (n >= 2) || !win->browser->hide_tabbar;
    GtkStyleContext *sctx = gtk_widget_get_style_context(win->notebook);
    if (show)
        gtk_style_context_remove_class(sctx, "single-tab");
    else
        gtk_style_context_add_class(sctx, "single-tab");
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(nb, i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
        if (!tab || !tab->tab_label)
            continue;
        gtk_notebook_set_tab_label(nb, page,
                                   show ? tab->tab_label : win->empty_tab_label);
    }
}

/* Destructor for the "tab" data attached to each page; frees the tab and
 * everything it privately owns. The web view and label box are children of
 * the page / label and are freed by the container. */
static void tab_destroy(Tab *tab)
{
    tab_drop_cancel(tab);
    drop_source_file(tab, NULL);
    g_free(tab->perm_pending_name);
    g_free(tab->perm_pending_origin);
    g_clear_object(&tab->tab_label);
    g_free(tab);
}

/* ---------- web view signal handlers ---------- */

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, Tab *tab)
{
    Win *win = tab->win;
    Browser *b = win->browser;
    if (tab->on_start_page &&
        (event == WEBKIT_LOAD_STARTED || event == WEBKIT_LOAD_FINISHED)) {
        /* Any navigation away from the custom home file hides the start
         * page; landing back on it (re-home) refreshes and re-shows it. */
        const char *uri = webkit_web_view_get_uri(view);
        if (uri && home_file_matches_uri(b, uri)) {
            start_page_show(tab);
            tab_fix_focus(tab);
        } else
            start_page_hide(tab);
    }
    if (event == WEBKIT_LOAD_FINISHED) {
        const char *uri = webkit_web_view_get_uri(view);
        drop_source_file(tab, uri);
        if (uri)
            history_add(b, uri);
    }
    if (event == WEBKIT_LOAD_STARTED && tab == win->find_tab) {
        webkit_find_controller_search_finish(
            webkit_web_view_get_find_controller(view));
        win->find_tab = NULL;
    }
    if (event == WEBKIT_LOAD_STARTED) {
        /* a pending permission prompt belongs to the previous page */
        g_free(tab->perm_pending_name);
        g_free(tab->perm_pending_origin);
        tab->perm_pending_name = NULL;
        tab->perm_pending_origin = NULL;
    }
    if (current_tab(win) == tab) {
        update_url_entry(win, tab);
        update_nav_buttons(win, tab);
        update_window_title(win, tab);
    }
}

static void on_notify_title(WebKitWebView *view, GParamSpec *ps, Tab *tab)
{
    const char *title = webkit_web_view_get_title(view);
    const char *uri = webkit_web_view_get_uri(view);
    const char *text = (title && *title) ? title : (uri ? uri : "New Tab");
    gtk_label_set_text(GTK_LABEL(tab->title_label), truncate_tab_title(text));
    gtk_widget_set_tooltip_text(tab->title_label, text);
    if (current_tab(tab->win) == tab)
        update_window_title(tab->win, tab);
}

static void on_notify_uri(WebKitWebView *view, GParamSpec *ps, Tab *tab)
{
    if (current_tab(tab->win) == tab)
        update_url_entry(tab->win, tab);
    /* record visited top-level hosts so the cookie manager can flag cookies
     * from sites we haven't visited as "shared" */
    const char *uri = webkit_web_view_get_uri(view);
    if (uri && (g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://"))) {
        GUri *u = g_uri_parse(uri, 0, NULL);
        const char *host = u ? g_uri_get_host(u) : NULL;
        if (host && *host) {
            Browser *b = tab->win->browser;
            if (!b->visited_hosts)
                b->visited_hosts = g_hash_table_new_full(g_str_hash, g_str_equal,
                                                         g_free, NULL);
            g_hash_table_replace(b->visited_hosts, g_strdup(host), (gpointer)1);
            if (g_hash_table_size(b->visited_hosts) > 2000)
                g_hash_table_remove_all(b->visited_hosts);
        }
        g_uri_unref(u);
    }
}

static void on_notify_loading(WebKitWebView *view, GParamSpec *ps, Tab *tab)
{
    Win *win = tab->win;
    if (current_tab(win) != tab)
        return;
    if (!webkit_web_view_is_loading(view))
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(win->progress), 0);
}

static void on_notify_progress(WebKitWebView *view, GParamSpec *ps, Tab *tab)
{
    Win *win = tab->win;
    if (current_tab(win) != tab)
        return;
    if (webkit_web_view_is_loading(view))
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(win->progress),
                                      webkit_web_view_get_estimated_load_progress(view));
    else
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(win->progress), 0);
}

static void on_notify_favicon(WebKitWebView *view, GParamSpec *ps, Tab *tab)
{
    cairo_surface_t *surface = webkit_web_view_get_favicon(view);
    if (!surface)
        return;
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_surface(surface, 0, 0,
                                                    cairo_image_surface_get_width(surface),
                                                    cairo_image_surface_get_height(surface));
    cairo_surface_destroy(surface);
    if (!pixbuf)
        return;
    if (current_tab(tab->win) == tab)
        gtk_window_set_icon(GTK_WINDOW(tab->win->window), pixbuf);
    gtk_image_set_from_pixbuf(GTK_IMAGE(tab->icon_image), pixbuf);
    g_object_unref(pixbuf);
}

/*
 * Handles both target="_blank" links and window.open():
 * open the URL in a new tab instead of a new window.
 */
static void on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision,
                             WebKitPolicyDecisionType type, Tab *tab)
{
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationPolicyDecision *nav_decision =
            WEBKIT_NAVIGATION_POLICY_DECISION(decision);
        WebKitNavigationAction *action =
            webkit_navigation_policy_decision_get_navigation_action(nav_decision);
        WebKitURIRequest *request = webkit_navigation_action_get_request(action);
        const char *uri = request ? webkit_uri_request_get_uri(request) : NULL;
        if (uri)
            new_tab(tab->win, uri);
        webkit_policy_decision_ignore(decision);
        return;
    }
    webkit_policy_decision_use(decision);
}

/* ---------- downloads manager ----------
 *
 * Every download gets a row in a global list (toolbar button → window):
 * live progress, then Done / Failed / Canceled with the final size. Files
 * are saved to the configured download folder (default: XDG "Downloads")
 * unless "ask where to save" is on, in which case a save dialog is shown
 * first, like before. WebKitDownload exposes no completion or failure
 * signal in this WebKit build: progress reaches 1.0 when a download
 * succeeds, and an active download that makes no progress for a while is
 * marked failed by a periodic check. */

typedef enum { DL_ACTIVE, DL_DONE, DL_FAILED, DL_CANCELED } DlState;

typedef struct {
    Browser *b;              /* back-pointer (the item outlives the download) */
    WebKitDownload *dl;      /* strong ref, unref'd on "destroy" */
    char *name;              /* display filename */
    char *dest_path;
    char *dest_uri;
    guint64 received;
    gdouble progress;        /* 0..1 from estimated-progress */
    gint64 last_ms;          /* last progress activity (stall detection) */
    DlState state;
    gboolean canceled;       /* we canceled it (user / save-dialog veto) */
    GtkTreeIter row;         /* row in b->downloads_model */
} DlItem;

/* model columns: item ptr, icon name, filename, status text */
#define DL_COL_ITEM 0
#define DL_COL_ICON 1
#define DL_COL_NAME 2
#define DL_COL_STATUS 3

static void dl_update_row(DlItem *it);
static void on_dl_progress_notify(WebKitDownload *dl, GParamSpec *ps, gpointer data);
static void on_dl_received_notify(WebKitDownload *dl, GParamSpec *ps, gpointer data);

static char *dl_human_size(guint64 n)
{
    if (n < 1024)
        return g_strdup_printf("%" G_GUINT64_FORMAT " B", n);
    double v = (double)n;
    const char *u;
    if (v < 1024 * 1024)        { v /= 1024;          u = "KB"; }
    else if (v < 1024.0 * 1024) { v /= 1024 * 1024;   u = "MB"; }
    else if (v < 1024.0 * 1024 * 1024) { v /= 1024.0 * 1024 * 1024; u = "GB"; }
    else                         { v /= 1024.0 * 1024 * 1024; u = "TB"; }
    return g_strdup_printf("%.1f %s", v, u);
}

static void dl_update_row(DlItem *it)
{
    Browser *b = it->b;
    const char *icon;
    char *status;
    switch (it->state) {
    case DL_ACTIVE:
        icon = "download";
        if (it->progress > 0)
            status = g_strdup_printf("%.0f%% · %s", it->progress * 100,
                                     dl_human_size(it->received));
        else
            status = g_strdup("Starting…");
        break;
    case DL_DONE:
        icon = "emblem-ok";
        status = g_strdup(dl_human_size(it->received));
        break;
    case DL_FAILED:
        icon = "dialog-error";
        status = g_strdup("Failed");
        break;
    case DL_CANCELED:
    default:
        icon = "process-stop";
        status = g_strdup("Canceled");
        break;
    }
    gtk_list_store_set(b->downloads_model, &it->row,
                       DL_COL_ICON, icon,
                       DL_COL_NAME, it->name,
                       DL_COL_STATUS, status, -1);
    g_free(status);
}

/* Folder for auto-saved files: the configured one, else XDG_DOWNLOAD_DIR,
 * else ~/Downloads (created if missing). */
static char *dl_auto_dir(Browser *b)
{
    const char *env = g_getenv("XDG_DOWNLOAD_DIR");
    if (b->download_dir && *b->download_dir)
        return g_strdup(b->download_dir);
    if (env && *env)
        return g_strdup(env);
    return g_build_filename(g_get_home_dir(), "Downloads", NULL);
}

/* <dir>/<name>, with " (n)" appended until the name is free. */
static char *dl_auto_path(Browser *b, const char *name)
{
    char *dir = dl_auto_dir(b);
    g_mkdir_with_parents(dir, 0755);
    char *p;
    for (int i = 0; ; i++) {
        p = (i == 0) ? g_strdup_printf("%s/%s", dir, name)
                     : g_strdup_printf("%s/%s (%d)", dir, name, i);
        if (!g_file_test(p, G_FILE_TEST_EXISTS))
            break;
        g_free(p);
    }
    g_free(dir);
    return p;
}

static void on_downloads_window_destroy(GtkWidget *w, Browser *b);
static void dl_window_open(Browser *b);
static void on_dl_open_clicked(GtkButton *btn, Browser *b);
static void on_dl_folder_clicked(GtkButton *btn, Browser *b);
static void on_dl_cancel_clicked(GtkButton *btn, Browser *b);
static void on_dl_remove_clicked(GtkButton *btn, Browser *b);
static void on_dl_clear_clicked(GtkButton *btn, Browser *b);

/* Toolbar button: open the downloads window. */
static void on_downloads_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    dl_window_open(b);
}

/* Open the downloads window (created on first use). */
static void dl_window_open(Browser *b)
{
    if (!b->downloads_window) {
        GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(win), "Downloads");
        gtk_window_set_default_size(GTK_WINDOW(win), 720, 420);
        gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(win_toplevel(b)));

        GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
        gtk_container_add(GTK_CONTAINER(win), vbox);

        GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(b->downloads_model));
        GtkCellRenderer *ri = gtk_cell_renderer_pixbuf_new();
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1,
                                                    "", ri, "icon-name", DL_COL_ICON, NULL);
        GtkCellRenderer *rn = gtk_cell_renderer_text_new();
        gtk_tree_view_insert_column_with_attributes(GTK_TREE_VIEW(tv), -1,
                                                    "File", rn, "text", DL_COL_NAME, NULL);
        /* this build's append_column returns a 1-based position, so the "File"
         * column (second) is view index 1 for get_column() */
        gtk_tree_view_column_set_expand(gtk_tree_view_get_column(GTK_TREE_VIEW(tv), 1), TRUE);
        GtkCellRenderer *rs = gtk_cell_renderer_text_new();
        gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
            gtk_tree_view_column_new_with_attributes("Status", rs, "text", DL_COL_STATUS, NULL));
        gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(tv)),
                                    GTK_SELECTION_BROWSE);
        GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
        gtk_container_add(GTK_CONTAINER(sw), tv);
        gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *open_btn = gtk_button_new_with_label("_Open");
        gtk_widget_set_tooltip_text(open_btn, "Open the selected file");
        GtkWidget *folder_btn = gtk_button_new_with_label("Show in _Folder");
        gtk_widget_set_tooltip_text(folder_btn, "Open the folder containing the selected file");
        GtkWidget *cancel_btn = gtk_button_new_with_label("Can_cell");
        gtk_widget_set_tooltip_text(cancel_btn, "Cancel the selected running download");
        GtkWidget *remove_btn = gtk_button_new_with_label("Re_move");
        gtk_widget_set_tooltip_text(remove_btn, "Remove the selected row from the list");
        GtkWidget *clear_btn = gtk_button_new_with_label("Clear _finished");
        gtk_widget_set_tooltip_text(clear_btn, "Remove all finished rows");
        g_object_set_data(G_OBJECT(open_btn), "tv", tv);
        g_object_set_data(G_OBJECT(folder_btn), "tv", tv);
        g_object_set_data(G_OBJECT(cancel_btn), "tv", tv);
        g_object_set_data(G_OBJECT(remove_btn), "tv", tv);
        g_signal_connect(open_btn, "clicked", G_CALLBACK(on_dl_open_clicked), b);
        g_signal_connect(folder_btn, "clicked", G_CALLBACK(on_dl_folder_clicked), b);
        g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_dl_cancel_clicked), b);
        g_signal_connect(remove_btn, "clicked", G_CALLBACK(on_dl_remove_clicked), b);
        g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_dl_clear_clicked), b);
        gtk_box_pack_start(GTK_BOX(row), open_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), folder_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), cancel_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), remove_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), clear_btn, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

        g_signal_connect(win, "destroy", G_CALLBACK(on_downloads_window_destroy), b);
        gtk_widget_show_all(win);
        b->downloads_window = win;
    }
    gtk_window_present(GTK_WINDOW(b->downloads_window));
}

static void on_downloads_window_destroy(GtkWidget *w, Browser *b)
{
    (void)w;
    b->downloads_window = NULL;
}

/* The selected row's item, or NULL. */
static DlItem *dl_selected(GtkTreeView *tv)
{
    GtkTreeSelection *sel = gtk_tree_view_get_selection(tv);
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return NULL;
    gpointer item;
    gtk_tree_model_get(model, &iter, DL_COL_ITEM, &item, -1);
    return item;
}

static void on_dl_open_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    DlItem *it = dl_selected(g_object_get_data(G_OBJECT(btn), "tv"));
    if (it && it->state == DL_DONE && it->dest_uri)
        g_app_info_launch_default_for_uri(it->dest_uri, NULL, NULL);
}

static void on_dl_folder_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    DlItem *it = dl_selected(g_object_get_data(G_OBJECT(btn), "tv"));
    if (!it || !it->dest_path)
        return;
    char *dir = g_path_get_dirname(it->dest_path);
    char *uri = g_filename_to_uri(dir, NULL, NULL);
    if (uri) {
        g_app_info_launch_default_for_uri(uri, NULL, NULL);
        g_free(uri);
    }
    g_free(dir);
}

static void on_dl_cancel_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    DlItem *it = dl_selected(g_object_get_data(G_OBJECT(btn), "tv"));
    if (it && it->state == DL_ACTIVE && it->dl) {
        it->canceled = TRUE;
        it->state = DL_CANCELED;
        dl_update_row(it);
        webkit_download_cancel(it->dl);
    }
}

static void dl_remove_row(Browser *b, DlItem *it)
{
    if (it->state == DL_ACTIVE && it->dl) {
        it->canceled = TRUE;
        webkit_download_cancel(it->dl);
    }
    gtk_list_store_remove(b->downloads_model, &it->row);
    g_ptr_array_remove(b->downloads, it);
}

static void on_dl_remove_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    DlItem *it = dl_selected(g_object_get_data(G_OBJECT(btn), "tv"));
    if (it)
        dl_remove_row(b, it);
}

static void on_dl_clear_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    for (guint i = 0; i < b->downloads->len; ) {
        DlItem *it = g_ptr_array_index(b->downloads, i);
        if (it->state != DL_ACTIVE)
            dl_remove_row(b, it);
        else
            i++;
    }
}

/* The download object went away: it finished, failed, or was canceled.
 * The file on disk tells us which (a canceled download writes nothing). */
/* WebKitDownload has no terminal signal: an active download that makes no
 * progress for a while has failed (success reaches 1.0 and is handled in
 * the progress notify; canceled ones are flagged by us and skipped). */
static gboolean on_dl_stall_check(gpointer data)
{
    Browser *b = data;
    gint64 now = g_get_monotonic_time() / 1000;
    for (guint i = 0; i < b->downloads->len; i++) {
        DlItem *it = g_ptr_array_index(b->downloads, i);
        if (it->state != DL_ACTIVE || it->canceled || it->progress >= 1.0)
            continue;
        gint64 limit = it->received > 0 ? 20000 : 60000;
        if (now - it->last_ms > limit) {
            it->state = DL_FAILED;
            dl_update_row(it);
        }
    }
    return G_SOURCE_CONTINUE;
}

/* A WebKitDownload started: register it in the manager, pick its
 * destination (save dialog or the download folder), and start it. */
static void on_download_started(WebKitWebContext *context, WebKitDownload *download, Browser *b)
{
    (void)context;
    WebKitURIRequest *request = webkit_download_get_request(download);
    const char *uri = request ? webkit_uri_request_get_uri(request) : NULL;
    char *suggested = uri ? g_uri_unescape_string(g_path_get_basename(uri), NULL)
                          : g_strdup("download");
    if (!*suggested) {
        g_free(suggested);
        suggested = g_strdup("download");
    }

    DlItem *it = g_new0(DlItem, 1);
    it->b = b;
    it->dl = g_object_ref(download);
    it->name = suggested;

    if (b->download_ask) {
        /* save dialog, as before */
        GtkWidget *dialog = gtk_file_chooser_dialog_new("Save file",
                                                        GTK_WINDOW(win_toplevel(b)),
                                                        GTK_FILE_CHOOSER_ACTION_SAVE,
                                                        "_Cancel", GTK_RESPONSE_CANCEL,
                                                        "_Save", GTK_RESPONSE_ACCEPT,
                                                        NULL);
        gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggested);
        if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT) {
            char *save_uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(dialog));
            if (save_uri) {
                it->dest_uri = save_uri;
                it->dest_path = g_filename_from_uri(save_uri, NULL, NULL);
            } else {
                it->canceled = TRUE;
            }
        } else {
            it->canceled = TRUE;
        }
        gtk_widget_destroy(dialog);
    } else {
        it->dest_path = dl_auto_path(b, suggested);
        it->dest_uri = g_filename_to_uri(it->dest_path, NULL, NULL);
    }

    g_ptr_array_add(b->downloads, it);
    gtk_list_store_append(b->downloads_model, &it->row);
    gtk_list_store_set(b->downloads_model, &it->row, DL_COL_ITEM, it, -1);
    dl_update_row(it);

    if (it->canceled) {
        it->state = DL_CANCELED;
        dl_update_row(it);
        webkit_download_cancel(it->dl);
        g_clear_object(&it->dl); /* the vetoed item keeps no live download */
    } else {
        g_signal_connect(it->dl, "notify::estimated-progress",
                         G_CALLBACK(on_dl_progress_notify), it);
        g_signal_connect(it->dl, "notify::received-data-length",
                         G_CALLBACK(on_dl_received_notify), it);
        it->last_ms = g_get_monotonic_time() / 1000;
        webkit_download_set_destination(it->dl, it->dest_uri);
    }
    dl_window_open(b);
}

static void on_dl_progress_notify(WebKitDownload *dl, GParamSpec *ps, gpointer data)
{
    (void)ps;
    DlItem *it = data;
    it->progress = webkit_download_get_estimated_progress(dl);
    it->received = webkit_download_get_received_data_length(dl);
    it->last_ms = g_get_monotonic_time() / 1000;
    if (it->state == DL_ACTIVE && it->progress >= 1.0)
        it->state = DL_DONE; /* file is written; progress stops at 1.0 */
    dl_update_row(it);
}

static void on_dl_received_notify(WebKitDownload *dl, GParamSpec *ps, gpointer data)
{
    (void)ps;
    DlItem *it = data;
    it->received = webkit_download_get_received_data_length(dl);
    it->last_ms = g_get_monotonic_time() / 1000;
    dl_update_row(it);
}

/* ---------- site permissions ----------
 *
 * WebKit asks twice about a permission: first a "query-permission-state"
 * (what did the user decide before — we answer from our own persisted
 * store) and, if we answer PROMPT, a "permission-request" which we answer
 * with a dialog. The store is "origin|name" -> granted/denied, persisted
 * in the data dir. The request object does not carry the origin in this
 * WebKit build, so the origin is taken from the matching state query when
 * one preceded the request (it always does when a stored decision exists)
 * or from the view's current URI otherwise. */

static char *perm_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtkbrowser",
                            "permissions", NULL);
}

static void perm_load(Browser *b)
{
    b->permissions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    char *path = perm_path();
    gchar *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, NULL)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i])
                continue;
            char **f = g_strsplit(lines[i], "\t", 3);
            if (g_strv_length(f) == 3 &&
                (g_strcmp0(f[2], "granted") == 0 || g_strcmp0(f[2], "denied") == 0)) {
                char *key = g_strdup_printf("%s|%s", f[0], f[1]);
                g_hash_table_replace(b->permissions, key, g_strdup(f[2]));
            }
            g_strfreev(f);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_free(path);
}

static void perm_save(Browser *b)
{
    GString *s = g_string_new(NULL);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, b->permissions);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const char *key = k;
        char *p = g_strdup(key);
        char *bar = strchr(p, '|');
        if (bar && bar[1])
            g_string_append_printf(s, "%s\t%s\t%s\n", p, bar + 1, (const char *)v);
        g_free(p);
    }
    char *path = perm_path();
    g_file_set_contents(path, s->str, s->len, NULL);
    g_free(path);
    g_string_free(s, TRUE);
}

static const char *perm_lookup(Browser *b, const char *origin, const char *name)
{
    if (!b->permissions || !origin || !*origin || !name || !*name)
        return NULL;
    char *key = g_strdup_printf("%s|%s", origin, name);
    const char *v = g_hash_table_lookup(b->permissions, key);
    g_free(key);
    return v;
}

static void perm_set(Browser *b, const char *origin, const char *name,
                     const char *state) /* "granted", "denied", or NULL = ask */
{
    if (!origin || !*origin || !name || !*name)
        return;
    char *key = g_strdup_printf("%s|%s", origin, name);
    if (state)
        g_hash_table_replace(b->permissions, key, g_strdup(state));
    else
        g_hash_table_remove(b->permissions, key);
    g_free(key);
    perm_save(b);
}

/* Human-readable label for a permission name (raw WebKit name unknown
 * names are shown as-is, first letter uppercased). */
static char *perm_display_name(const char *name)
{
    static const struct { const char *raw; const char *label; } known[] = {
        { "geolocation", "Location" },
        { "notifications", "Notifications" },
        { "notification", "Notifications" },
        { "audio-capture", "Microphone" },
        { "video-capture", "Camera" },
        { "display-capture", "Screen recording" },
        { "media", "Camera / microphone" },
        { "device-info", "Device information" },
        { "pointer-lock", "Pointer lock" },
        { "clipboard", "Clipboard" },
        { "clipboard-read", "Clipboard (read)" },
        { "clipboard-write", "Clipboard (write)" },
        { "media-key-system", "Protected content (DRM)" },
        { "storage-access", "Cross-site data access" },
        { "website-data-access", "Cross-site data access" },
        { "xr", "VR / AR (WebXR)" },
        { "webxr", "VR / AR (WebXR)" },
        { "install-missing-media-plugins", "Media plugins" },
    };
    for (size_t i = 0; i < G_N_ELEMENTS(known); i++)
        if (g_strcmp0(known[i].raw, name) == 0)
            return g_strdup(known[i].label);
    char *out = g_strdup(name);
    if (out[0])
        out[0] = g_ascii_toupper(out[0]);
    return out;
}

/* The origin of the page currently shown in the view (scheme://host[:port]),
 * or NULL for non-web pages. */
static char *view_origin_string(WebKitWebView *view)
{
    const char *uri = webkit_web_view_get_uri(view);
    if (!uri || !(g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://")))
        return NULL;
    GUri *u = g_uri_parse(uri, 0, NULL);
    if (!u)
        return NULL;
    const char *scheme = g_uri_get_scheme(u);
    const char *host = g_uri_get_host(u);
    gint port = g_uri_get_port(u);
    char *out;
    if (port > 0 &&
        !(g_ascii_strcasecmp(scheme, "http") == 0 && port == 80) &&
        !(g_ascii_strcasecmp(scheme, "https") == 0 && port == 443))
        out = g_strdup_printf("%s://%s:%d", scheme, host, port);
    else
        out = g_strdup_printf("%s://%s", scheme, host);
    g_uri_unref(u);
    return out;
}

/* Ask the user. Returns TRUE to allow the request, FALSE to deny it, and
 * sets *mode_out to 0 ("ask every time"), 1 ("always allow") or 2
 * ("always deny") so the caller persists a decision only when the user
 * picked an "always" radio. */
static gboolean perm_prompt(Browser *b, const char *origin, const char *desc,
                            int *mode_out)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Permission request", GTK_WINDOW(win_toplevel(b)), GTK_DIALOG_MODAL,
        "_Deny", GTK_RESPONSE_REJECT, "_Allow", GTK_RESPONSE_ACCEPT, NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 12);
    gtk_box_set_spacing(GTK_BOX(box), 8);

    GtkWidget *org = gtk_label_new(origin);
    gtk_widget_set_halign(org, GTK_ALIGN_START);
    GtkWidget *dsc = gtk_label_new(desc);
    gtk_label_set_line_wrap(GTK_LABEL(dsc), TRUE);
    gtk_label_set_xalign(GTK_LABEL(dsc), 0.0);
    GtkWidget *r_ask = gtk_radio_button_new_with_label(NULL, "Ask every time");
    GtkWidget *r_allow = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(r_ask), "Always allow for this site");
    GtkWidget *r_deny = gtk_radio_button_new_with_label_from_widget(
        GTK_RADIO_BUTTON(r_ask), "Always deny for this site");
    gtk_box_pack_start(GTK_BOX(box), org, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), dsc, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), r_ask, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), r_allow, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), r_deny, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    gboolean mode_allow = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r_allow));
    gboolean mode_deny = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(r_deny));
    gtk_widget_destroy(dialog);

    *mode_out = mode_allow ? 1 : (mode_deny ? 2 : 0);
    gboolean allow = (response == GTK_RESPONSE_ACCEPT);
    if (mode_allow)
        allow = TRUE;
    if (mode_deny)
        allow = FALSE;
    return allow;
}

/* What a permission request is about: its canonical name and a one-line
 * description for the prompt. *name_out borrows a static string except in
 * the unknown-type branch, which allocates (flagged via *name_owned). */
static void perm_request_info(WebKitPermissionRequest *req,
                              const char **name_out, char **desc_out,
                              gboolean *name_owned)
{
    const char *name = NULL;
    char *desc = NULL;
    *name_owned = FALSE;
    if (WEBKIT_IS_GEOLOCATION_PERMISSION_REQUEST(req)) {
        name = "geolocation";
        desc = g_strdup("access your location");
    } else if (WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(req)) {
        name = "notifications";
        desc = g_strdup("send you notifications");
    } else if (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(req)) {
        WebKitUserMediaPermissionRequest *um = WEBKIT_USER_MEDIA_PERMISSION_REQUEST(req);
        if (webkit_user_media_permission_is_for_video_device(um)) {
            name = "video-capture";
            desc = g_strdup("use your camera");
        } else if (webkit_user_media_permission_is_for_audio_device(um)) {
            name = "audio-capture";
            desc = g_strdup("use your microphone");
        } else if (webkit_user_media_permission_is_for_display_device(um)) {
            name = "display-capture";
            desc = g_strdup("record or share your screen");
        } else {
            name = "media";
            desc = g_strdup("use your camera and microphone");
        }
    } else if (WEBKIT_IS_DEVICE_INFO_PERMISSION_REQUEST(req)) {
        name = "device-info";
        desc = g_strdup("read information about your devices");
    } else if (strstr(g_type_name(G_OBJECT_TYPE(req)),
                  "PointerLockPermissionRequest") != NULL) {
        /* the public headers only expose this type behind a build flag,
         * so match it by GType name */
        name = "pointer-lock";
        desc = g_strdup("capture your mouse pointer");
    } else if (WEBKIT_IS_CLIPBOARD_PERMISSION_REQUEST(req)) {
        name = "clipboard";
        desc = g_strdup("read and write your clipboard");
    } else if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(req)) {
        name = "media-key-system";
        desc = g_strdup("play protected content (DRM)");
    } else if (WEBKIT_IS_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(req)) {
        WebKitWebsiteDataAccessPermissionRequest *wd =
            WEBKIT_WEBSITE_DATA_ACCESS_PERMISSION_REQUEST(req);
        name = "storage-access";
        const char *rd = webkit_website_data_access_permission_request_get_requesting_domain(wd);
        const char *cd = webkit_website_data_access_permission_request_get_current_domain(wd);
        desc = g_strdup_printf("access %s data from %s",
                               rd ? rd : "the requesting site",
                               cd ? cd : "this site");
    } else if (WEBKIT_IS_XR_PERMISSION_REQUEST(req)) {
        name = "xr";
        desc = g_strdup("use VR / AR (WebXR)");
    } else if (WEBKIT_IS_INSTALL_MISSING_MEDIA_PLUGINS_PERMISSION_REQUEST(req)) {
        WebKitInstallMissingMediaPluginsPermissionRequest *mm =
            WEBKIT_INSTALL_MISSING_MEDIA_PLUGINS_PERMISSION_REQUEST(req);
        name = "install-missing-media-plugins";
        const char *d = webkit_install_missing_media_plugins_permission_request_get_description(mm);
        desc = d && *d ? g_strdup_printf("install missing media plugins (%s)", d)
                       : g_strdup("install missing media plugins");
    } else {
        char *t = g_strdup(g_type_name(G_OBJECT_TYPE(req)));
        for (char *c = t; *c; c++)
            *c = g_ascii_tolower(*c);
        name = t;
        *name_owned = TRUE; /* only this branch allocates *name_out */
        desc = g_strdup("request a permission");
    }
    *name_out = name;
    *desc_out = desc;
}

/* WebKit checks whether a (origin, name) decision was stored. We answer
 * from our own store; PROMPT means "ask the user" (the request signal
 * follows). */
static gboolean on_query_permission_state(WebKitWebView *view,
                                          WebKitPermissionStateQuery *query,
                                          Tab *tab)
{
    (void)view;
    Browser *b = tab->win->browser;
    WebKitSecurityOrigin *so = webkit_permission_state_query_get_security_origin(query);
    char *origin = so ? webkit_security_origin_to_string(so) : NULL;
    const char *name = webkit_permission_state_query_get_name(query);

    g_free(tab->perm_pending_name);
    g_free(tab->perm_pending_origin);
    tab->perm_pending_name = NULL;
    tab->perm_pending_origin = NULL;

    WebKitPermissionState state = WEBKIT_PERMISSION_STATE_PROMPT;
    const char *v = perm_lookup(b, origin, name);
    if (g_strcmp0(v, "granted") == 0)
        state = WEBKIT_PERMISSION_STATE_GRANTED;
    else if (g_strcmp0(v, "denied") == 0)
        state = WEBKIT_PERMISSION_STATE_DENIED;
    /* only a PROMPT answer is followed by a request, so only then is the
     * (origin, name) pair the one a later decision will be stored under */
    if (state == WEBKIT_PERMISSION_STATE_PROMPT &&
        origin && *origin && name && *name) {
        tab->perm_pending_name = g_strdup(name);
        tab->perm_pending_origin = g_strdup(origin);
    }
    webkit_permission_state_query_finish(query, state);
    g_free(origin);
    return TRUE;
}

/* The user-facing prompt for a permission request. */
static gboolean on_permission_request(WebKitWebView *view,
                                      WebKitPermissionRequest *req,
                                      Tab *tab)
{
    Browser *b = tab->win->browser;
    char *vorigin = view_origin_string(view);
    char *qname = tab->perm_pending_name;
    char *qorigin = tab->perm_pending_origin;
    tab->perm_pending_name = NULL;
    tab->perm_pending_origin = NULL;

    const char *type_name;
    char *desc;
    gboolean type_name_owned = FALSE;
    perm_request_info(req, &type_name, &desc, &type_name_owned);

    /* a stored decision skips the prompt entirely */
    const char *saved = NULL;
    if (qorigin && qname)
        saved = perm_lookup(b, qorigin, qname);
    if (!saved && vorigin)
        saved = perm_lookup(b, vorigin, type_name);
    if (saved) {
        if (g_strcmp0(saved, "granted") == 0)
            webkit_permission_request_allow(req);
        else
            webkit_permission_request_deny(req);
        if (type_name_owned)
            g_free((char *)type_name);
        g_free(desc);
        g_free(qname);
        g_free(qorigin);
        g_free(vorigin);
        return TRUE;
    }

    /* show the prompt */
    const char *disp = (qorigin && *qorigin) ? qorigin :
                       (vorigin ? vorigin : "a local page");
    int mode;
    gboolean allow = perm_prompt(b, disp, desc, &mode);

    /* record an "always" decision under the query's key when we know it
     * (that is the key WebKit will query again); otherwise fall back to
     * the view's origin plus the type name */
    const char *key_origin = (qorigin && *qorigin) ? qorigin : vorigin;
    const char *key_name = (qname && *qname) ? qname : type_name;
    if (mode == 1 && key_origin && *key_origin && key_name)
        perm_set(b, key_origin, key_name, "granted");
    else if (mode == 2 && key_origin && *key_origin && key_name)
        perm_set(b, key_origin, key_name, "denied");

    if (allow)
        webkit_permission_request_allow(req);
    else
        webkit_permission_request_deny(req);

    if (type_name_owned)
        g_free((char *)type_name);
    g_free(desc);
    g_free(qname);
    g_free(qorigin);
    g_free(vorigin);
    return TRUE;
}

/* ---------- permission manager (Settings → Manage permissions) ---------- */

typedef struct {
    Browser *b;
    GtkListStore *store;   /* origin, raw name, display name, state text */
    GtkTreeView *tv;
    GtkWidget *empty_lbl;
} PermMgr;

/* store columns */
#define PM_ORIGIN 0
#define PM_NAME   1
#define PM_DISPLAY 2
#define PM_STATE  3

static void perm_mgr_reload(PermMgr *m);

/* Apply the state attached to the clicked button ("granted" / "denied" /
 * NULL = ask again) to the selected row. */
static void on_perm_set_state_clicked(GtkButton *btn, gpointer d)
{
    PermMgr *m = d;
    const char *state = g_object_get_data(G_OBJECT(btn), "perm-state");
    GtkTreeSelection *sel = gtk_tree_view_get_selection(m->tv);
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;
    char *origin, *name;
    gtk_tree_model_get(model, &iter, PM_ORIGIN, &origin, PM_NAME, &name, -1);
    perm_set(m->b, origin, name, state);
    g_free(origin);
    g_free(name);
    perm_mgr_reload(m);
}

static void on_perm_clear_clicked(GtkButton *btn, gpointer d)
{
    (void)btn;
    PermMgr *m = d;
    g_hash_table_remove_all(m->b->permissions);
    perm_save(m->b);
    perm_mgr_reload(m);
}

static void perm_mgr_reload(PermMgr *m)
{
    gtk_list_store_clear(m->store);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, m->b->permissions);
    int n = 0;
    while (g_hash_table_iter_next(&it, &k, &v)) {
        char *p = g_strdup((const char *)k);
        char *bar = strchr(p, '|');
        if (!bar || !bar[1]) {
            g_free(p);
            continue;
        }
        *bar = '\0';
        GtkTreeIter iter;
        gtk_list_store_append(m->store, &iter);
        gtk_list_store_set(m->store, &iter, PM_ORIGIN, p, PM_NAME, bar + 1,
                           PM_DISPLAY, perm_display_name(bar + 1),
                           PM_STATE,
                           g_strcmp0(v, "granted") == 0 ? "Allow" : "Deny",
                           -1);
        g_free(p);
        n++;
    }
    gtk_widget_set_visible(m->empty_lbl, n == 0);
}

static void on_manage_permissions_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Site permissions",
                                                    GTK_WINDOW(win_toplevel(b)),
                                                    GTK_DIALOG_MODAL,
                                                    "_Close", GTK_RESPONSE_CLOSE,
                                                    NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 640, 420);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_set_spacing(GTK_BOX(box), 6);

    GtkWidget *hint = gtk_label_new(
        "Saved decisions from the permission prompts. Select a row and use "
        "the buttons below; “Set to Ask” (or deleting a row) forgets the "
        "decision so the site is asked again.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

    PermMgr *m = g_new0(PermMgr, 1);
    m->b = b;
    m->store = gtk_list_store_new(4, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING);
    GtkWidget *tv = gtk_tree_view_new_with_model(GTK_TREE_MODEL(m->store));
    m->tv = GTK_TREE_VIEW(tv);

    GtkCellRenderer *ro = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Origin", ro, "text",
                                                 PM_ORIGIN, NULL));
    GtkCellRenderer *rd = gtk_cell_renderer_text_new();
    int perm_col = gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Permission", rd,
                                                 "text", PM_DISPLAY, NULL));
    gtk_tree_view_column_set_expand(
        /* append_column returns a 1-based position in this build */
        gtk_tree_view_get_column(GTK_TREE_VIEW(tv), perm_col - 1), TRUE);
    GtkCellRenderer *rs = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(tv),
        gtk_tree_view_column_new_with_attributes("Decision", rs, "text",
                                                 PM_STATE, NULL));
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(m->tv),
                                GTK_SELECTION_BROWSE);

    m->empty_lbl = gtk_label_new("No saved decisions yet.");
    gtk_box_pack_start(GTK_BOX(box), m->empty_lbl, FALSE, FALSE, 0);

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), tv);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *allow_btn = gtk_button_new_with_label("Set _Allow");
    g_object_set_data(G_OBJECT(allow_btn), "perm-state", (gpointer)"granted");
    gtk_widget_set_tooltip_text(allow_btn, "Always allow for the selected site");
    g_signal_connect(allow_btn, "clicked", G_CALLBACK(on_perm_set_state_clicked), m);
    GtkWidget *deny_btn = gtk_button_new_with_label("Set _Deny");
    g_object_set_data(G_OBJECT(deny_btn), "perm-state", (gpointer)"denied");
    gtk_widget_set_tooltip_text(deny_btn, "Always deny for the selected site");
    g_signal_connect(deny_btn, "clicked", G_CALLBACK(on_perm_set_state_clicked), m);
    GtkWidget *ask_btn = gtk_button_new_with_label("Set to _Ask");
    g_object_set_data(G_OBJECT(ask_btn), "perm-state", NULL);
    gtk_widget_set_tooltip_text(ask_btn, "Forget the decision; ask again next time");
    g_signal_connect(ask_btn, "clicked", G_CALLBACK(on_perm_set_state_clicked), m);
    GtkWidget *clear_btn = gtk_button_new_with_label("Clear _all");
    gtk_widget_set_tooltip_text(clear_btn, "Forget every saved decision");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(on_perm_clear_clicked), m);
    gtk_box_pack_start(GTK_BOX(row), allow_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), deny_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), ask_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), clear_btn, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);

    perm_mgr_reload(m);
    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    g_free(m);
    gtk_widget_destroy(dialog);
}

/* ---------- tab creation ---------- */

static GtkWidget *new_tab(Win *win, const char *uri)
{
    Browser *b = win->browser;
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    WebKitWebView *view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    Tab *tab = g_new0(Tab, 1);
    tab->win = win;
    tab->view = view;

    WebKitSettings *settings = webkit_web_view_get_settings(view);
    webkit_settings_set_enable_developer_extras(settings, b->devtools_enabled);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);
    webkit_web_view_set_zoom_level(view, b->default_zoom / 100.0);
    if (b->user_agent && *b->user_agent)
        webkit_settings_set_user_agent(settings, b->user_agent);
    WebKitFindController *fc = webkit_web_view_get_find_controller(view);
    g_signal_connect(fc, "found-text", G_CALLBACK(on_find_found_text), win);
    g_signal_connect(fc, "counted-matches", G_CALLBACK(on_find_counted_matches), win);
    g_signal_connect(fc, "failed-to-find-text", G_CALLBACK(on_find_failed), win);

    /* tab label: favicon + title + close button */
    GtkWidget *label_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    tab->icon_image = gtk_image_new_from_icon_name("web-browser", GTK_ICON_SIZE_MENU);
    tab->title_label = gtk_label_new("New Tab");
    gtk_label_set_ellipsize(GTK_LABEL(tab->title_label), PANGO_ELLIPSIZE_END);
    /* GtkNotebook allocates each tab its minimum width, so this minimum
     * (in characters) is the visible tab width: the title stays readable.
     * The notebook is scrollable, so the window's minimum size is one tab
     * plus the arrows — it stays constant no matter how many tabs are open. */
    gtk_label_set_width_chars(GTK_LABEL(tab->title_label), 20);
    GtkWidget *close_btn = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_MENU);
    gtk_button_set_relief(GTK_BUTTON(close_btn), GTK_RELIEF_NONE);
    gtk_widget_set_tooltip_text(close_btn, "Close tab");
    gtk_box_pack_start(GTK_BOX(label_box), tab->icon_image, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(label_box), tab->title_label, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(label_box), close_btn, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(page), GTK_WIDGET(view), TRUE, TRUE, 0);
    /* the built-in start page lives next to the view; only one is shown.
     * It sits in a scrolled window so its content (search box, tile grids)
     * doesn't drive the window's minimum height: in a short window the start
     * page scrolls instead of forcing the window taller. Horizontal scrolling
     * is off — the tiles reflow to the window width instead. */
    tab->start_page = start_page_build(tab);
    tab->start_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(tab->start_scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(tab->start_scroll),
                                        GTK_SHADOW_NONE);
    gtk_container_add(GTK_CONTAINER(tab->start_scroll), tab->start_page);
    gtk_box_pack_start(GTK_BOX(page), tab->start_scroll, TRUE, TRUE, 0);

    g_object_set_data_full(G_OBJECT(page), "tab", tab, (GDestroyNotify)tab_destroy);
    gtk_notebook_append_page(GTK_NOTEBOOK(win->notebook), page, label_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(win->notebook), page, TRUE);
    /* keep a ref on the label so detach_tab can re-attach it in another
     * window (remove_page would otherwise destroy it) */
    tab->tab_label = g_object_ref(label_box);

    /* GtkNotebook tab labels are not container children, so
     * gtk_widget_show_all() never reaches them — show explicitly.
     * Same for the page contents of tabs created after startup. */
    gtk_widget_show_all(page);
    gtk_widget_show_all(label_box);
    /* hidden until the tab is on home (same show-all-then-hide trick as
     * the find bar in new_window) */
    gtk_widget_hide(tab->start_scroll);

    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_notify_title), tab);
    g_signal_connect(view, "notify::uri", G_CALLBACK(on_notify_uri), tab);
    g_signal_connect(view, "notify::favicon", G_CALLBACK(on_notify_favicon), tab);
    g_signal_connect(view, "notify::loading", G_CALLBACK(on_notify_loading), tab);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_notify_progress), tab);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), tab);
    g_signal_connect(view, "context-menu", G_CALLBACK(on_view_context_menu), tab);
    /* site permissions: WebKit asks for a stored state first
     * (query-permission-state), then a request if we answer PROMPT */
    g_signal_connect(view, "permission-request",
                     G_CALLBACK(on_permission_request), tab);
    g_signal_connect(view, "query-permission-state",
                     G_CALLBACK(on_query_permission_state), tab);
    /* shell-level drop fallback; runs before WebKit's own handlers, so
     * a drop the web page refused can be opened here as new tabs */
    g_signal_connect(view, "drag-motion", G_CALLBACK(on_view_drag_motion), tab);
    g_signal_connect(view, "drag-leave", G_CALLBACK(on_view_drag_leave), tab);
    g_signal_connect(view, "drag-data-received", G_CALLBACK(on_view_drag_data_received), tab);
    g_signal_connect(view, "drag-drop", G_CALLBACK(on_view_drag_drop), tab);
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), page);
    g_signal_connect(label_box, "button-press-event", G_CALLBACK(on_tab_button_press), page);

    /* Explicit URL wins; then the configured new-tab page; then the home
     * target — except the built-in home needs no load: the start page
     * (GTK widgets) is shown instead of the web view. */
    const char *start_url = uri && *uri ? uri
                                        : (b->new_tab_url && *b->new_tab_url
                                           ? b->new_tab_url
                                           : (home_kind(b) == HOME_KIND_BUILTIN
                                              ? NULL : home_target_uri(b)));
    if (start_url) {
        char *load_uri_str = normalize_uri(start_url);
        webkit_web_view_load_uri(view, load_uri_str);
        g_free(load_uri_str);
    }

    gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook),
                                  gtk_notebook_page_num(GTK_NOTEBOOK(win->notebook), page));
    update_tabbar(win);
    update_url_entry(win, tab);
    update_nav_buttons(win, tab);
    update_window_title(win, tab);

    if (!start_url || home_file_matches_uri(b, start_url)) {
        /* the tab starts on home: show the start page and focus its search
         * box (the web view is hidden, so its caret doesn't matter) */
        start_page_show(tab);
        GtkWidget *search = find_named(tab->start_page, "start-search");
        if (search)
            gtk_widget_grab_focus(search);
    } else {
        /* give the new tab's view the input focus so its text caret is
         * drawn; a WebKit view that never receives a focus-in doesn't
         * blink its caret */
        gtk_widget_grab_focus(GTK_WIDGET(view));
    }
    return page;
}

/* ---------- start page (built-in GTK home) ----------
 * Shown in a tab instead of the web view when the tab is on the built-in
 * home (no custom home file or URL configured). A search box on top, then
 * tiles for bookmarks and recent sites; a tile opens a new tab so the
 * start page stays, like a browser's normal start page. */

#define START_PAGE_MAX_SITES 12

/* The host part of a URI, for the recent-sites tiles (history keeps no
 * titles). */
static char *uri_host_label(const char *uri)
{
    const char *p = uri;
    if (strstr(p, "://"))
        p = strchr(p, ':') + 2;
    const char *end = strchr(p, '/');
    return g_strndup(p, end ? (gsize)(end - p) : strlen(p));
}

static void on_start_search_activated(GtkEntry *entry, Tab *tab)
{
    Browser *b = tab->win->browser;
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!text || !*text)
        return;
    if (looks_like_url(text)) {
        char *uri = normalize_uri(text);
        if (home_file_matches_uri(b, uri))
            start_page_show(tab);
        else
            webkit_web_view_load_uri(tab->view, uri);
        g_free(uri);
    } else {
        char *q = g_uri_escape_string(text, NULL, TRUE);
        char *url = build_search_url(search_template(b), q);
        g_free(q);
        webkit_web_view_load_uri(tab->view, url);
        g_free(url);
    }
}

/* Tiles open a new tab; the start page stays (like a browser start page). */
static void on_start_tile_clicked(GtkButton *btn, Tab *tab)
{
    (void)btn;
    const char *uri = g_object_get_data(G_OBJECT(btn), "uri");
    if (uri)
        new_tab(tab->win, uri);
}

static void start_tile_add(GtkWidget *grid, const char *uri, const char *title,
                           const char *icon, Tab *tab)
{
    GtkWidget *btn = gtk_button_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "flat");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn), "start-tile");
    gtk_widget_set_tooltip_text(btn, uri);
    /* fixed tile size: without it the flowbox stretches each child and
     * collapses the grid to one tile per row */
    gtk_widget_set_size_request(btn, 160, -1);
    gtk_widget_set_halign(btn, GTK_ALIGN_CENTER);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    GtkWidget *img = gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_DND);
    GtkWidget *lbl = gtk_label_new(title && *title ? title : uri);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(lbl), 16);
    gtk_label_set_max_width_chars(GTK_LABEL(lbl), 16);
    GtkWidget *sub = gtk_label_new(uri);
    gtk_label_set_ellipsize(GTK_LABEL(sub), PANGO_ELLIPSIZE_END);
    gtk_label_set_width_chars(GTK_LABEL(sub), 16);
    gtk_label_set_max_width_chars(GTK_LABEL(sub), 16);
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "dim-label");
    gtk_box_pack_start(GTK_BOX(box), img, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), sub, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(btn), box);
    /* Copy the URI: the bookmark/history lists can change before the tile
     * is clicked; the copy is freed with the button (tab destruction). */
    g_object_set_data_full(G_OBJECT(btn), "uri", g_strdup(uri), g_free);
    g_signal_connect(btn, "clicked", G_CALLBACK(on_start_tile_clicked), tab);
    gtk_container_add(GTK_CONTAINER(grid), btn);
}

/* Rebuild the tile grid: bookmarks first, then the recent unique sites. */
static void start_page_refresh(Tab *tab)
{
    if (!tab->start_page)
        return;
    Browser *b = tab->win->browser;
    GtkWidget *sites = find_named(tab->start_page, "start-sites");
    GtkWidget *hint = find_named(tab->start_page, "start-hint");
    if (!sites)
        return;

    GList *rows = gtk_container_get_children(GTK_CONTAINER(sites));
    for (GList *l = rows; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(rows);

    gboolean any = FALSE;
    if (b->bookmarks && b->bookmarks->len) {
        GtkWidget *hdr = gtk_label_new(NULL);
        gtk_label_set_markup(GTK_LABEL(hdr), "<b>Bookmarks</b>");
        gtk_widget_set_halign(hdr, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_top(hdr, 12);
        gtk_box_pack_start(GTK_BOX(sites), hdr, FALSE, FALSE, 0);
        GtkWidget *grid = gtk_flow_box_new();
        gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
        /* min 1 (not 3): a 3-tile floor made the grid's minimum width ~500px, which
         * would pin the window's minimum width on the start page. Wide
         * windows still get 6 tiles per row (max above); narrow ones simply
         * reflow to fewer per row. */
        gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 1);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 6);
        gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
        gtk_widget_set_margin_bottom(grid, 6);
        for (guint i = 0; i < b->bookmarks->len; i++) {
            Bookmark *bm = g_ptr_array_index(b->bookmarks, i);
            start_tile_add(grid, bm->uri, bm->title, "starred", tab);
        }
        gtk_box_pack_start(GTK_BOX(sites), grid, FALSE, FALSE, 0);
        any = TRUE;
    }
    if (b->history) {
        GPtrArray *seen = g_ptr_array_new_with_free_func(g_free);
        GPtrArray *recent = g_ptr_array_new();
        for (guint i = 0; i < b->history->len &&
                            recent->len < START_PAGE_MAX_SITES; i++) {
            const char *u = g_ptr_array_index(b->history, i);
            if (!is_web_uri(u))
                continue;
            gboolean dup = FALSE;
            for (guint j = 0; j < seen->len; j++)
                if (g_strcmp0(g_ptr_array_index(seen, j), u) == 0) {
                    dup = TRUE;
                    break;
                }
            if (dup)
                continue;
            g_ptr_array_add(seen, g_strdup(u));
            g_ptr_array_add(recent, (gpointer)u); /* borrowed from b->history */
        }
        if (recent->len > 0) {
            GtkWidget *hdr = gtk_label_new(NULL);
            gtk_label_set_markup(GTK_LABEL(hdr), "<b>Recent</b>");
            gtk_widget_set_halign(hdr, GTK_ALIGN_CENTER);
            gtk_widget_set_margin_top(hdr, 12);
            gtk_box_pack_start(GTK_BOX(sites), hdr, FALSE, FALSE, 0);
            GtkWidget *grid = gtk_flow_box_new();
            gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(grid), GTK_SELECTION_NONE);
            gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(grid), 1);
            gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(grid), 6);
            gtk_widget_set_halign(grid, GTK_ALIGN_CENTER);
            gtk_widget_set_margin_bottom(grid, 6);
            for (guint i = 0; i < recent->len; i++) {
                const char *u = g_ptr_array_index(recent, i);
                char *host = uri_host_label(u);
                start_tile_add(grid, u, host, "web-browser", tab);
                g_free(host);
            }
            gtk_box_pack_start(GTK_BOX(sites), grid, FALSE, FALSE, 0);
            any = TRUE;
        }
        g_ptr_array_unref(seen);
        g_ptr_array_unref(recent);
    }
    if (hint) {
        if (any)
            gtk_widget_hide(hint);
        else {
            gtk_label_set_text(GTK_LABEL(hint),
                "No bookmarks yet — press Ctrl+D on a page to add one. "
                "Sites you visit show up here.");
            gtk_widget_show(hint);
        }
    }
}

static GtkWidget *start_page_build(Tab *tab)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_style_context_add_class(gtk_widget_get_style_context(page), "start-page");

    /* header: app name + one-liner */
    GtkWidget *head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_halign(head, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(head, GTK_ALIGN_START);
    gtk_widget_set_margin_top(head, 48);
    GtkWidget *title = gtk_label_new(NULL);
    char *markup = g_strdup_printf(
        "<span size=\"xx-large\" weight=\"bold\">" APP_NAME "</span>");
    gtk_label_set_markup(GTK_LABEL(title), markup);
    g_free(markup);
    GtkWidget *sub = gtk_label_new("Search the web, or pick a site below");
    gtk_style_context_add_class(gtk_widget_get_style_context(sub), "dim-label");
    gtk_box_pack_start(GTK_BOX(head), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(head), sub, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page), head, FALSE, FALSE, 0);

    /* search box */
    GtkWidget *search = gtk_entry_new();
    gtk_widget_set_name(search, "start-search");
    /* ~480px wide by default — max-width-chars is its natural width, and
     * width-chars its minimum. (A size_request would have made 480px the
     * minimum and pinned the window's minimum width on the start page.)
     * halign CENTER keeps it at natural width in wide windows. */
    gtk_entry_set_width_chars(GTK_ENTRY(search), 6);
    gtk_entry_set_max_width_chars(GTK_ENTRY(search), 44);
    gtk_entry_set_placeholder_text(GTK_ENTRY(search), "Search or enter address");
    gtk_style_context_add_class(gtk_widget_get_style_context(search), "start-search");
    gtk_widget_set_halign(search, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(search, 18);
    gtk_widget_set_margin_start(search, 12);
    gtk_widget_set_margin_end(search, 12);
    g_signal_connect(search, "activate", G_CALLBACK(on_start_search_activated), tab);
    gtk_box_pack_start(GTK_BOX(page), search, FALSE, FALSE, 0);

    /* bookmark / recent-site tiles, rebuilt each time the page is shown */
    GtkWidget *sites = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_name(sites, "start-sites");
    gtk_box_pack_start(GTK_BOX(page), sites, FALSE, FALSE, 0);

    /* empty-state hint, shown only when there are no tiles */
    GtkWidget *hint = gtk_label_new("");
    gtk_widget_set_name(hint, "start-hint");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_widget_set_halign(hint, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(hint, 24);
    gtk_widget_set_margin_bottom(hint, 12);
    gtk_style_context_add_class(gtk_widget_get_style_context(hint), "dim-label");
    gtk_widget_set_no_show_all(hint, TRUE);
    gtk_box_pack_start(GTK_BOX(page), hint, FALSE, FALSE, 0);

    return page;
}

/* Show the start page in the tab (hides the web view). */
static void start_page_show(Tab *tab)
{
    Win *win = tab->win;
    Browser *b = win->browser;
    if (!tab->start_page)
        return;
    start_page_refresh(tab);
    tab->on_start_page = TRUE;
    gtk_widget_hide(GTK_WIDGET(tab->view));
    gtk_widget_show_all(tab->start_scroll);
    gtk_label_set_text(GTK_LABEL(tab->title_label), "Home");
    gtk_widget_set_tooltip_text(tab->title_label, "Home");
    const char *display = (home_kind(b) == HOME_KIND_FILE) ? b->home_file : "";
    gtk_entry_set_text(GTK_ENTRY(win->url_entry), display);
    update_security_icon(win, display);
    gtk_window_set_title(GTK_WINDOW(win->window), "Home — " APP_NAME);
    update_nav_buttons(win, tab);
    if (win->find_tab == tab)
        find_close(win);
}

/* Hide the start page and restore the web view (any navigation). */
static void start_page_hide(Tab *tab)
{
    Win *win = tab->win;
    if (!tab->on_start_page)
        return;
    tab->on_start_page = FALSE;
    gtk_widget_hide(tab->start_scroll);
    gtk_widget_show(GTK_WIDGET(tab->view));
    const char *title = webkit_web_view_get_title(tab->view);
    const char *uri = webkit_web_view_get_uri(tab->view);
    const char *text = (title && *title) ? title : (uri ? uri : "New Tab");
    gtk_label_set_text(GTK_LABEL(tab->title_label), truncate_tab_title(text));
    gtk_widget_set_tooltip_text(tab->title_label, text);
    update_url_entry(win, tab);
    update_nav_buttons(win, tab);
    update_window_title(win, tab);
}

/* Make sure this tab's visible content holds the input focus. After a tab
 * switch or the start page being shown over the view, the focus widget can
 * be left on a hidden widget — no visible caret, and typing goes nowhere.
 * Leaves the focus alone when a visible widget (URL bar, find bar, search
 * box) already holds it. */
static void tab_fix_focus(Tab *tab)
{
    Win *win = tab->win;
    GtkWidget *fw = gtk_window_get_focus(GTK_WINDOW(win->window));
    if (fw && gtk_widget_is_visible(fw))
        return;
    GtkWidget *want = tab->on_start_page
        ? find_named(tab->start_page, "start-search")
        : GTK_WIDGET(tab->view);
    if (want)
        gtk_widget_grab_focus(want);
}

/* Ctrl+H / home button: go to the configured home. */
static void home_goto(Win *win)
{
    Browser *b = win->browser;
    Tab *tab = current_tab(win);
    if (!tab)
        return;
    switch (home_kind(b)) {
    case HOME_KIND_BUILTIN:
        start_page_show(tab);
        tab_fix_focus(tab);
        break;
    case HOME_KIND_URL: {
        char *uri = home_target_uri(b);
        webkit_web_view_load_uri(tab->view, uri);
        g_free(uri);
        break;
    }
    case HOME_KIND_FILE: {
        char *uri = home_target_uri(b);
        if (g_strcmp0(webkit_web_view_get_uri(tab->view), uri) == 0) {
            start_page_show(tab); /* already there: refresh in place */
            tab_fix_focus(tab);
        } else
            webkit_web_view_load_uri(tab->view, uri); /* load-changed re-shows */
        g_free(uri);
        break;
    }
    }
}

/* ---------- toolbar / entry handlers ---------- */

static void on_back_clicked(GtkButton *btn, Win *win)
{
    (void)btn;
    Tab *tab = current_tab(win);
    if (tab && webkit_web_view_can_go_back(tab->view))
        webkit_web_view_go_back(tab->view);
}

static void on_forward_clicked(GtkButton *btn, Win *win)
{
    (void)btn;
    Tab *tab = current_tab(win);
    if (tab && webkit_web_view_can_go_forward(tab->view))
        webkit_web_view_go_forward(tab->view);
}

static void on_reload_clicked(GtkButton *btn, Win *win)
{
    (void)btn;
    Tab *tab = current_tab(win);
    if (!tab)
        return;
    if (webkit_web_view_is_loading(tab->view))
        webkit_web_view_stop_loading(tab->view);
    else
        webkit_web_view_reload(tab->view);
}

static void on_entry_activate(GtkEntry *entry, Win *win)
{
    (void)entry;
    navigate_from_entry(win);
}

static void on_new_tab_clicked(GtkButton *btn, Win *win)
{
    (void)btn;
    new_tab(win, NULL);
}

static void on_close_tab_clicked(GtkButton *btn, Win *win)
{
    (void)btn;
    GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
    GtkWidget *page = gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb));
    if (page)
        close_tab(win, page);
}

/*
 * NOTE: during the "switch-page" signal, gtk_notebook_get_current_page()
 * still reports the OLD page. The new page is passed as the `page`
 * argument, so we must use that — reading current_page here is what used
 * to make the URL bar / nav buttons / title show the previous tab.
 */
static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, Win *win)
{
    (void)nb; (void)num;
    Tab *tab = page ? g_object_get_data(G_OBJECT(page), "tab") : NULL;
    if (win->find_tab) {
        webkit_find_controller_search_finish(
            webkit_web_view_get_find_controller(win->find_tab->view));
        win->find_tab = NULL;
        win->find_pos = 0;
        win->find_count = 0;
        gtk_label_set_text(GTK_LABEL(win->find_label), "");
        gtk_widget_hide(win->find_bar);
    }
    update_url_entry(win, tab);
    update_nav_buttons(win, tab);
    update_window_title(win, tab);
    /* give the tab's visible content the input focus so its text caret
     * shows; tab_fix_focus keeps the URL bar (or any visible widget)
     * focused when it already is (e.g. cycling tabs while typing a URL) */
    if (tab)
        tab_fix_focus(tab);
}

/* ---------- find in page (Ctrl+F) ---------- */

static void find_set_count(Win *win)
{
    char buf[24];
    if (win->find_count > 0 && win->find_pos > 0)
        g_snprintf(buf, sizeof(buf), "%u/%u", win->find_pos, win->find_count);
    else
        buf[0] = '\0';
    gtk_label_set_text(GTK_LABEL(win->find_label), buf);
}

static void on_find_found_text(GtkWidget *controller, guint pos, Win *win)
{
    (void)controller;
    win->find_pos = pos;
    find_set_count(win);
}

static void on_find_counted_matches(GtkWidget *controller, guint count, Win *win)
{
    (void)controller;
    win->find_count = count;
    if (count == 0)
        gtk_label_set_text(GTK_LABEL(win->find_label), "No results");
    else
        find_set_count(win);
}

static void on_find_failed(GtkWidget *controller, Win *win)
{
    (void)controller;
    win->find_pos = 0;
    gtk_label_set_text(GTK_LABEL(win->find_label), "No results");
}

static void find_open(Win *win)
{
    gtk_widget_show(win->find_bar);
    gtk_widget_grab_focus(win->find_entry);
    gtk_editable_select_region(GTK_EDITABLE(win->find_entry), 0, -1);
}

static void find_close(Win *win)
{
    if (win->find_tab) {
        webkit_find_controller_search_finish(
            webkit_web_view_get_find_controller(win->find_tab->view));
        win->find_tab = NULL;
    }
    win->find_pos = 0;
    win->find_count = 0;
    gtk_label_set_text(GTK_LABEL(win->find_label), "");
    gtk_widget_hide(win->find_bar);
}

static void on_find_changed(GtkEntry *entry, Win *win)
{
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry));
    if (!win->find_tab)
        win->find_tab = current_tab(win);
    WebKitFindController *fc = win->find_tab ?
        webkit_web_view_get_find_controller(win->find_tab->view) : NULL;
    if (!fc)
        return;
    if (!*text) {
        webkit_find_controller_search_finish(fc);
        win->find_pos = 0;
        win->find_count = 0;
        gtk_label_set_text(GTK_LABEL(win->find_label), "");
        return;
    }
    win->find_pos = 0;
    win->find_count = 0;
    gtk_label_set_text(GTK_LABEL(win->find_label), "Searching…");
    webkit_find_controller_search(fc, text, WEBKIT_FIND_OPTIONS_NONE, G_MAXUINT);
}

static void on_find_step(Win *win, gboolean prev)
{
    if (!win->find_tab)
        return;
    WebKitFindController *fc = webkit_web_view_get_find_controller(win->find_tab->view);
    if (prev)
        webkit_find_controller_search_previous(fc);
    else
        webkit_find_controller_search_next(fc);
}

static void on_find_next_clicked(GtkButton *btn, Win *win)
{
    (void)btn;
    on_find_step(win, FALSE);
}

static void on_find_prev_clicked(GtkButton *btn, Win *win)
{
    (void)btn;
    on_find_step(win, TRUE);
}

static void on_find_close_clicked(GtkButton *btn, Win *win)
{
    (void)btn;
    find_close(win);
}

static gboolean on_find_key_press(GtkWidget *w, GdkEventKey *e, Win *win)
{
    (void)w;
    if (e->keyval == GDK_KEY_Escape) {
        find_close(win);
        return TRUE;
    }
    if (e->keyval == GDK_KEY_Return) {
        on_find_step(win, e->state & GDK_SHIFT_MASK);
        return TRUE;
    }
    return FALSE;
}

/* ---------- session restore ---------- */

#define SESSION_MAX_TABS 32

static char *session_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtkbrowser",
                            "session", NULL);
}

static void session_save(Browser *b)
{
    GKeyFile *kf = g_key_file_new();
    g_key_file_set_integer(kf, "session", "windows",
                           b->windows ? b->windows->len : 0);
    for (guint w = 0; b->windows && w < b->windows->len; w++) {
        Win *win = g_ptr_array_index(b->windows, w);
        GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
        int n = MIN(gtk_notebook_get_n_pages(nb), SESSION_MAX_TABS);
        /* window 0 keeps the legacy key layout so older sessions still load */
        if (w == 0)
            g_key_file_set_integer(kf, "session", "count", n);
        else {
            char *kn = g_strdup_printf("win%u.n", w);
            g_key_file_set_integer(kf, "session", kn, n);
            g_free(kn);
        }
        for (int i = 0; i < n; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(nb, i);
            Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
            const char *uri = tab ? webkit_web_view_get_uri(tab->view) : NULL;
            const char *title = tab ? webkit_web_view_get_title(tab->view) : NULL;
            if (!(uri && *uri))
                continue;
            char *k = (w == 0) ? g_strdup_printf("uri-%d", i)
                               : g_strdup_printf("win%u.uri-%d", w, i);
            g_key_file_set_string(kf, "session", k, uri);
            g_free(k);
            if (title && *title) {
                char *kt = (w == 0) ? g_strdup_printf("title-%d", i)
                                    : g_strdup_printf("win%u.title-%d", w, i);
                g_key_file_set_string(kf, "session", kt, title);
                g_free(kt);
            }
        }
    }
    char *path = session_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_key_file_save_to_file(kf, path, NULL);
    g_free(path);
    g_key_file_unref(kf);
}

/* Reopen the last session's tabs across all windows. Window 0 is restored
 * into first_win; the remaining windows are recreated. Returns TRUE if any
 * tab was restored. */
static gboolean session_restore(Browser *b, Win *first_win)
{
    GKeyFile *kf = g_key_file_new();
    char *path = session_path();
    gboolean ok = g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL);
    g_free(path);
    if (!ok)
        return FALSE;
    int W = g_key_file_get_integer(kf, "session", "windows", NULL);
    if (W <= 0)
        W = g_key_file_has_key(kf, "session", "count", NULL) ? 1 : 0;
    gboolean any = FALSE;
    for (int w = 0; W > 0 && w < W && w < 32; w++) {
        int n;
        if (w == 0)
            n = g_key_file_get_integer(kf, "session", "count", NULL);
        else {
            char *kn = g_strdup_printf("win%d.n", w);
            n = g_key_file_get_integer(kf, "session", kn, NULL);
            g_free(kn);
        }
        if (n <= 0)
            continue;
        Win *win = (w == 0) ? first_win : new_window(b);
        for (int i = 0; i < n && i < SESSION_MAX_TABS; i++) {
            char *k = (w == 0) ? g_strdup_printf("uri-%d", i)
                               : g_strdup_printf("win%d.uri-%d", w, i);
            const char *uri = g_key_file_get_string(kf, "session", k, NULL);
            g_free(k);
            if (!uri)
                continue;
            GtkWidget *page = new_tab(win, uri);
            Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
            if (tab) {
                char *kt = (w == 0) ? g_strdup_printf("title-%d", i)
                                    : g_strdup_printf("win%d.title-%d", w, i);
                const char *title = g_key_file_get_string(kf, "session", kt, NULL);
                g_free(kt);
                if (title)
                    gtk_label_set_text(GTK_LABEL(tab->title_label), title);
            }
            any = TRUE;
        }
    }
    g_key_file_unref(kf);
    return any;
}

/* ---------- settings ---------- */

static void apply_proxy_settings(Browser *b)
{
    WebKitWebsiteDataManager *dm = webkit_web_context_get_website_data_manager(
        webkit_web_context_get_default());
    if (!dm)
        return;
    if (b->adblock_enabled && b->proxy && adb_proxy_port(b->proxy) > 0) {
        char *uri = g_strdup_printf("http://127.0.0.1:%d", adb_proxy_port(b->proxy));
        WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(uri, NULL);
        webkit_website_data_manager_set_network_proxy_settings(
            dm, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
        webkit_network_proxy_settings_free(ps);
        g_free(uri);
    } else {
        webkit_website_data_manager_set_network_proxy_settings(
            dm, WEBKIT_NETWORK_PROXY_MODE_DEFAULT, NULL);
    }
}

/* Apply the cookie acceptance policy from settings. When blocking is on,
 * WebKit itself refuses cookies set by anything other than the main document. */
static void apply_cookie_policy(Browser *b)
{
    WebKitCookieManager *cm = webkit_web_context_get_cookie_manager(
        webkit_web_context_get_default());
    webkit_cookie_manager_set_accept_policy(
        cm, b->block_third_party ? WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY
                                 : WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
}

static void set_adblock_enabled(Browser *b, gboolean enabled)
{
    b->adblock_enabled = enabled;
    if (enabled) {
        if (!b->proxy) {
            b->proxy = adb_proxy_new();
            if (b->blocklist_file)
                adb_proxy_load_file(b->proxy, b->blocklist_file);
        }
        if (!adb_proxy_start(b->proxy)) {
            b->adblock_enabled = FALSE;
            return;
        }
    }
    apply_proxy_settings(b);
}

static void on_adblock_toggled(GtkToggleButton *btn, Browser *b)
{
    set_adblock_enabled(b, gtk_toggle_button_get_active(btn));
    /* sync the toggle back if enabling failed (e.g. bind error) */
    gtk_toggle_button_set_active(btn, b->adblock_enabled);
}

static void on_block_third_party_toggled(GtkToggleButton *btn, Browser *b)
{
    b->block_third_party = gtk_toggle_button_get_active(btn);
    apply_cookie_policy(b);
    save_settings(b);
}

/* Call fn(tab, data) for every open tab in every window. */
static void for_each_tab(Browser *b, void (*fn)(Tab *tab, gpointer), gpointer data)
{
    for (guint w = 0; b->windows && w < b->windows->len; w++) {
        GtkNotebook *nb = GTK_NOTEBOOK(((Win *)g_ptr_array_index(b->windows, w))->notebook);
        int n = gtk_notebook_get_n_pages(nb);
        for (int i = 0; i < n; i++) {
            Tab *tab = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(nb, i)), "tab");
            if (tab)
                fn(tab, data);
        }
    }
}

static void tab_apply_devtools(Tab *tab, gpointer data)
{
    Browser *b = data;
    WebKitSettings *settings = webkit_web_view_get_settings(tab->view);
    webkit_settings_set_enable_developer_extras(settings, b->devtools_enabled);
}

static void on_devtools_toggled(GtkToggleButton *btn, Browser *b)
{
    b->devtools_enabled = gtk_toggle_button_get_active(btn);
    for_each_tab(b, tab_apply_devtools, b);
}

/* Refresh the toolbar "trackers blocked" counter. Runs from a 1s timeout
 * so it stays live while pages load. */
static gboolean update_blocked_label(gpointer data)
{
    Browser *b = data;
    guint64 count = b->proxy ? adb_proxy_blocked_count(b->proxy) : 0;
    char buf[48];
    g_snprintf(buf, sizeof(buf), "%llu", (unsigned long long)count);
    for (guint w = 0; b->windows && w < b->windows->len; w++) {
        Win *win = g_ptr_array_index(b->windows, w);
        if (win->blocked_label)
            gtk_label_set_text(GTK_LABEL(win->blocked_label), buf);
    }
    return G_SOURCE_CONTINUE;
}

/* Clicking the counter shows the running total. */
static void on_blocked_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    guint64 count = b->proxy ? adb_proxy_blocked_count(b->proxy) : 0;
    char buf[80];
    g_snprintf(buf, sizeof(buf),
               "%llu tracker requests blocked so far.\n\n"
               "Toggle filtering in Settings.",
               (unsigned long long)count);
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(win_toplevel(b)),
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_INFO,
                                               GTK_BUTTONS_OK, "%s", buf);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* ---------- general settings (dialog callbacks) ---------- */

static void on_home_changed(GtkEntry *entry, Browser *b)
{
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
    g_free(b->home_url);
    b->home_url = (t && *t) ? g_strdup(t) : NULL;
    save_settings(b);
    home_update_labels(GTK_WIDGET(entry), b);
}

static void on_new_tab_url_changed(GtkEntry *entry, Browser *b)
{
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
    g_free(b->new_tab_url);
    b->new_tab_url = (t && *t) ? g_strdup(t) : NULL;
    save_settings(b);
    home_update_labels(GTK_WIDGET(entry), b);
}

static void tab_apply_zoom(Tab *tab, gpointer data)
{
    Browser *b = data;
    webkit_web_view_set_zoom_level(tab->view, b->default_zoom / 100.0);
}

static void on_zoom_changed(GtkSpinButton *spin, Browser *b)
{
    b->default_zoom = (int)gtk_spin_button_get_value(spin);
    for_each_tab(b, tab_apply_zoom, b);
    save_settings(b);
}

static void tab_apply_ua(Tab *tab, gpointer data)
{
    Browser *b = data;
    WebKitSettings *s = webkit_web_view_get_settings(tab->view);
    webkit_settings_set_user_agent(s, b->user_agent);
}

static void on_user_agent_changed(GtkEntry *entry, Browser *b)
{
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
    g_free(b->user_agent);
    b->user_agent = (t && *t) ? g_strdup(t) : NULL;
    /* NULL restores the WebKit default UA. */
    for_each_tab(b, tab_apply_ua, b);
    save_settings(b);
}

static void on_blocklist_browse(GtkButton *btn, Browser *b)
{
    (void)btn;
    GtkWidget *chooser = gtk_file_chooser_dialog_new("Choose blocklist file",
                                                    GTK_WINDOW(win_toplevel(b)),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    if (b->blocklist_file && g_file_test(b->blocklist_file, G_FILE_TEST_EXISTS))
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), b->blocklist_file);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        g_free(b->blocklist_file);
        b->blocklist_file = fn;
        save_settings(b);
    }
    gtk_widget_destroy(chooser);

    GtkWidget *dlg = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_DIALOG);
    GtkWidget *lbl = dlg ? find_named(dlg, "blocklist-label") : NULL;
    if (lbl) {
        char *text = g_strdup_printf("Blocklist: %s",
                                     b->blocklist_file ? b->blocklist_file
                                                       : "(none configured)");
        gtk_label_set_text(GTK_LABEL(lbl), text);
        g_free(text);
    }
}

/* What the home button / Ctrl+H will show, for the settings label. */
static char *home_display_text(Browser *b)
{
    if (home_kind(b) == HOME_KIND_FILE)
        return g_strdup_printf("custom file %s", b->home_file);
    if (home_kind(b) == HOME_KIND_URL)
        return g_strdup("the home page URL above");
    return g_strdup("the built-in start page");
}

/* What a new tab will show, for the settings label. */
static char *newtab_display_text(Browser *b)
{
    if (b->new_tab_url && *b->new_tab_url)
        return g_strdup("the new tab page URL above");
    if (home_kind(b) == HOME_KIND_BUILTIN)
        return g_strdup("the built-in start page");
    return g_strdup("the home page");
}

/* Refresh the "Home: …" and "New tabs: …" labels in the settings dialog. */
static void home_update_labels(GtkWidget *from, Browser *b)
{
    (void)from;
    GtkWidget *dlg = gtk_widget_get_ancestor(from, GTK_TYPE_DIALOG);
    GtkWidget *lbl;
    char *desc, *text;

    lbl = dlg ? find_named(dlg, "homefile-label") : NULL;
    if (lbl) {
        desc = home_display_text(b);
        text = g_strdup_printf("Home: %s", desc);
        gtk_label_set_text(GTK_LABEL(lbl), text);
        g_free(text);
        g_free(desc);
    }
    lbl = dlg ? find_named(dlg, "newtab-label") : NULL;
    if (lbl) {
        desc = newtab_display_text(b);
        text = g_strdup_printf("New tabs: %s", desc);
        gtk_label_set_text(GTK_LABEL(lbl), text);
        g_free(text);
        g_free(desc);
    }
}

/* Pick a local HTML file as the custom home page. The file is re-read on
 * every home visit, so the user can edit it live and see the result. */
static void on_home_file_browse(GtkButton *btn, Browser *b)
{
    (void)btn;
    GtkWidget *chooser = gtk_file_chooser_dialog_new("Choose home page file",
                                                     GTK_WINDOW(win_toplevel(b)),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN,
                                                     "_Cancel", GTK_RESPONSE_CANCEL,
                                                     "_Open", GTK_RESPONSE_ACCEPT,
                                                     NULL);
    GtkFileFilter *html = gtk_file_filter_new();
    gtk_file_filter_set_name(html, "HTML files");
    gtk_file_filter_add_pattern(html, "*.html");
    gtk_file_filter_add_pattern(html, "*.htm");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), html);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all);
    if (b->home_file && g_file_test(b->home_file, G_FILE_TEST_EXISTS))
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), b->home_file);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        g_free(b->home_file);
        b->home_file = fn;
        save_settings(b);
    }
    gtk_widget_destroy(chooser);
    home_update_labels(GTK_WIDGET(btn), b);
}

/* Reset home and new tabs to the built-in start page: clear the custom
 * home file, the home page URL, and the new tab page URL (and the URL
 * entries, so the change shows at once). */
static void on_home_builtin(GtkButton *btn, Browser *b)
{
    (void)btn;
    g_free(b->home_url);
    b->home_url = NULL;
    g_free(b->home_file);
    b->home_file = NULL;
    g_free(b->new_tab_url);
    b->new_tab_url = NULL;
    GtkWidget *dlg = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_DIALOG);
    if (dlg) {
        GtkWidget *entry = find_named(dlg, "home-entry");
        if (entry)
            gtk_entry_set_text(GTK_ENTRY(entry), "");
        entry = find_named(dlg, "newtab-entry");
        if (entry)
            gtk_entry_set_text(GTK_ENTRY(entry), "");
    }
    save_settings(b);
    home_update_labels(GTK_WIDGET(btn), b);
}

static void on_restore_toggled(GtkToggleButton *btn, Browser *b)
{
    b->restore_session = gtk_toggle_button_get_active(btn);
    save_settings(b);
}

static void on_hide_tabbar_toggled(GtkToggleButton *btn, Browser *b)
{
    b->hide_tabbar = gtk_toggle_button_get_active(btn);
    for (guint w = 0; b->windows && w < b->windows->len; w++)
        update_tabbar(g_ptr_array_index(b->windows, w));
    save_settings(b);
}

/* Add a "label: entry" row to the settings dialog and return the entry. */
static GtkWidget *setting_entry_row(GtkWidget *content, const char *label_text,
                                    const char *current, const char *placeholder,
                                    GCallback on_changed, gpointer user)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(row, GTK_ALIGN_START);
    GtkWidget *lbl = gtk_label_new(label_text);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    gtk_entry_set_width_chars(GTK_ENTRY(entry), 36);
    if (current)
        gtk_entry_set_text(GTK_ENTRY(entry), current);
    g_signal_connect(entry, "changed", on_changed, user);
    gtk_box_pack_start(GTK_BOX(row), lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), entry, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), row, FALSE, FALSE, 0);
    return entry;
}

/* ---------- history & bookmarks manager ---------- */

typedef struct {
    Browser *b;
    GtkTreeView *tv;
    gboolean history;
} ManagerCtx;

static void manager_reload(ManagerCtx *m)
{
    GtkListStore *store =
        GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(m->tv)));
    gtk_list_store_clear(store);
    GtkTreeIter iter;
    if (m->history) {
        for (guint i = 0; i < m->b->history->len; i++) {
            const char *uri = g_ptr_array_index(m->b->history, i);
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, uri, 1, "", -1);
        }
    } else {
        for (guint i = 0; i < m->b->bookmarks->len; i++) {
            Bookmark *bm = g_ptr_array_index(m->b->bookmarks, i);
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter, 0, bm->uri, 1, bm->title, -1);
        }
    }
}

static void manager_open(GtkButton *btn, gpointer data)
{
    (void)btn;
    ManagerCtx *m = data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(m->tv));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;
    gchar *uri;
    gtk_tree_model_get(model, &iter, 0, &uri, -1);
    Win *w = active_window(m->b);
    if (w)
        new_tab(w, uri);
    g_free(uri);
}

static void manager_delete(GtkButton *btn, gpointer data)
{
    (void)btn;
    ManagerCtx *m = data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(m->tv));
    GtkTreeModel *model;
    GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(sel, &model, &iter))
        return;
    gchar *uri;
    gtk_tree_model_get(model, &iter, 0, &uri, -1);
    if (m->history) {
        for (guint i = 0; i < m->b->history->len; i++) {
            if (g_strcmp0(g_ptr_array_index(m->b->history, i), uri) == 0) {
                g_free(g_ptr_array_index(m->b->history, i));
                g_ptr_array_remove_index(m->b->history, i);
                break;
            }
        }
        history_save(m->b);
    } else {
        for (guint i = 0; i < m->b->bookmarks->len; i++) {
            Bookmark *bm = g_ptr_array_index(m->b->bookmarks, i);
            if (g_strcmp0(bm->uri, uri) == 0) {
                g_free(bm->uri);
                g_free(bm->title);
                g_free(bm);
                g_ptr_array_remove_index(m->b->bookmarks, i);
                break;
            }
        }
        bookmarks_save(m->b);
    }
    g_free(uri);
    manager_reload(m);
    refresh_url_model(m->b);
}

static void manager_clear(GtkButton *btn, gpointer data)
{
    (void)btn;
    ManagerCtx *m = data;
    if (m->history) {
        while (m->b->history->len) {
            g_free(g_ptr_array_index(m->b->history, m->b->history->len - 1));
            g_ptr_array_remove_index(m->b->history, m->b->history->len - 1);
        }
        history_save(m->b);
    } else {
        while (m->b->bookmarks->len) {
            Bookmark *bm = g_ptr_array_index(m->b->bookmarks,
                                             m->b->bookmarks->len - 1);
            g_free(bm->uri);
            g_free(bm->title);
            g_free(bm);
            g_ptr_array_remove_index(m->b->bookmarks, m->b->bookmarks->len - 1);
        }
        bookmarks_save(m->b);
    }
    manager_reload(m);
    refresh_url_model(m->b);
}

static GtkWidget *manager_build_page(Browser *b, ManagerCtx *m)
{
    (void)b;
    m->tv = GTK_TREE_VIEW(gtk_tree_view_new_with_model(
        GTK_TREE_MODEL(gtk_list_store_new(2, G_TYPE_STRING,
                                          G_TYPE_STRING))));
    GtkCellRenderer *r = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
        "URI", r, "text", 0, NULL);
    gtk_tree_view_column_set_expand(c, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(m->tv), c);
    GtkCellRenderer *r2 = gtk_cell_renderer_text_new();
    c = gtk_tree_view_column_new_with_attributes("Title", r2, "text", 1, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(m->tv), c);
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_set_border_width(GTK_CONTAINER(sw), 6);
    gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(m->tv));
    manager_reload(m);
    return sw;
}

static void on_manage_history_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    if (!b->history || !b->bookmarks)
        return;
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "History and Bookmarks");
    gtk_window_set_default_size(GTK_WINDOW(win), 720, 480);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(win_toplevel(b)));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);
    GtkWidget *nb = gtk_notebook_new();
    for (int i = 0; i < 2; i++) {
        ManagerCtx *m = g_new0(ManagerCtx, 1);
        m->b = b;
        m->history = (i == 0);
        GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
        gtk_box_pack_start(GTK_BOX(page), manager_build_page(b, m), TRUE, TRUE, 0);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
        GtkWidget *open_btn = gtk_button_new_with_label("_Open");
        GtkWidget *del_btn = gtk_button_new_with_label("_Delete");
        GtkWidget *clear_btn = gtk_button_new_with_label("_Clear all");
        g_signal_connect(open_btn, "clicked", G_CALLBACK(manager_open), m);
        g_signal_connect(del_btn, "clicked", G_CALLBACK(manager_delete), m);
        g_signal_connect(clear_btn, "clicked", G_CALLBACK(manager_clear), m);
        g_signal_connect_swapped(win, "destroy", G_CALLBACK(g_free), m);
        gtk_box_pack_start(GTK_BOX(row), open_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), del_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(row), clear_btn, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page), row, FALSE, FALSE, 0);
        gtk_notebook_append_page(GTK_NOTEBOOK(nb), page,
                                 gtk_label_new(i == 0 ? "History" : "Bookmarks"));
    }
    gtk_box_pack_start(GTK_BOX(vbox), nb, TRUE, TRUE, 0);
    gtk_widget_show_all(win);
    gtk_widget_grab_focus(win_toplevel(b));
}

/* ---------- cookie manager ----------
 *
 * The whole cookie jar is fetched asynchronously from WebKit's cookie
 * manager and shown in a list where entries can be inspected, deleted,
 * cleared, or exported/imported in the standard Netscape/Mozilla
 * "cookies.txt" format (the format curl and Firefox use). The "Shared"
 * column flags cookies from sites this browser has not visited directly
 * (third-party cookies); blocking them outright is a WebKit cookie-policy
 * setting (see apply_cookie_policy()).
 */

typedef struct {
    Browser *b;
    GtkWidget *win;        /* NULL once the window is destroyed */
    GtkTreeView *tv;
    GtkListStore *store;
    GtkWidget *shared_only;
    GtkWidget *status;
    GList *cookies;        /* SoupCookie* from WebKit (owned by this dialog) */
    gboolean loading;      /* get_all_cookies in flight */
    guint refresh_timer;   /* delayed refresh after delete/import (0 = none) */
} CookieCtx;

static WebKitCookieManager *cookie_manager_of(Browser *b)
{
    return webkit_web_context_get_cookie_manager(webkit_web_context_get_default());
}

static void cookie_free_list(GList *list)
{
    if (list)
        g_list_free_full(list, (GDestroyNotify)soup_cookie_free);
}

static void cookie_set_status(CookieCtx *m, const char *text)
{
    gtk_label_set_text(GTK_LABEL(m->status), text);
}

/* A cookie is "shared" (third-party) when its site matches none of the
 * top-level hosts this browser has visited. */
static gboolean cookie_is_shared(SoupCookie *c, Browser *b)
{
    const char *domain = soup_cookie_get_domain(c);
    if (!domain || !*domain || !b->visited_hosts || !g_hash_table_size(b->visited_hosts))
        return FALSE;
    char *d = g_strdup(domain);
    if (d[0] == '.')
        memmove(d, d + 1, strlen(d));
    gboolean shared = TRUE;
    GHashTableIter it;
    gpointer v;
    g_hash_table_iter_init(&it, b->visited_hosts);
    while (shared && g_hash_table_iter_next(&it, &v, NULL)) {
        const char *host = v;
        size_t dl = strlen(d), vl = strlen(host);
        if (dl == vl)
            shared = (g_ascii_strcasecmp(d, host) != 0);
        else if (vl > dl)
            shared = !(host[vl - dl - 1] == '.' &&
                       g_ascii_strncasecmp(host + (vl - dl), d, dl) == 0);
        else
            shared = !(d[dl - vl - 1] == '.' &&
                       g_ascii_strncasecmp(d + (dl - vl), host, vl) == 0);
    }
    g_free(d);
    return shared;
}

static void cookie_reload(CookieCtx *m)
{
    gtk_list_store_clear(m->store);
    gboolean only_shared = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(m->shared_only));
    guint n = 0;
    GtkTreeIter iter;
    for (GList *l = m->cookies; l; l = l->next) {
        SoupCookie *c = l->data;
        gboolean shared = cookie_is_shared(c, m->b);
        if (only_shared && !shared)
            continue;
        const char *site = soup_cookie_get_domain(c);
        GDateTime *exp = soup_cookie_get_expires(c);
        char expbuf[40] = "session";
        if (exp) {
            char *s = g_date_time_format(exp, "%Y-%m-%d %H:%M");
            g_snprintf(expbuf, sizeof(expbuf), "%s UTC", s);
            g_free(s);
        }
        GString *flags = g_string_new(NULL);
        if (soup_cookie_get_secure(c))
            g_string_append(flags, "secure ");
        if (soup_cookie_get_http_only(c))
            g_string_append(flags, "http-only");
        gtk_list_store_append(m->store, &iter);
        gtk_list_store_set(m->store, &iter,
                           0, site ? site : "",
                           1, soup_cookie_get_name(c),
                           2, soup_cookie_get_value(c),
                           3, expbuf,
                           4, flags->str,
                           5, shared ? "yes" : "",
                           6, c, -1);
        g_string_free(flags, TRUE);
        n++;
    }
    char msg[64];
    g_snprintf(msg, sizeof(msg), "%u cookie%s", n, n == 1 ? "" : "s");
    cookie_set_status(m, msg);
}

static void on_cookies_fetched(GObject *manager, GAsyncResult *res, gpointer data)
{
    CookieCtx *m = data;
    /* loading was set by cookie_refresh() while this fetch was in flight */
    g_assert(m->loading);
    GError *err = NULL;
    GList *list = webkit_cookie_manager_get_all_cookies_finish(
        WEBKIT_COOKIE_MANAGER(manager), res, &err);
    g_clear_error(&err);
    m->loading = FALSE;
    if (!m->win) { /* the window was closed while the fetch was in flight */
        cookie_free_list(list);
        g_free(m);
        return;
    }
    cookie_free_list(m->cookies);
    m->cookies = list;
    cookie_reload(m);
}

static void cookie_refresh(CookieCtx *m)
{
    if (m->loading || !m->win)
        return;
    m->loading = TRUE;
    webkit_cookie_manager_get_all_cookies(cookie_manager_of(m->b), NULL,
                                          on_cookies_fetched, m);
}

static void on_cookie_refresh_clicked(GtkButton *btn, gpointer data)
{
    (void)btn;
    cookie_refresh(data);
}

static gboolean cookie_refresh_later(gpointer data)
{
    cookie_refresh(data);
    return G_SOURCE_REMOVE;
}

static void on_shared_only_toggled(GtkToggleButton *btn, gpointer data)
{
    (void)btn;
    cookie_reload(data);
}

static void on_delete_cookie_done(GObject *manager, GAsyncResult *res, gpointer data)
{
    (void)manager; (void)res;
    /* WebKit converts the cookie synchronously before dispatching, so it is
     * safe to free it here */
    soup_cookie_free(data);
}

static void cookie_delete_selected(GtkButton *btn, gpointer data)
{
    (void)btn;
    CookieCtx *m = data;
    GtkTreeSelection *sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(m->tv));
    GPtrArray *to_delete = g_ptr_array_new();
    GtkTreeIter iter;
    if (gtk_tree_model_iter_children(GTK_TREE_MODEL(m->store), &iter, NULL)) {
        do {
            if (gtk_tree_selection_path_is_selected(
                    sel, gtk_tree_model_get_path(GTK_TREE_MODEL(m->store), &iter))) {
                gpointer c;
                gtk_tree_model_get(GTK_TREE_MODEL(m->store), &iter, 6, &c, -1);
                g_ptr_array_add(to_delete, c);
            }
        } while (gtk_tree_model_iter_next(GTK_TREE_MODEL(m->store), &iter));
    }
    if (!to_delete->len) {
        g_ptr_array_free(to_delete, TRUE);
        return;
    }
    guint n = to_delete->len;
    /* The selected cookies are handed to WebKit and freed by
     * on_delete_cookie_done(); free the rest of the jar now (the refresh
     * below re-fetches everything from WebKit). */
    for (GList *l = m->cookies; l; l = l->next)
        if (g_ptr_array_find(to_delete, l->data, NULL) < 0)
            soup_cookie_free(l->data);
    g_list_free(m->cookies);
    m->cookies = NULL;
    /* drop the stale cookie pointers from column 6 until the refresh lands */
    gtk_list_store_clear(m->store);
    for (guint i = 0; i < to_delete->len; i++)
        webkit_cookie_manager_delete_cookie(
            cookie_manager_of(m->b),
            (SoupCookie *)g_ptr_array_index(to_delete, i),
            NULL, on_delete_cookie_done,
            (gpointer)g_ptr_array_index(to_delete, i));
    g_ptr_array_free(to_delete, TRUE);
    char msg[32];
    g_snprintf(msg, sizeof(msg), "Deleted %u cookie%s", (unsigned)n,
               n == 1 ? "" : "s");
    cookie_set_status(m, msg);
    if (m->refresh_timer)
        g_source_remove(m->refresh_timer);
    /* the deletes run in the network process; refresh shortly after */
    m->refresh_timer = g_timeout_add(400, cookie_refresh_later, m);
}

static void cookie_clear_all(GtkButton *btn, gpointer data)
{
    (void)btn;
    CookieCtx *m = data;
    GtkWidget *confirm = gtk_message_dialog_new(
        GTK_WINDOW(m->win), GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_YES_NO,
        "Delete all cookies?\n\nYou will be signed out of every site.");
    gboolean yes = (gtk_dialog_run(GTK_DIALOG(confirm)) == GTK_RESPONSE_YES);
    gtk_widget_destroy(confirm);
    if (!yes)
        return;
    WebKitWebsiteDataManager *dm = webkit_web_context_get_website_data_manager(
        webkit_web_context_get_default());
    cookie_set_status(m, "Deleting…");
    webkit_website_data_manager_clear(dm, WEBKIT_WEBSITE_DATA_COOKIES, 0,
                                      NULL, NULL, NULL);
    if (m->refresh_timer)
        g_source_remove(m->refresh_timer);
    m->refresh_timer = g_timeout_add(600, cookie_refresh_later, m);
}

static void cookie_export(GtkButton *btn, gpointer data)
{
    (void)btn;
    CookieCtx *m = data;
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Export cookies", GTK_WINDOW(m->win), GTK_FILE_CHOOSER_ACTION_SAVE,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(chooser), "cookies.txt");
    if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(chooser);
        return;
    }
    char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);

    GString *out = g_string_new(
        "# HTTP Cookie File\n"
        "# https://curl.se/docs/http-cookies.html\n"
        "# This file was generated by GtkBrowser.\n");
    int n = 0;
    for (GList *l = m->cookies; l; l = l->next) {
        SoupCookie *c = l->data;
        const char *site = soup_cookie_get_domain(c);
        const char *path = soup_cookie_get_path(c);
        GDateTime *exp = soup_cookie_get_expires(c);
        g_string_append_printf(out, "%s\t%s\t%s\t%s\t%lld\t%s\t%s\n",
                               site ? site : "",
                               (site && site[0] == '.') ? "TRUE" : "FALSE",
                               path ? path : "/",
                               soup_cookie_get_secure(c) ? "TRUE" : "FALSE",
                               exp ? (long long)g_date_time_to_unix(exp) : 0LL,
                               soup_cookie_get_name(c),
                               soup_cookie_get_value(c));
        n++;
    }
    if (!g_file_set_contents(file, out->str, -1, NULL)) {
        cookie_set_status(m, "Export failed: could not write file.");
    } else {
        char *base = g_path_get_basename(file);
        char *msg = g_strdup_printf("Exported %d cookie%s to %s",
                                    n, n == 1 ? "" : "s", base);
        g_free(base);
        cookie_set_status(m, msg);
        g_free(msg);
    }
    g_free(file);
    g_string_free(out, TRUE);
}

static void cookie_import(GtkButton *btn, gpointer data)
{
    (void)btn;
    CookieCtx *m = data;
    GtkWidget *chooser = gtk_file_chooser_dialog_new(
        "Import cookies (cookies.txt)", GTK_WINDOW(m->win),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Cancel", GTK_RESPONSE_CANCEL, "_Import", GTK_RESPONSE_ACCEPT, NULL);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(chooser);
        return;
    }
    char *file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    gtk_widget_destroy(chooser);

    gchar *contents = NULL;
    if (!g_file_get_contents(file, &contents, NULL, NULL)) {
        cookie_set_status(m, "Import failed: could not read file.");
        g_free(file);
        return;
    }
    WebKitCookieManager *cm = cookie_manager_of(m->b);
    char **lines = g_strsplit(contents, "\n", -1);
    int added = 0;
    for (int i = 0; lines[i]; i++) {
        char *line = g_strchomp(lines[i]);
        if (!*line || line[0] == '#')
            continue;
        char **f = g_strsplit(line, "\t", 7);
        if (g_strv_length(f) != 7 || !f[5][0]) {
            g_strfreev(f);
            continue;
        }
        /* Netscape format: domain flag path secure expiry name value */
        long long exp = g_ascii_strtoll(f[4], NULL, 10);
        SoupCookie *c = soup_cookie_new(f[5], f[6], f[0], f[2],
                                        exp > 0 ? (int)(exp - time(NULL)) : -1);
        if (exp > 0)
            soup_cookie_set_expires(c, g_date_time_new_from_unix_utc(exp));
        soup_cookie_set_secure(c, g_ascii_strcasecmp(f[3], "TRUE") == 0);
        webkit_cookie_manager_add_cookie(cm, c, NULL, NULL, NULL);
        soup_cookie_free(c); /* converted synchronously by WebKit */
        added++;
        g_strfreev(f);
    }
    g_strfreev(lines);
    g_free(contents);
    char *base = g_path_get_basename(file);
    char *msg = g_strdup_printf("Imported %d cookie%s from %s",
                                added, added == 1 ? "" : "s", base);
    g_free(base);
    g_free(file);
    cookie_set_status(m, msg);
    g_free(msg);
    if (m->refresh_timer)
        g_source_remove(m->refresh_timer);
    m->refresh_timer = g_timeout_add(600, cookie_refresh_later, m);
}

static void on_cookie_dialog_destroy(GtkWidget *w, gpointer data)
{
    (void)w;
    CookieCtx *m = data;
    m->win = NULL; /* pending async callbacks are guarded by this */
    if (m->refresh_timer)
        g_source_remove(m->refresh_timer);
    if (!m->loading) { /* otherwise on_cookies_fetched() frees it */
        cookie_free_list(m->cookies);
        g_free(m);
    }
}

static void on_manage_cookies_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    CookieCtx *m = g_new0(CookieCtx, 1);
    m->b = b;
    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    m->win = win;
    gtk_window_set_title(GTK_WINDOW(win), "Cookies");
    gtk_window_set_default_size(GTK_WINDOW(win), 840, 540);
    gtk_window_set_transient_for(GTK_WINDOW(win), GTK_WINDOW(win_toplevel(b)));
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    g_signal_connect(win, "destroy", G_CALLBACK(on_cookie_dialog_destroy), m);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 8);
    gtk_container_add(GTK_CONTAINER(win), vbox);

    m->store = gtk_list_store_new(7, G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_STRING, G_TYPE_STRING,
                                  G_TYPE_POINTER);
    m->tv = GTK_TREE_VIEW(gtk_tree_view_new_with_model(GTK_TREE_MODEL(m->store)));
    gtk_tree_selection_set_mode(gtk_tree_view_get_selection(GTK_TREE_VIEW(m->tv)),
                                GTK_SELECTION_MULTIPLE);
    const char *cols[] = { "Site", "Name", "Value", "Expires", "Flags", "Shared" };
    for (int i = 0; i < 6; i++) {
        GtkCellRenderer *r = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *c = gtk_tree_view_column_new_with_attributes(
            cols[i], r, "text", i, NULL);
        if (i == 0 || i == 1)
            gtk_tree_view_column_set_expand(c, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(m->tv), c);
    }
    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_set_border_width(GTK_CONTAINER(sw), 6);
    gtk_container_add(GTK_CONTAINER(sw), GTK_WIDGET(m->tv));
    gtk_box_pack_start(GTK_BOX(vbox), sw, TRUE, TRUE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    m->shared_only = gtk_check_button_new_with_label(
        "Show shared (third-party) cookies only");
    gtk_widget_set_tooltip_text(m->shared_only,
        "\"Shared\" = the cookie was set by a site other than the one you "
        "are browsing, so it follows you across sites.");
    g_signal_connect(m->shared_only, "toggled", G_CALLBACK(on_shared_only_toggled), m);
    GtkWidget *refresh_btn = gtk_button_new_with_label("_Refresh");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK(on_cookie_refresh_clicked), m);
    GtkWidget *del_btn = gtk_button_new_with_label("Delete selected");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(cookie_delete_selected), m);
    GtkWidget *clear_btn = gtk_button_new_with_label("Clear all…");
    g_signal_connect(clear_btn, "clicked", G_CALLBACK(cookie_clear_all), m);
    GtkWidget *export_btn = gtk_button_new_with_label("Export…");
    g_signal_connect(export_btn, "clicked", G_CALLBACK(cookie_export), m);
    GtkWidget *import_btn = gtk_button_new_with_label("Import…");
    g_signal_connect(import_btn, "clicked", G_CALLBACK(cookie_import), m);
    gtk_box_pack_start(GTK_BOX(row), m->shared_only, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), refresh_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), del_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), clear_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), export_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), import_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), row, FALSE, FALSE, 0);

    m->status = gtk_label_new("Loading…");
    gtk_widget_set_halign(m->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(vbox), m->status, FALSE, FALSE, 0);

    gtk_widget_show_all(win);
    cookie_refresh(m);
    gtk_widget_grab_focus(win_toplevel(b));
}

static void on_settings_clicked(GtkButton *btn, Browser *b)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Settings",
                                                    GTK_WINDOW(win_toplevel(b)),
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel", GTK_RESPONSE_REJECT,
                                                    "_Save", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);
    gtk_box_set_spacing(GTK_BOX(content), 8);

    GtkWidget *adblock_check = gtk_check_button_new_with_label(
        "Block ads and trackers (uBlock-like)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(adblock_check), b->adblock_enabled);
    GtkWidget *devtools_check = gtk_check_button_new_with_label("Enable developer tools");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(devtools_check), b->devtools_enabled);

    gtk_box_pack_start(GTK_BOX(content), adblock_check, FALSE, FALSE, 0);

    char blocked_buf[48];
    g_snprintf(blocked_buf, sizeof(blocked_buf), "Trackers blocked so far: %llu",
               (unsigned long long)(b->proxy ? adb_proxy_blocked_count(b->proxy) : 0));
    GtkWidget *blocked_info = gtk_label_new(blocked_buf);
    gtk_widget_set_halign(blocked_info, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), blocked_info, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(content), devtools_check, FALSE, FALSE, 0);

    GtkWidget *engine_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(engine_row, GTK_ALIGN_START);
    GtkWidget *engine_label = gtk_label_new("Search engine:");
    GtkWidget *engine_combo = gtk_combo_box_text_new();
    for (int i = 0; i < N_SEARCH_ENGINES; i++)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(engine_combo),
                                       SEARCH_ENGINES[i].name);
    gtk_combo_box_set_active(GTK_COMBO_BOX(engine_combo), b->search_engine);
    gtk_box_pack_start(GTK_BOX(engine_row), engine_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(engine_row), engine_combo, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), engine_row, FALSE, FALSE, 0);

    GtkWidget *custom_entry = setting_entry_row(content, "Custom search URL:",
                                                b->custom_search_base,
                                                "https://search.example.com/?q=%s",
                                                G_CALLBACK(on_custom_search_changed), b);

    GtkWidget *gen_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(content), gen_sep, FALSE, FALSE, 0);
    GtkWidget *gen_hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(gen_hdr), "<b>General</b>");
    gtk_widget_set_halign(gen_hdr, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), gen_hdr, FALSE, FALSE, 0);

    GtkWidget *home_entry = setting_entry_row(content, "Home page:",
                                          b->home_url, HOME_URL,
                                          G_CALLBACK(on_home_changed), b);
    gtk_widget_set_name(home_entry, "home-entry");
    GtkWidget *nt_entry = setting_entry_row(content, "New tab page:",
                                            b->new_tab_url, HOME_URL,
                                            G_CALLBACK(on_new_tab_url_changed), b);
    gtk_widget_set_name(nt_entry, "newtab-entry");

    char *hf_desc = home_display_text(b);
    char *hf_text = g_strdup_printf("Home: %s", hf_desc);
    GtkWidget *hf_lbl = gtk_label_new(hf_text);
    gtk_widget_set_name(hf_lbl, "homefile-label");
    gtk_widget_set_halign(hf_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(hf_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(content), hf_lbl, FALSE, FALSE, 0);
    g_free(hf_text);
    g_free(hf_desc);
    char *nt_desc = newtab_display_text(b);
    char *nt_text = g_strdup_printf("New tabs: %s", nt_desc);
    GtkWidget *nt_lbl = gtk_label_new(nt_text);
    gtk_widget_set_name(nt_lbl, "newtab-label");
    gtk_widget_set_halign(nt_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(nt_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(content), nt_lbl, FALSE, FALSE, 0);
    g_free(nt_text);
    g_free(nt_desc);
    GtkWidget *hf_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *hf_btn = gtk_button_new_with_label("Choose home page file…");
    gtk_widget_set_tooltip_text(hf_btn,
        "Your own start page: an HTML file, re-read on every home visit so "
        "edits show up immediately. Takes precedence over the home page URL.");
    g_signal_connect(hf_btn, "clicked", G_CALLBACK(on_home_file_browse), b);
    GtkWidget *hf_builtin = gtk_button_new_with_label("Use built-in start page");
    gtk_widget_set_tooltip_text(hf_builtin,
        "Clear the custom home file, the home page URL, and the new tab "
        "page URL so the home button / Ctrl+H and new tabs show the "
        "built-in GTK start page again.");
    g_signal_connect(hf_builtin, "clicked", G_CALLBACK(on_home_builtin), b);
    gtk_box_pack_start(GTK_BOX(hf_row), hf_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hf_row), hf_builtin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), hf_row, FALSE, FALSE, 0);

    GtkWidget *restore_check = gtk_check_button_new_with_label(
        "Restore last session (reopen previous tabs)");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(restore_check),
                                 b->restore_session);
    gtk_widget_set_halign(restore_check, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), restore_check, FALSE, FALSE, 0);

    GtkWidget *hide_tabbar_check = gtk_check_button_new_with_label(
        "Hide the tab bar when a window has a single tab");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(hide_tabbar_check),
                                 b->hide_tabbar);
    gtk_widget_set_halign(hide_tabbar_check, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), hide_tabbar_check, FALSE, FALSE, 0);

    GtkWidget *zoom_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(zoom_row, GTK_ALIGN_START);
    GtkWidget *zoom_lbl = gtk_label_new("Default zoom:");
    GtkWidget *zoom_spin = gtk_spin_button_new_with_range(50, 400, 5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(zoom_spin),
                              b->default_zoom ? b->default_zoom : 100);
    gtk_box_pack_start(GTK_BOX(zoom_row), zoom_lbl, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(zoom_row), zoom_spin, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), zoom_row, FALSE, FALSE, 0);

    setting_entry_row(content, "User agent:", b->user_agent, "(browser default)",
                      G_CALLBACK(on_user_agent_changed), b);

    char bl_text[512];
    g_snprintf(bl_text, sizeof(bl_text), "Blocklist: %s",
               b->blocklist_file ? b->blocklist_file : "(none configured)");
    GtkWidget *bl_lbl = gtk_label_new(bl_text);
    gtk_widget_set_name(bl_lbl, "blocklist-label");
    gtk_widget_set_halign(bl_lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(bl_lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(content), bl_lbl, FALSE, FALSE, 0);
    GtkWidget *bl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *bl_btn = gtk_button_new_with_label("Choose blocklist…");
    g_signal_connect(bl_btn, "clicked", G_CALLBACK(on_blocklist_browse), b);
    gtk_box_pack_start(GTK_BOX(bl_row), bl_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), bl_row, FALSE, FALSE, 0);

    GtkWidget *manage_btn = gtk_button_new_with_label(
        "Manage history and bookmarks…");
    gtk_widget_set_halign(manage_btn, GTK_ALIGN_START);
    g_signal_connect(manage_btn, "clicked", G_CALLBACK(on_manage_history_clicked), b);
    gtk_box_pack_start(GTK_BOX(content), manage_btn, FALSE, FALSE, 0);

    GtkWidget *perm_btn = gtk_button_new_with_label("Manage permissions…");
    gtk_widget_set_halign(perm_btn, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(perm_btn,
        "Site decisions from the permission prompts (allow / deny)");
    g_signal_connect(perm_btn, "clicked", G_CALLBACK(on_manage_permissions_clicked), b);
    gtk_box_pack_start(GTK_BOX(content), perm_btn, FALSE, FALSE, 0);

    GtkWidget *sc_btn = gtk_button_new_with_label("Manage shortcuts…");
    gtk_widget_set_halign(sc_btn, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(sc_btn, "Rebind the keyboard shortcuts");
    g_signal_connect(sc_btn, "clicked", G_CALLBACK(on_manage_shortcuts_clicked), b);
    gtk_box_pack_start(GTK_BOX(content), sc_btn, FALSE, FALSE, 0);

    GtkWidget *dl_sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(content), dl_sep, FALSE, FALSE, 0);
    GtkWidget *dl_hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(dl_hdr), "<b>Downloads</b>");
    gtk_widget_set_halign(dl_hdr, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), dl_hdr, FALSE, FALSE, 0);

    GtkWidget *dl_ask_check = gtk_check_button_new_with_label(
        "Ask where to save each download");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(dl_ask_check), b->download_ask);
    gtk_widget_set_halign(dl_ask_check, GTK_ALIGN_START);
    gtk_widget_set_tooltip_text(dl_ask_check,
        "Off: files are saved straight to the download folder below and "
        "tracked in the downloads list (toolbar button).");
    gtk_box_pack_start(GTK_BOX(content), dl_ask_check, FALSE, FALSE, 0);

    GtkWidget *dl_dir_entry = setting_entry_row(content, "Download folder:",
                                             b->download_dir,
                                             "Default (~/Downloads)",
                                             G_CALLBACK(on_download_dir_changed), b);
    gtk_widget_set_name(dl_dir_entry, "download-dir-entry");
    GtkWidget *dl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *dl_btn = gtk_button_new_with_label("Choose…");
    gtk_widget_set_tooltip_text(dl_btn,
        "Pick the folder auto-downloads are saved to");
    g_signal_connect(dl_btn, "clicked", G_CALLBACK(on_download_dir_browse), b);
    gtk_box_pack_start(GTK_BOX(dl_row), dl_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), dl_row, FALSE, FALSE, 0);

    GtkWidget *tpc_check = gtk_check_button_new_with_label(
        "Block third-party (shared) cookies");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(tpc_check), b->block_third_party);
    gtk_widget_set_halign(tpc_check, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), tpc_check, FALSE, FALSE, 0);
    GtkWidget *manage_cookies_btn = gtk_button_new_with_label("Manage cookies…");
    gtk_widget_set_halign(manage_cookies_btn, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), manage_cookies_btn, FALSE, FALSE, 0);

    kp_build_settings_section(b, content);

    g_signal_connect(adblock_check, "toggled", G_CALLBACK(on_adblock_toggled), b);
    g_signal_connect(devtools_check, "toggled", G_CALLBACK(on_devtools_toggled), b);
    g_signal_connect(engine_combo, "changed", G_CALLBACK(on_engine_changed), b);
    g_signal_connect(custom_entry, "changed", G_CALLBACK(on_custom_search_changed), b);
    g_signal_connect(zoom_spin, "value-changed", G_CALLBACK(on_zoom_changed), b);
    g_signal_connect(restore_check, "toggled", G_CALLBACK(on_restore_toggled), b);
    g_signal_connect(hide_tabbar_check, "toggled",
                     G_CALLBACK(on_hide_tabbar_toggled), b);
    g_signal_connect(dl_ask_check, "toggled",
                     G_CALLBACK(on_download_ask_toggled), b);
    g_signal_connect(tpc_check, "toggled",
                     G_CALLBACK(on_block_third_party_toggled), b);
    g_signal_connect(manage_cookies_btn, "clicked",
                     G_CALLBACK(on_manage_cookies_clicked), b);

    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        save_settings(b);
    gtk_widget_destroy(dialog);
}

/* Downloads settings handlers */
static void on_download_ask_toggled(GtkToggleButton *btn, Browser *b)
{
    b->download_ask = gtk_toggle_button_get_active(btn);
}

static void on_download_dir_changed(GtkEntry *entry, Browser *b)
{
    const char *t = gtk_entry_get_text(entry);
    g_free(b->download_dir);
    b->download_dir = (*t ? g_strdup(t) : NULL);
}

static void on_download_dir_browse(GtkButton *btn, Browser *b)
{
    (void)btn;
    GtkWidget *chooser = gtk_file_chooser_dialog_new("Choose download folder",
                                                    GTK_WINDOW(win_toplevel(b)),
                                                    GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Select", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    if (b->download_dir && g_file_test(b->download_dir, G_FILE_TEST_IS_DIR))
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), b->download_dir);
    else
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser),
                                            g_get_home_dir());
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        g_free(b->download_dir);
        b->download_dir = fn;
        save_settings(b);
    }
    gtk_widget_destroy(chooser);

    /* refresh the settings entry, which also fires its "changed" handler */
    GtkWidget *dlg = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_DIALOG);
    GtkWidget *entry = dlg ? find_named(dlg, "download-dir-entry") : NULL;
    if (entry)
        gtk_entry_set_text(GTK_ENTRY(entry),
                           b->download_dir ? b->download_dir : "");
}

/* ---------- KeePass integration ---------- */

static void secure_free(gchar **p)
{
    if (*p) {
        memset(*p, 0, strlen(*p));
        g_free(*p);
        *p = NULL;
    }
}

/* Encode a C string as a JSON string literal (with quotes). */
static char *json_escape(const char *s)
{
    GString *g = g_string_new("\"");
    for (const char *p = s ? s : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        switch (c) {
        case '"': g_string_append(g, "\\\""); break;
        case '\\': g_string_append(g, "\\\\"); break;
        case '\n': g_string_append(g, "\\n"); break;
        case '\r': g_string_append(g, "\\r"); break;
        case '\t': g_string_append(g, "\\t"); break;
        default:
            if (c < 0x20)
                g_string_append_printf(g, "\\u%04x", c);
            else
                g_string_append_c(g, (char)c);
        }
    }
    g_string_append_c(g, '"');
    return g_string_free(g, FALSE);
}

/* Settings persistence (only non-secret values; the password stays in
 * memory for the session). */
static char *settings_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtkbrowser",
                            "settings.ini", NULL);
}

static void load_settings(Browser *b)
{
    GKeyFile *kf = g_key_file_new();
    char *path = settings_path();
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) {
        b->kp_db_path = g_key_file_get_string(kf, "keepass", "db", NULL);
        int se = g_key_file_get_integer(kf, "search", "engine", NULL);
        if (se >= 0 && se < N_SEARCH_ENGINES)
            b->search_engine = se;
        b->custom_search_base = g_key_file_get_string(kf, "search", "custom", NULL);
        int zoom = g_key_file_get_integer(kf, "general", "zoom", NULL);
        if (zoom >= 50 && zoom <= 400)
            b->default_zoom = zoom;
        if (g_key_file_has_key(kf, "general", "restore", NULL))
            b->restore_session =
                g_key_file_get_integer(kf, "general", "restore", NULL) != 0;
        if (g_key_file_has_key(kf, "general", "hide_tabbar", NULL))
            b->hide_tabbar =
                g_key_file_get_integer(kf, "general", "hide_tabbar", NULL) != 0;
        if (g_key_file_has_key(kf, "cookies", "block_third_party", NULL))
            b->block_third_party =
                g_key_file_get_integer(kf, "cookies", "block_third_party", NULL) != 0;
        b->home_url = g_key_file_get_string(kf, "general", "home", NULL);
        b->home_file = g_key_file_get_string(kf, "general", "home_file", NULL);
        b->new_tab_url = g_key_file_get_string(kf, "general", "newtab", NULL);
        b->user_agent = g_key_file_get_string(kf, "general", "user_agent", NULL);
        if (!b->blocklist_file)
            b->blocklist_file = g_key_file_get_string(kf, "adblock", "blocklist", NULL);
        if (g_key_file_has_key(kf, "downloads", "ask", NULL))
            b->download_ask =
                g_key_file_get_integer(kf, "downloads", "ask", NULL) != 0;
        b->download_dir = g_key_file_get_string(kf, "downloads", "folder", NULL);
        sc_load(b, kf); /* needs b->sc[] defaulted by sc_init() first */
    }
    g_key_file_unref(kf);
    g_free(path);
}

static void save_settings(Browser *b)
{
    GKeyFile *kf = g_key_file_new();
    if (b->kp_db_path)
        g_key_file_set_string(kf, "keepass", "db", b->kp_db_path);
    g_key_file_set_integer(kf, "search", "engine", b->search_engine);
    if (b->custom_search_base)
        g_key_file_set_string(kf, "search", "custom", b->custom_search_base);
    if (b->home_url)
        g_key_file_set_string(kf, "general", "home", b->home_url);
    if (b->home_file)
        g_key_file_set_string(kf, "general", "home_file", b->home_file);
    if (b->new_tab_url)
        g_key_file_set_string(kf, "general", "newtab", b->new_tab_url);
    g_key_file_set_integer(kf, "general", "zoom",
                           b->default_zoom ? b->default_zoom : 100);
    if (b->user_agent)
        g_key_file_set_string(kf, "general", "user_agent", b->user_agent);
    if (b->blocklist_file)
        g_key_file_set_string(kf, "adblock", "blocklist", b->blocklist_file);
    g_key_file_set_integer(kf, "general", "restore",
                           b->restore_session ? 1 : 0);
    g_key_file_set_integer(kf, "general", "hide_tabbar",
                           b->hide_tabbar ? 1 : 0);
    g_key_file_set_integer(kf, "cookies", "block_third_party",
                           b->block_third_party ? 1 : 0);
    g_key_file_set_integer(kf, "downloads", "ask",
                           b->download_ask ? 1 : 0);
    if (b->download_dir)
        g_key_file_set_string(kf, "downloads", "folder", b->download_dir);
    sc_save(b, kf);
    char *path = settings_path();
    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    g_key_file_save_to_file(kf, path, NULL);
    g_key_file_unref(kf);
    g_free(path);
}

static gchar *ask_kp_password(Browser *b)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("KeePass database",
                                                    GTK_WINDOW(win_toplevel(b)),
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Unlock", GTK_RESPONSE_OK,
                                                    NULL);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *info = gtk_label_new(NULL);
    char *lbl = g_strdup_printf("Enter the password for:\n%s", b->kp_db_path);
    gtk_label_set_text(GTK_LABEL(info), lbl);
    g_free(lbl);
    gtk_widget_set_halign(info, GTK_ALIGN_START);
    GtkWidget *entry = gtk_entry_new();
    gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
    gtk_entry_set_invisible_char(GTK_ENTRY(entry), '*');
    gtk_box_pack_start(GTK_BOX(box), info, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(box), entry, FALSE, FALSE, 0);
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(entry);
    gchar *pw = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
        if (*t)
            pw = g_strdup(t);
    }
    gtk_widget_destroy(dialog);
    return pw;
}

static gboolean kp_job_done(gpointer data); /* defined below */

/* The database is read in a worker thread (keepassxc-cli takes a moment). */
typedef struct {
    Browser *b;
    char *host;
    GPtrArray *entries;
    char *error;
    gboolean is_test;
    int retried;
} KpJob;

static void kp_job_free(KpJob *job)
{
    for (guint i = 0; i < job->entries->len; i++)
        kp_entry_free(g_ptr_array_index(job->entries, i));
    g_ptr_array_free(job->entries, TRUE);
    g_free(job->host);
    g_free(job->error);
    g_free(job);
}

static gpointer kp_job_thread(gpointer data)
{
    KpJob *job = data;
    kp_load_entries(job->b->kp_db_path, job->b->kp_password, job->entries,
                    &job->error);
    g_idle_add(kp_job_done, job); /* back on the main thread */
    return job;
}

typedef struct {
    Browser *b;
    char *title;
    char *user;
} FillResult;

static void on_fill_js_done(GObject *source, GAsyncResult *res, gpointer user_data)
{
    FillResult *fr = user_data;
    GError *err = NULL;
    JSCValue *result = WEBKIT_IS_WEB_VIEW(source) ?
        webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), res,
                                                   &err) : NULL;
    gboolean ok = !err && result && jsc_value_is_boolean(result) &&
                  jsc_value_to_boolean(result);
    GtkWidget *d;
    if (ok) {
        char buf[160];
        g_snprintf(buf, sizeof(buf), "Filled %s (%s).", fr->title, fr->user);
        d = gtk_message_dialog_new(GTK_WINDOW(win_toplevel(fr->b)), GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", buf);
    } else {
        d = gtk_message_dialog_new(GTK_WINDOW(win_toplevel(fr->b)), GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
                                   err ? err->message
                                       : "No login form found on this page.");
    }
    gtk_dialog_run(GTK_DIALOG(d));
    gtk_widget_destroy(d);
    if (result)
        g_object_unref(result);
    g_clear_error(&err);
    g_free(fr->title);
    g_free(fr->user);
    g_free(fr);
}

/* Fill the current page's login form (password field + best-guess username
 * field in the same form) with the given entry's values. */
static void do_fill(Browser *b, KpEntry *e)
{
    Win *w = active_window(b);
    Tab *tab = w ? current_tab(w) : NULL;
    if (!tab)
        return;
    char *u = json_escape(e->username ? e->username : "");
    char *p = json_escape(e->password ? e->password : "");
    char *script = g_strdup_printf(
        "(function(){"
        "var u=%s,p=%s;"
        "var all=Array.prototype.slice.call(document.querySelectorAll('input'));"
        "var pwd=all.filter(function(el){return el.type==='password'&&el.offsetParent!==null;})[0];"
        "if(!pwd)return false;"
        "var scope=pwd.form?Array.prototype.slice.call(pwd.form.elements):all;"
        "var user=scope.filter(function(el){return el!==pwd&&el.offsetParent!==null&&(el.type==='email'||el.type==='text')&&/user|email|login|account|name/i.test(el.name||'');})[0];"
        "if(!user)user=scope.filter(function(el){return el!==pwd&&el.offsetParent!==null&&(el.type==='email'||el.type==='text');})[0];"
        "function set(el,v){el.focus();el.value=v;el.dispatchEvent(new Event('input',{bubbles:true}));el.dispatchEvent(new Event('change',{bubbles:true}));}"
        "if(user)set(user,u);"
        "set(pwd,p);pwd.focus();return true;"
        "})()", u, p);
    g_free(u);
    g_free(p);
    FillResult *fr = g_new0(FillResult, 1);
    fr->b = b;
    fr->title = g_strdup(e->title ? e->title : "");
    fr->user = g_strdup(e->username ? e->username : "");
    webkit_web_view_evaluate_javascript(tab->view, script, -1, NULL, NULL, NULL,
                                    on_fill_js_done, fr);
    g_free(script);
}

/* Pick one of several matching entries. Returns a borrowed KpEntry*. */
static KpEntry *choose_entry(Browser *b, const char *host, GPtrArray *matches)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Choose password",
                                                    GTK_WINDOW(win_toplevel(b)),
                                                    GTK_DIALOG_MODAL,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Use", GTK_RESPONSE_OK,
                                                    NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, 320);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    char hdr[160];
    g_snprintf(hdr, sizeof(hdr), "%u entries for %s", (unsigned)matches->len, host);
    GtkWidget *title = gtk_label_new(hdr);
    gtk_widget_set_halign(title, GTK_ALIGN_START);
    GtkWidget *list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_BROWSE);
    for (guint i = 0; i < matches->len; i++) {
        KpEntry *e = g_ptr_array_index(matches, i);
        char row[200];
        g_snprintf(row, sizeof(row), "%s — %s", e->title ? e->title : "?",
                   e->username ? e->username : "(no username)");
        GtkWidget *label = gtk_label_new(row);
        gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_list_box_insert(GTK_LIST_BOX(list), label, -1);
    }
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);
    GtkListBoxRow *first = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), 0);
    if (first)
        gtk_list_box_select_row(GTK_LIST_BOX(list), first);
    KpEntry *pick = NULL;
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        GtkListBoxRow *sel = gtk_list_box_get_selected_row(GTK_LIST_BOX(list));
        int idx = sel ? gtk_list_box_row_get_index(sel) : -1;
        if (idx >= 0 && idx < (int)matches->len)
            pick = g_ptr_array_index(matches, idx);
    }
    gtk_widget_destroy(dialog);
    return pick;
}

static void kp_start_job(Browser *b, const char *host, gboolean is_test)
{
    KpJob *job = g_new0(KpJob, 1);
    job->b = b;
    job->host = g_strdup(host ? host : "");
    job->is_test = is_test;
    job->entries = g_ptr_array_new();
    g_thread_new("keepass", kp_job_thread, job);
}

static gboolean kp_job_done(gpointer data)
{
    KpJob *job = data;
    Browser *b = job->b;
    GtkWidget *d;

    if (job->error && g_str_has_prefix(job->error, "INVALID_PW") && !job->retried) {
        /* cached password was wrong: clear it and ask once more */
        secure_free(&b->kp_password);
        gchar *pw = ask_kp_password(b);
        if (pw) {
            b->kp_password = pw;
            job->retried = 1;
            g_thread_new("keepass", kp_job_thread, job);
            return G_SOURCE_REMOVE;
        }
        kp_job_free(job);
        return G_SOURCE_REMOVE; /* user cancelled */
    }

    if (job->error) {
        d = gtk_message_dialog_new(GTK_WINDOW(win_toplevel(b)), GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
                                   "KeePass: %s", job->error);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        kp_job_free(job);
        return G_SOURCE_REMOVE;
    }

    if (job->is_test) {
        char msg[64];
        g_snprintf(msg, sizeof(msg), "%u entries loaded from the database.",
                   (unsigned)job->entries->len);
        d = gtk_message_dialog_new(GTK_WINDOW(win_toplevel(b)), GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", msg);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        kp_job_free(job);
        return G_SOURCE_REMOVE;
    }

    GPtrArray *matches = g_ptr_array_new();
    for (guint i = 0; i < job->entries->len; i++) {
        KpEntry *e = g_ptr_array_index(job->entries, i);
        if (kp_entry_matches_host(e, job->host))
            g_ptr_array_add(matches, e);
    }
    KpEntry *pick = NULL;
    if (matches->len == 1)
        pick = g_ptr_array_index(matches, 0);
    else if (matches->len > 1)
        pick = choose_entry(b, job->host, matches);
    else {
        d = gtk_message_dialog_new(GTK_WINDOW(win_toplevel(b)), GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                   "No KeePass entries found for %s.",
                                   job->host);
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
    }
    if (pick)
        do_fill(b, pick);
    g_ptr_array_free(matches, TRUE);
    kp_job_free(job);
    return G_SOURCE_REMOVE;
}

static void on_fill_button_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    fill_login(b);
}

static void fill_login(Browser *b)
{
    Win *w = active_window(b);
    Tab *tab = w ? current_tab(w) : NULL;
    if (!tab)
        return;
    GtkWidget *d;
    if (!b->kp_db_path) {
        d = gtk_message_dialog_new(GTK_WINDOW(win_toplevel(b)), GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                   "No KeePass database configured.\n\n"
                                   "Choose a .kdbx file in Settings.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    const char *uri = webkit_web_view_get_uri(tab->view);
    GError *err = NULL;
    GUri *u = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, &err) : NULL;
    char host[512] = {0};
    if (u) {
        const char *h = g_uri_get_host(u);
        if (h)
            g_strlcpy(host, h, sizeof(host));
        g_uri_unref(u);
    }
    g_clear_error(&err);
    if (!*host) {
        d = gtk_message_dialog_new(GTK_WINDOW(win_toplevel(b)), GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
                                   "Open a login page first, then press "
                                   "Ctrl+Shift+L again.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    if (!b->kp_password) {
        b->kp_password = ask_kp_password(b);
        if (!b->kp_password)
            return; /* cancelled */
    }
    kp_start_job(b, host, FALSE);
}

static GtkWidget *find_named(GtkWidget *w, const char *name)
{
    if (gtk_widget_get_name(w) && strcmp(gtk_widget_get_name(w), name) == 0)
        return w;
    if (GTK_IS_CONTAINER(w)) {
        GList *kids = gtk_container_get_children(GTK_CONTAINER(w));
        GList *k;
        GtkWidget *found = NULL;
        for (k = kids; k && !found; k = k->next)
            found = find_named(GTK_WIDGET(k->data), name);
        g_list_free(kids);
        return found;
    }
    return NULL;
}

static void choose_kp_database(GtkButton *btn, Browser *b)
{
    (void)btn;
    GtkWidget *chooser = gtk_file_chooser_dialog_new("Choose KeePass database",
                                                    GTK_WINDOW(win_toplevel(b)),
                                                    GTK_FILE_CHOOSER_ACTION_OPEN,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Open", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    GtkFileFilter *f = gtk_file_filter_new();
    gtk_file_filter_add_pattern(f, "*.kdbx");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), f);
    if (b->kp_db_path && g_file_test(b->kp_db_path, G_FILE_TEST_EXISTS))
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), b->kp_db_path);
    if (gtk_dialog_run(GTK_DIALOG(chooser)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
        g_free(b->kp_db_path);
        b->kp_db_path = fn;
        secure_free(&b->kp_password); /* new db, old password no longer valid */
        save_settings(b);
    }
    gtk_widget_destroy(chooser);

    /* refresh the label in the settings dialog */
    GtkWidget *dlg = gtk_widget_get_ancestor(GTK_WIDGET(btn), GTK_TYPE_DIALOG);
    GtkWidget *lbl = dlg ? find_named(dlg, "kp-db-label") : NULL;
    if (lbl) {
        char *text = g_strdup_printf("Database: %s",
                                     b->kp_db_path ? b->kp_db_path
                                                   : "(none configured)");
        gtk_label_set_text(GTK_LABEL(lbl), text);
        g_free(text);
    }
}

static void on_kp_test_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    if (!b->kp_db_path) {
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(win_toplevel(b)),
                                              GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                                              GTK_BUTTONS_OK,
                                              "Choose a database first.");
        gtk_dialog_run(GTK_DIALOG(d));
        gtk_widget_destroy(d);
        return;
    }
    if (!b->kp_password) {
        b->kp_password = ask_kp_password(b);
        if (!b->kp_password)
            return;
    }
    kp_start_job(b, NULL, TRUE);
}

/* Build the KeePass section of the settings dialog. */
static void kp_build_settings_section(Browser *b, GtkWidget *content)
{
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(content), sep, FALSE, FALSE, 0);

    GtkWidget *hdr = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(hdr), "<b>KeePass passwords</b>");
    gtk_widget_set_halign(hdr, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(content), hdr, FALSE, FALSE, 0);

    if (!kp_have_backend()) {
        GtkWidget *warn = gtk_label_new(
            "keepassxc-cli not found in PATH. Install KeePassXC "
            "(apt install keepassxc) to use password filling.");
        gtk_widget_set_halign(warn, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(content), warn, FALSE, FALSE, 0);
        return;
    }

    char db_text[512];
    g_snprintf(db_text, sizeof(db_text), "Database: %s",
               b->kp_db_path ? b->kp_db_path : "(none configured)");
    GtkWidget *lbl = gtk_label_new(db_text);
    gtk_widget_set_name(lbl, "kp-db-label");
    gtk_widget_set_halign(lbl, GTK_ALIGN_START);
    gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(content), lbl, FALSE, FALSE, 0);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    GtkWidget *choose = gtk_button_new_with_label("Choose database…");
    g_signal_connect(choose, "clicked", G_CALLBACK(choose_kp_database), b);
    GtkWidget *test = gtk_button_new_with_label("Test…");
    g_signal_connect(test, "clicked", G_CALLBACK(on_kp_test_clicked), b);
    gtk_box_pack_start(GTK_BOX(row), choose, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), test, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(content), row, FALSE, FALSE, 0);
}

/* ---------- history, bookmarks, autocomplete ---------- */

static gboolean is_web_uri(const char *uri)
{
    return uri && (g_str_has_prefix(uri, "http://") ||
                   g_str_has_prefix(uri, "https://"));
}

static void ensure_data_dir(void)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "gtkbrowser", NULL);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
}

static char *history_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtkbrowser",
                            "history", NULL);
}

static char *bookmarks_path(void)
{
    return g_build_filename(g_get_user_config_dir(), "gtkbrowser",
                            "bookmarks", NULL);
}

static void history_save(Browser *b)
{
    if (!b->history)
        return;
    ensure_data_dir();
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < b->history->len; i++)
        g_string_append_printf(s, "%s\n",
                               (const char *)g_ptr_array_index(b->history, i));
    char *path = history_path();
    g_file_set_contents(path, s->str, s->len, NULL);
    g_free(path);
    g_string_free(s, TRUE);
}

static void load_history(Browser *b)
{
    ensure_data_dir();
    b->history = g_ptr_array_new();
    char *path = history_path();
    GError *err = NULL;
    gchar *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, &err)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++)
            if (*lines[i])
                g_ptr_array_add(b->history, g_strdup(lines[i]));
        g_strfreev(lines);
        g_free(contents);
    }
    g_clear_error(&err);
    g_free(path);
    while (b->history->len > HISTORY_MAX) {
        g_free(g_ptr_array_index(b->history, b->history->len - 1));
        g_ptr_array_remove_index(b->history, b->history->len - 1);
    }
}

static void history_add(Browser *b, const char *uri)
{
    if (!b->history || !is_web_uri(uri))
        return;
    /* move the URL to the front (drops older duplicates) */
    for (guint i = 0; i < b->history->len; i++) {
        char *old = g_ptr_array_index(b->history, i);
        if (g_strcmp0(old, uri) == 0) {
            g_free(old);
            g_ptr_array_remove_index(b->history, i);
            break;
        }
    }
    g_ptr_array_insert(b->history, 0, g_strdup(uri));
    while (b->history->len > HISTORY_MAX) {
        g_free(g_ptr_array_index(b->history, b->history->len - 1));
        g_ptr_array_remove_index(b->history, b->history->len - 1);
    }
    history_save(b);
    refresh_url_model(b);
}

static void bookmarks_save(Browser *b)
{
    if (!b->bookmarks)
        return;
    ensure_data_dir();
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < b->bookmarks->len; i++) {
        Bookmark *bm = g_ptr_array_index(b->bookmarks, i);
        char *title = g_strdup(bm->title);
        for (char *c = title; *c; c++)
            if (*c == '\t' || *c == '\n')
                *c = ' ';
        g_string_append_printf(s, "%s\t%s\n", bm->uri, title);
        g_free(title);
    }
    char *path = bookmarks_path();
    g_file_set_contents(path, s->str, s->len, NULL);
    g_free(path);
    g_string_free(s, TRUE);
}

static void load_bookmarks(Browser *b)
{
    ensure_data_dir();
    b->bookmarks = g_ptr_array_new();
    char *path = bookmarks_path();
    GError *err = NULL;
    gchar *contents = NULL;
    if (g_file_get_contents(path, &contents, NULL, &err)) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (int i = 0; lines[i]; i++) {
            if (!*lines[i])
                continue;
            Bookmark *bm = g_new0(Bookmark, 1);
            char *tab = strchr(lines[i], '\t');
            if (tab) {
                *tab = '\0';
                bm->title = g_strdup(tab + 1);
            } else {
                bm->title = g_strdup(lines[i]);
            }
            bm->uri = g_strdup(lines[i]);
            g_ptr_array_add(b->bookmarks, bm);
        }
        g_strfreev(lines);
        g_free(contents);
    }
    g_clear_error(&err);
    g_free(path);
}

static void bookmark_remove(Browser *b, const char *uri)
{
    if (!b->bookmarks)
        return;
    for (guint i = 0; i < b->bookmarks->len; i++) {
        Bookmark *bm = g_ptr_array_index(b->bookmarks, i);
        if (g_strcmp0(bm->uri, uri) == 0) {
            g_free(bm->uri);
            g_free(bm->title);
            g_free(bm);
            g_ptr_array_remove_index(b->bookmarks, i);
            break;
        }
    }
    bookmarks_save(b);
    refresh_url_model(b);
}

static void bookmark_add(Browser *b, const char *uri, const char *title)
{
    if (!b->bookmarks || !is_web_uri(uri))
        return;
    for (guint i = 0; i < b->bookmarks->len; i++)
        if (g_strcmp0(((Bookmark *)g_ptr_array_index(b->bookmarks, i))->uri,
                      uri) == 0)
            return;
    Bookmark *bm = g_new0(Bookmark, 1);
    bm->uri = g_strdup(uri);
    bm->title = g_strdup(title && *title ? title : uri);
    g_ptr_array_add(b->bookmarks, bm);
    bookmarks_save(b);
    refresh_url_model(b);
}

static void bookmark_toggle(Browser *b, const char *uri, const char *title)
{
    if (!b->bookmarks || !is_web_uri(uri))
        return;
    for (guint i = 0; i < b->bookmarks->len; i++)
        if (g_strcmp0(((Bookmark *)g_ptr_array_index(b->bookmarks, i))->uri,
                      uri) == 0) {
            bookmark_remove(b, uri);
            return;
        }
    bookmark_add(b, uri, title);
}

/* Rebuild the URL-bar completion list: bookmarks first (stable), then
 * history (most recent first), duplicates dropped. */
static void refresh_url_model(Browser *b)
{
    if (!b->url_model)
        return;
    gtk_list_store_clear(b->url_model);
    GPtrArray *seen = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *lists[2] = { b->bookmarks ? b->bookmarks : NULL,
                            b->history ? b->history : NULL };
    for (int which = 0; which < 2; which++) {
        if (!lists[which])
            continue;
        for (guint i = 0; i < lists[which]->len; i++) {
            const char *uri;
            if (which == 0)
                uri = ((Bookmark *)g_ptr_array_index(lists[which], i))->uri;
            else
                uri = g_ptr_array_index(lists[which], i);
            gboolean dup = FALSE;
            for (guint j = 0; j < seen->len; j++)
                if (g_strcmp0(g_ptr_array_index(seen, j), uri) == 0)
                    dup = TRUE;
            if (dup)
                continue;
            GtkTreeIter iter;
            gtk_list_store_append(b->url_model, &iter);
            gtk_list_store_set(b->url_model, &iter, 0, uri, -1);
            g_ptr_array_add(seen, g_strdup(uri));
        }
    }
    g_ptr_array_unref(seen);
}

/* ---------- bookmarks manager ---------- */

static void on_bookmark_remove_clicked(GtkButton *btn, gpointer list);

static void build_bookmarks_list(Browser *b, GtkWidget *list)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
    for (GList *l = rows; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(rows);
    if (b->bookmarks->len == 0) {
        GtkWidget *empty = gtk_label_new(
            "No bookmarks yet — press Ctrl+D on a page to add one.");
        gtk_container_add(GTK_CONTAINER(list), empty);
        return;
    }
    for (guint i = 0; i < b->bookmarks->len; i++) {
        Bookmark *bm = g_ptr_array_index(b->bookmarks, i);
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        GtkWidget *lbl = gtk_label_new(NULL);
        char *text = g_strdup_printf("%s\n%s", bm->title, bm->uri);
        gtk_label_set_text(GTK_LABEL(lbl), text);
        gtk_label_set_line_wrap(GTK_LABEL(lbl), TRUE);
        /* not selectable: a click must fall through to the row so it
         * opens the bookmark (selectable labels eat the press) */
        g_free(text);
        gtk_widget_set_hexpand(lbl, TRUE);
        GtkWidget *remove = gtk_button_new_with_mnemonic("_Remove");
        gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), remove, FALSE, FALSE, 0);
        g_object_set_data_full(G_OBJECT(row), "uri", g_strdup(bm->uri),
                               g_free);
        g_signal_connect(remove, "clicked",
                         G_CALLBACK(on_bookmark_remove_clicked), list);
        g_signal_connect(row, "button-press-event",
                          G_CALLBACK(on_bookmark_row_press), b);
        gtk_container_add(GTK_CONTAINER(list), row);
    }
}

static void on_bookmark_remove_clicked(GtkButton *btn, gpointer list)
{
    (void)btn;
    GtkWidget *row = gtk_widget_get_parent(GTK_WIDGET(btn));
    char *uri = row ? g_object_get_data(G_OBJECT(row), "uri") : NULL;
    Browser *b = g_object_get_data(G_OBJECT(list), "browser");
    if (uri && b) {
        bookmark_remove(b, uri);
        build_bookmarks_list(b, GTK_WIDGET(list));
    }
}

/* Single click on a row opens the bookmark in a new tab and closes the
 * dialog (the "Remove" button consumes its own click, so it is safe). */
static gboolean on_bookmark_row_press(GtkWidget *row, GdkEventButton *e,
                                      Browser *b)
{
    if (e->type != GDK_BUTTON_PRESS || e->button != 1)
        return FALSE;
    const char *uri = g_object_get_data(G_OBJECT(row), "uri");
    if (!uri)
        return FALSE;
    Win *w = active_window(b);
    if (w)
        new_tab(w, uri);
    GtkWidget *dlg = gtk_widget_get_ancestor(row, GTK_TYPE_DIALOG);
    if (dlg)
        gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_CLOSE);
    return TRUE;
}

/* Enter on the focused row (row-activated) does the same. */
static void on_bookmark_row_activated(GtkListBox *list, GtkListBoxRow *row,
                                      Browser *b)
{
    (void)list;
    GtkWidget *inner = gtk_bin_get_child(GTK_BIN(row));
    char *uri = inner ? g_object_get_data(G_OBJECT(inner), "uri") : NULL;
    if (!uri)
        return;
    Win *w = active_window(b);
    if (w)
        new_tab(w, uri);
    GtkWidget *dlg = gtk_widget_get_ancestor(GTK_WIDGET(row), GTK_TYPE_DIALOG);
    if (dlg)
        gtk_dialog_response(GTK_DIALOG(dlg), GTK_RESPONSE_CLOSE);
}

static void on_bookmarks_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(
        "Bookmarks", GTK_WINDOW(win_toplevel(b)), GTK_DIALOG_MODAL,
        "_Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 400);
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(content), 12);

    GtkWidget *hint = gtk_label_new(
        "Click a bookmark to open it in a new tab. "
        "Ctrl+D toggles the current page.");
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_box_pack_start(GTK_BOX(content), hint, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);

    GtkWidget *list = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scroll), list);
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);

    g_object_set_data(G_OBJECT(list), "browser", b);
    build_bookmarks_list(b, list);
    g_signal_connect(list, "row-activated",
                     G_CALLBACK(on_bookmark_row_activated), b);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

/* ---------- tab cycling ---------- */

static void cycle_tab(Win *win, int delta)
{
    GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
    int n = gtk_notebook_get_n_pages(nb);
    if (n <= 1)
        return;
    int cur = gtk_notebook_get_current_page(nb);
    int next = ((cur + delta) % n + n) % n;
    if (next != cur)
        gtk_notebook_set_current_page(nb, next);
}

/* ---------- search engine ---------- */

static void on_engine_changed(GtkComboBox *combo, Browser *b)
{
    int idx = gtk_combo_box_get_active(combo);
    if (idx >= 0 && idx < N_SEARCH_ENGINES) {
        b->search_engine = idx;
        save_settings(b);
    }
}

static void on_custom_search_changed(GtkEntry *entry, Browser *b)
{
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
    g_free(b->custom_search_base);
    b->custom_search_base = (t && *t) ? g_strdup(t) : NULL;
    save_settings(b);
}

/* ---------- page source / save (Ctrl+U / Ctrl+S) ----------
 * This WebKit build has no view-source handler and no print_to_pdf, so the
 * page's HTML is fetched with JavaScript. View source loads it from a temp
 * file (a data: URI of the same size stalls the load in this build), and
 * saving writes it to a file the user picks. */

#define JS_GET_SOURCE "document.documentElement ? document.documentElement.outerHTML : ''"

static char *page_slug(const char *title)
{
    char buf[128];
    int n = 0;
    if (title) {
        for (int i = 0; title[i] && n < (int)sizeof(buf) - 1; i++)
            if (g_ascii_isalnum((guchar)title[i]))
                buf[n++] = title[i];
    }
    if (n == 0)
        g_strlcpy(buf, "page", sizeof(buf));
    else
        buf[n] = '\0';
    return g_strdup(buf);
}

static void on_source_fetched_view(GObject *src, GAsyncResult *res, gpointer user_data)
{
    Tab *tab = user_data;
    JSCValue *val = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, NULL);
    char *html = val ? jsc_value_to_string(val) : g_strdup("");
    g_free(tab->source_file);
    tab->source_file = g_strdup_printf("%s/.gtkbrowser-source-%d.html",
                                       g_get_tmp_dir(), (int)getpid());
    g_file_set_contents(tab->source_file, html, (gssize)strlen(html), NULL);
    char *uri = g_strdup_printf("file://%s", tab->source_file);
    webkit_web_view_load_uri(tab->view, uri);
    g_free(uri);
    g_free(html);
}

static void on_source_fetched_save(GObject *src, GAsyncResult *res, gpointer user_data)
{
    Tab *tab = user_data;
    Browser *b = tab->win->browser;
    JSCValue *val = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(src), res, NULL);
    char *html = val ? jsc_value_to_string(val) : g_strdup("");
    GtkWidget *dlg = gtk_file_chooser_dialog_new("Save page as HTML",
                                                 GTK_WINDOW(win_toplevel(b)),
                                                 GTK_FILE_CHOOSER_ACTION_SAVE,
                                                 "_Cancel", GTK_RESPONSE_CANCEL,
                                                 "_Save", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dlg), GTK_RESPONSE_ACCEPT);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dlg), TRUE);
    char *slug = page_slug(webkit_web_view_get_title(tab->view));
    char *name = g_strdup_printf("%s.html", slug);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg), name);
    g_free(name);
    g_free(slug);
    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        char *path = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dlg));
        if (path) {
            GError *err = NULL;
            if (!g_file_set_contents(path, html, (gssize)strlen(html), &err))
                g_warning("Saving the page failed: %s",
                          err ? err->message : "unknown error");
            g_free(path);
        }
    }
    gtk_widget_destroy(dlg);
    g_free(html);
}

static void view_source_current(WebKitWebView *view, Tab *tab)
{
    webkit_web_view_evaluate_javascript(view, JS_GET_SOURCE, -1, NULL, NULL, NULL,
                                        on_source_fetched_view, tab);
}

static void save_page_current(WebKitWebView *view, Tab *tab)
{
    webkit_web_view_evaluate_javascript(view, JS_GET_SOURCE, -1, NULL, NULL, NULL,
                                        on_source_fetched_save, tab);
}

static void drop_source_file(Tab *tab, const char *uri)
{
    if (!tab->source_file)
        return;
    char *src_uri = g_strdup_printf("file://%s", tab->source_file);
    gboolean keeping = uri && g_str_equal(uri, src_uri);
    g_free(src_uri);
    if (keeping)
        return;
    unlink(tab->source_file);
    g_free(tab->source_file);
    tab->source_file = NULL;
}

/* ---------- page context menu ----------
 *
 * WebKit hands the ::context-menu handler its default menu; we suppress it
 * (returning TRUE) and show our own, combining the page's copy/edit with the
 * browser's actions. The hit-test context says what was right-clicked. */

/* labelled item wired to cb(item, user_data) */
static void ctx_add(GtkWidget *menu, const char *label,
                    GCallback cb, gpointer user_data)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    if (cb)
        g_signal_connect(item, "activate", cb, user_data);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void ctx_sep(GtkWidget *menu)
{
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), gtk_separator_menu_item_new());
}

/* item whose attached URI is opened/copied via open_cb(item, user_data) */
static void ctx_uri(GtkWidget *menu, const char *label, const char *uri,
                    Tab *tab, GCallback open_cb)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_object_set_data_full(G_OBJECT(item), "uri", g_strdup(uri), g_free);
    g_signal_connect(item, "activate", open_cb, tab);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

/* item that runs document.execCommand(attach) on the page */
static void ctx_exec(GtkMenuItem *item, gpointer user_data)
{
    Tab *tab = user_data;
    const char *cmd = g_object_get_data(G_OBJECT(item), "cmd");
    char *js = g_strdup_printf("document.execCommand('%s')", cmd);
    webkit_web_view_evaluate_javascript(tab->view, js, -1, NULL, NULL, NULL,
                                        NULL, NULL);
    g_free(js);
}

static void ctx_exec_item(GtkWidget *menu, const char *label, Tab *tab,
                          const char *cmd)
{
    GtkWidget *item = gtk_menu_item_new_with_label(label);
    g_object_set_data_full(G_OBJECT(item), "cmd", g_strdup(cmd), g_free);
    g_signal_connect(item, "activate", G_CALLBACK(ctx_exec), tab);
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
}

static void ctx_open_tab(GtkMenuItem *item, gpointer user_data)
{
    Tab *tab = user_data;
    const char *uri = g_object_get_data(G_OBJECT(item), "uri");
    if (uri)
        new_tab(tab->win, uri);
}

/* Start an explicit download of the right-clicked link. This WebKit build
 * never turns a navigation into a download on its own, so this is the
 * entry point to the downloads manager for files. */
static void ctx_download(GtkMenuItem *item, gpointer user_data)
{
    (void)user_data;
    const char *uri = g_object_get_data(G_OBJECT(item), "uri");
    if (uri)
        webkit_web_context_download_uri(webkit_web_context_get_default(), uri);
}

static void ctx_open_window(GtkMenuItem *item, gpointer user_data)
{
    Tab *tab = user_data;
    const char *uri = g_object_get_data(G_OBJECT(item), "uri");
    if (uri)
        new_tab(new_window(tab->win->browser), uri);
}

static void ctx_copy_uri(GtkMenuItem *item, gpointer user_data)
{
    (void)user_data;
    const char *uri = g_object_get_data(G_OBJECT(item), "uri");
    if (!uri)
        return;
    GtkClipboard *clip = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    gtk_clipboard_set_text(clip, uri, -1);
}

/* fetch the selection, search it in a new tab */
static void ctx_search_done(GObject *src, GAsyncResult *res, gpointer user_data)
{
    Tab *tab = user_data;
    JSCValue *val = webkit_web_view_evaluate_javascript_finish(
        WEBKIT_WEB_VIEW(src), res, NULL);
    char *sel = val ? jsc_value_to_string(val) : NULL;
    if (sel && *sel) {
        char *q = g_uri_escape_string(sel, NULL, TRUE);
        char *url = build_search_url(search_template(tab->win->browser), q);
        new_tab(tab->win, url);
        g_free(url);
        g_free(q);
    }
    g_free(sel);
}

static void ctx_search(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    Tab *tab = user_data;
    webkit_web_view_evaluate_javascript(tab->view, "window.getSelection().toString()",
                                        -1, NULL, NULL, NULL, ctx_search_done, tab);
}

static void ctx_view_source(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    Tab *tab = user_data;
    view_source_current(tab->view, tab);
}

static void ctx_save_page(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    Tab *tab = user_data;
    save_page_current(tab->view, tab);
}

static void ctx_reload(GtkMenuItem *item, gpointer user_data)
{
    (void)item;
    Tab *tab = user_data;
    webkit_web_view_reload(tab->view);
}

static gboolean on_view_context_menu(WebKitWebView *view, GtkWidget *menu,
                                     GdkEvent *event, WebKitHitTestResult *hit,
                                     gpointer user_data)
{
    (void)view;
    (void)menu; /* WebKit's default menu is unused; we build our own */
    Tab *tab = user_data;
    const char *link = webkit_hit_test_result_get_link_uri(hit);
    const char *image = webkit_hit_test_result_get_image_uri(hit);
    const char *media = webkit_hit_test_result_get_media_uri(hit);
    gboolean editable = webkit_hit_test_result_context_is_editable(hit);
    gboolean selection = webkit_hit_test_result_context_is_selection(hit);

    GtkWidget *m = gtk_menu_new();
    if (link) {
        ctx_uri(m, "Open in New Tab", link, tab, G_CALLBACK(ctx_open_tab));
        ctx_uri(m, "Open in New Window", link, tab, G_CALLBACK(ctx_open_window));
        ctx_uri(m, "Download Link", link, tab, G_CALLBACK(ctx_download));
        ctx_uri(m, "Copy Link Address", link, tab, G_CALLBACK(ctx_copy_uri));
        ctx_sep(m);
    }
    if (image) {
        ctx_uri(m, "Open Image in New Tab", image, tab, G_CALLBACK(ctx_open_tab));
        ctx_uri(m, "Copy Image Address", image, tab, G_CALLBACK(ctx_copy_uri));
        ctx_sep(m);
    }
    if (media) {
        ctx_uri(m, "Open Media in New Tab", media, tab, G_CALLBACK(ctx_open_tab));
        ctx_uri(m, "Copy Media Address", media, tab, G_CALLBACK(ctx_copy_uri));
        ctx_sep(m);
    }
    if (editable) {
        ctx_exec_item(m, "Cut", tab, "cut");
        ctx_exec_item(m, "Copy", tab, "copy");
        ctx_exec_item(m, "Paste", tab, "paste");
        ctx_exec_item(m, "Select All", tab, "selectAll");
        ctx_sep(m);
    }
    if (selection) {
        ctx_exec_item(m, "Copy", tab, "copy");
        ctx_add(m, "Search for This", G_CALLBACK(ctx_search), tab);
        ctx_sep(m);
    }
    ctx_add(m, "View Page Source", G_CALLBACK(ctx_view_source), tab);
    ctx_add(m, "Save Page As...", G_CALLBACK(ctx_save_page), tab);
    ctx_add(m, "Reload", G_CALLBACK(ctx_reload), tab);

    gtk_widget_show_all(m);
    gtk_menu_popup_at_pointer(GTK_MENU(m), (GdkEvent *)event);
    return TRUE; /* suppress WebKit's default menu */
}

/* ---------- keyboard shortcuts ----------
 *
 * Every action has a configurable accelerator (Settings → Manage
 * shortcuts). on_key_press first handles the fixed tab jumps, then walks
 * the SC_DEFS table below; on a conflicting binding the earlier action
 * wins. Capture is done with plain GtkEntry fields: click one and press
 * the combination. */

static void sc_run_back(Win *win, Browser *b, Tab *tab)
{
    (void)b;
    if (tab && webkit_web_view_can_go_back(tab->view))
        webkit_web_view_go_back(tab->view);
}

static void sc_run_forward(Win *win, Browser *b, Tab *tab)
{
    (void)b;
    if (tab && webkit_web_view_can_go_forward(tab->view))
        webkit_web_view_go_forward(tab->view);
}

static void sc_run_reload(Win *win, Browser *b, Tab *tab)
{
    (void)win; (void)b;
    if (tab)
        webkit_web_view_reload(tab->view);
}

static void sc_run_fullscreen(Win *win, Browser *b, Tab *tab)
{
    (void)b; (void)tab;
    if (win->fullscreen)
        gtk_window_unfullscreen(GTK_WINDOW(win->window));
    else
        gtk_window_fullscreen(GTK_WINDOW(win->window));
    win->fullscreen = !win->fullscreen;
}

static void sc_run_new_tab(Win *win, Browser *b, Tab *tab)
{
    (void)b; (void)tab;
    new_tab(win, NULL);
}

static void sc_run_close_tab(Win *win, Browser *b, Tab *tab)
{
    (void)b; (void)tab;
    GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
    GtkWidget *page = gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb));
    if (page)
        close_tab(win, page);
}

static void sc_run_reopen_tab(Win *win, Browser *b, Tab *tab)
{
    (void)tab;
    if (b->closed_tabs && b->closed_tabs->len) {
        guint i = b->closed_tabs->len - 1;
        ClosedTab *ct = g_ptr_array_index(b->closed_tabs, i);
        GtkWidget *page = new_tab(win, ct->uri);
        Tab *rtab = g_object_get_data(G_OBJECT(page), "tab");
        if (rtab && ct->title)
            gtk_label_set_text(GTK_LABEL(rtab->title_label), ct->title);
        g_free(ct->uri);
        g_free(ct->title);
        g_free(ct);
        g_ptr_array_remove_index(b->closed_tabs, i);
    }
}

static void sc_run_focus_url(Win *win, Browser *b, Tab *tab)
{
    (void)b; (void)tab;
    gtk_widget_grab_focus(win->url_entry);
    gtk_editable_select_region(GTK_EDITABLE(win->url_entry), 0, -1);
}

static void sc_run_home(Win *win, Browser *b, Tab *tab)
{
    (void)b; (void)tab;
    home_goto(win);
}

static void sc_run_find(Win *win, Browser *b, Tab *tab)
{
    (void)b; (void)tab;
    find_open(win);
}

static void sc_run_bookmark(Win *win, Browser *b, Tab *tab)
{
    (void)win;
    if (tab)
        bookmark_toggle(b, webkit_web_view_get_uri(tab->view),
                        webkit_web_view_get_title(tab->view));
}

static void sc_run_source(Win *win, Browser *b, Tab *tab)
{
    (void)win; (void)b;
    if (tab)
        view_source_current(tab->view, tab);
}

static void sc_run_save_page(Win *win, Browser *b, Tab *tab)
{
    (void)win; (void)b;
    if (tab)
        save_page_current(tab->view, tab);
}

static void sc_run_zoom_in(Win *win, Browser *b, Tab *tab)
{
    (void)win; (void)b;
    if (tab)
        webkit_web_view_set_zoom_level(tab->view,
                                       webkit_web_view_get_zoom_level(tab->view) + 0.1);
}

static void sc_run_zoom_out(Win *win, Browser *b, Tab *tab)
{
    (void)win; (void)b;
    if (tab)
        webkit_web_view_set_zoom_level(tab->view,
                                       webkit_web_view_get_zoom_level(tab->view) - 0.1);
}

static void sc_run_zoom_reset(Win *win, Browser *b, Tab *tab)
{
    (void)win; (void)b;
    if (tab)
        webkit_web_view_set_zoom_level(tab->view, 1.0);
}

static void sc_run_fill_login(Win *win, Browser *b, Tab *tab)
{
    (void)win; (void)tab;
    fill_login(b);
}

static void sc_run_cycle_next(Win *win, Browser *b, Tab *tab)
{
    (void)b; (void)tab;
    cycle_tab(win, 1);
}

static void sc_run_cycle_prev(Win *win, Browser *b, Tab *tab)
{
    (void)b; (void)tab;
    cycle_tab(win, -1);
}

typedef struct {
    const char *id;      /* settings key in the [shortcuts] group */
    const char *label;   /* shown in the manager dialog */
    const char *def;     /* default accelerator */
    void (*run)(Win *win, Browser *b, Tab *tab);
} ScDef;

/* Indexed by ShortcutAction, in conflict-priority order. */
static const ScDef SC_DEFS[SC_N_ACTIONS] = {
    { "back",       "Back",               "Alt+Left",       sc_run_back },
    { "forward",    "Forward",            "Alt+Right",      sc_run_forward },
    { "reload",     "Reload / stop",      "F5",             sc_run_reload },
    { "fullscreen", "Fullscreen",         "F11",            sc_run_fullscreen },
    { "new_tab",    "New tab",            "Ctrl+T",         sc_run_new_tab },
    { "close_tab",  "Close tab",          "Ctrl+W",         sc_run_close_tab },
    { "reopen_tab", "Reopen closed tab",  "Ctrl+Shift+T",   sc_run_reopen_tab },
    { "focus_url",  "Focus URL bar",      "Ctrl+L",         sc_run_focus_url },
    { "home",       "Home",               "Ctrl+H",         sc_run_home },
    { "find",       "Find in page",       "Ctrl+F",         sc_run_find },
    { "bookmark",   "Bookmark page",      "Ctrl+D",         sc_run_bookmark },
    { "source",     "View page source",   "Ctrl+U",         sc_run_source },
    { "save_page",  "Save page as HTML",  "Ctrl+S",         sc_run_save_page },
    { "zoom_in",    "Zoom in",            "Ctrl+Plus",      sc_run_zoom_in },
    { "zoom_out",   "Zoom out",           "Ctrl+Minus",     sc_run_zoom_out },
    { "zoom_reset", "Reset zoom",         "Ctrl+0",         sc_run_zoom_reset },
    { "fill_login", "Fill login",         "Ctrl+Shift+L",   sc_run_fill_login },
    { "cycle_next", "Next tab",           "Ctrl+Tab",       sc_run_cycle_next },
    { "cycle_prev", "Previous tab",       "Ctrl+Shift+Tab", sc_run_cycle_prev },
};

/* Parse a shortcut's default into a ScKey (keyval 0 = disabled; this
 * build's gtk_accelerator_parse returns void, so a failed parse is a
 * zero keyval). */
static ScKey sc_default(int i)
{
    guint keyval = 0;
    GdkModifierType mods = 0;
    gtk_accelerator_parse(SC_DEFS[i].def, &keyval, &mods);
    return (ScKey){ keyval, mods };
}

static void sc_init(Browser *b)
{
    for (int i = 0; i < SC_N_ACTIONS; i++)
        b->sc[i] = sc_default(i);
}

static void sc_load(Browser *b, GKeyFile *kf)
{
    for (int i = 0; i < SC_N_ACTIONS; i++) {
        char *s = g_key_file_get_string(kf, "shortcuts", SC_DEFS[i].id, NULL);
        if (!s)
            continue;
        guint keyval = 0;
        GdkModifierType mods = 0;
        gtk_accelerator_parse(s, &keyval, &mods);
        if (keyval)
            b->sc[i] = (ScKey){ keyval, mods };
        g_free(s);
    }
}

static void sc_save(Browser *b, GKeyFile *kf)
{
    for (int i = 0; i < SC_N_ACTIONS; i++) {
        if (b->sc[i].keyval == 0)
            continue;
        /* get_label doubles as the persistable string ("Ctrl+T");
         * gtk_accelerator_parse reads it back */
        char *s = gtk_accelerator_get_label(b->sc[i].keyval, b->sc[i].mods);
        g_key_file_set_string(kf, "shortcuts", SC_DEFS[i].id, s);
        g_free(s);
    }
}

/* Whether a (keyval, mods) event matches a configured shortcut. Shift+Tab
 * arrives as ISO_Left_Tab on some keymaps and plus/equal are interchangeable
 * (zoom in), so both are normalized. */
static gboolean sc_match(const ScKey *sc, guint keyval, GdkModifierType mods)
{
    if (sc->keyval == 0)
        return FALSE;
    if (sc->keyval == GDK_KEY_Tab && keyval == GDK_KEY_ISO_Left_Tab)
        keyval = GDK_KEY_Tab;
    if ((sc->keyval == GDK_KEY_plus && keyval == GDK_KEY_equal) ||
        (sc->keyval == GDK_KEY_equal && keyval == GDK_KEY_plus))
        keyval = sc->keyval;
    return keyval == sc->keyval && mods == sc->mods;
}

static gboolean on_key_press(GtkWidget *w, GdkEventKey *event, Win *win)
{
    (void)w;
    Browser *b = win->browser;
    Tab *tab = current_tab(win);
    guint keyval = event->keyval;
    GdkModifierType mods = event->state & gtk_accelerator_get_default_mod_mask();

    /* Tab jumps are fixed (not configurable): Ctrl+1..9 and Alt+1..0
     * (Alt+0 -> first tab). */
    if ((keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9) &&
        (mods == GDK_MOD1_MASK || mods == GDK_CONTROL_MASK)) {
        GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
        int n = gtk_notebook_get_n_pages(nb);
        int idx = keyval - GDK_KEY_1;
        if (n > 0)
            gtk_notebook_set_current_page(nb, MIN(idx, n - 1));
        return TRUE;
    }
    if (mods == GDK_MOD1_MASK && keyval == GDK_KEY_0) {
        GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
        if (gtk_notebook_get_n_pages(nb) > 0)
            gtk_notebook_set_current_page(nb, 0);
        return TRUE;
    }

    /* Everything else is configurable (Settings → Manage shortcuts);
     * earlier table entries win on a conflict. */
    for (int i = 0; i < SC_N_ACTIONS; i++) {
        if (sc_match(&b->sc[i], keyval, mods)) {
            SC_DEFS[i].run(win, b, tab);
            return TRUE;
        }
    }
    return FALSE;
}

/* ---------- shortcut manager (Settings → Manage shortcuts) ---------- */

/* One capture row: an entry that records the pressed key combination. */
typedef struct {
    GtkWidget *entry;
    int action;      /* ShortcutAction index */
    Browser *b;
    GtkWidget *status; /* shared status/warning label */
} ScRow;

static char *sc_text(const ScKey *sc)
{
    if (sc->keyval == 0)
        return g_strdup("(disabled)");
    return gtk_accelerator_get_label(sc->keyval, sc->mods);
}

/* End the capture: drop focus from the entry so key presses reach the
 * browser shortcuts again. (While the entry is focused, its key handler
 * consumes everything, so no keyboard grab is needed.) */
static void sc_capture_stop(GtkWidget *entry)
{
    gtk_window_set_focus(GTK_WINDOW(gtk_widget_get_toplevel(entry)), NULL);
}

/* The keyval of a bare modifier key (no gdk_keyval_type in this build). */
static gboolean sc_is_mod_key(guint keyval)
{
    static const guint mods[] = {
        GDK_KEY_Control_L, GDK_KEY_Control_R,
        GDK_KEY_Shift_L, GDK_KEY_Shift_R,
        GDK_KEY_Alt_L, GDK_KEY_Alt_R,
        GDK_KEY_Meta_L, GDK_KEY_Meta_R,
        GDK_KEY_Super_L, GDK_KEY_Super_R,
        GDK_KEY_Hyper_L, GDK_KEY_Hyper_R,
        GDK_KEY_Mode_switch, GDK_KEY_ISO_Level3_Shift,
        GDK_KEY_ISO_Level5_Shift,
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(mods); i++)
        if (keyval == mods[i])
            return TRUE;
    return FALSE;
}

/* Applying a new binding: update the browser, the row's entry text, and
 * the settings file. */
static void sc_set_key(ScRow *r, guint keyval, GdkModifierType mods,
                       const char *status)
{
    r->b->sc[r->action] = (ScKey){ keyval, mods };
    char *t = sc_text(&r->b->sc[r->action]);
    gtk_entry_set_text(GTK_ENTRY(r->entry), t);
    g_free(t);
    save_settings(r->b);
    if (r->status)
        gtk_label_set_text(GTK_LABEL(r->status), status ? status : "");
}

static void on_sc_reset_clicked(GtkButton *btn, gpointer d)
{
    (void)btn;
    ScRow *r = d;
    ScKey def = sc_default(r->action);
    sc_set_key(r, def.keyval, def.mods, "Reset to default");
}

static void on_sc_reset_all_clicked(GtkButton *btn, gpointer d)
{
    (void)btn;
    GPtrArray *rows = d;
    if (!rows->len)
        return;
    Browser *b = ((ScRow *)g_ptr_array_index(rows, 0))->b;
    for (guint i = 0; i < rows->len; i++) {
        ScRow *r = g_ptr_array_index(rows, i);
        ScKey def = sc_default(r->action);
        r->b->sc[r->action] = def;
        char *t = sc_text(&def);
        gtk_entry_set_text(GTK_ENTRY(r->entry), t);
        g_free(t);
    }
    save_settings(b);
    ScRow *r0 = g_ptr_array_index(rows, 0);
    if (r0->status)
        gtk_label_set_text(GTK_LABEL(r0->status), "All shortcuts reset to defaults");
}

static gboolean sc_capture_key_press(GtkWidget *entry, GdkEventKey *event,
                                     gpointer d)
{
    ScRow *r = d;
    guint keyval = event->keyval;
    GdkModifierType mods = event->state & gtk_accelerator_get_default_mod_mask();

    if (keyval == GDK_KEY_Escape) { /* cancel */
        if (r->status)
            gtk_label_set_text(GTK_LABEL(r->status), "");
        sc_capture_stop(entry);
        return TRUE;
    }
    if (keyval == GDK_KEY_Delete) { /* restore the default binding */
        ScKey def = sc_default(r->action);
        sc_set_key(r, def.keyval, def.mods, "Reset to default");
        sc_capture_stop(entry);
        return TRUE;
    }
    if (keyval == 0)
        return TRUE;
    if (sc_is_mod_key(keyval))
        return TRUE; /* lone modifier presses are ignored */
    /* bare Enter/Backspace just end the capture */
    if ((keyval == GDK_KEY_Return || keyval == GDK_KEY_KP_Enter ||
         keyval == GDK_KEY_BackSpace) &&
        (mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK)) == 0) {
        if (r->status)
            gtk_label_set_text(GTK_LABEL(r->status), "");
        sc_capture_stop(entry);
        return TRUE;
    }

    /* normalize Shift+Tab so Ctrl+Shift+Tab can be captured as Tab */
    guint k = (keyval == GDK_KEY_ISO_Left_Tab) ? GDK_KEY_Tab : keyval;

    /* letters, Tab and other printable keys need a modifier, else they
     * would swallow ordinary typing and focus traversal */
    gboolean needs_mod = k == GDK_KEY_Tab ||
                         g_unichar_isprint(gdk_keyval_to_unicode(k));
    if (needs_mod &&
        (mods & (GDK_CONTROL_MASK | GDK_MOD1_MASK | GDK_SHIFT_MASK)) == 0) {
        if (r->status)
            gtk_label_set_text(GTK_LABEL(r->status),
                               "Use a modifier key (Ctrl, Alt or Shift) too");
        return TRUE;
    }
    /* digits with plain Ctrl/Alt are reserved for the fixed tab jumps */
    if (k >= GDK_KEY_0 && k <= GDK_KEY_9 &&
        (mods == GDK_CONTROL_MASK || mods == GDK_MOD1_MASK)) {
        if (r->status)
            gtk_label_set_text(GTK_LABEL(r->status),
                               "Ctrl/Alt + a digit is reserved for tab jumps");
        return TRUE;
    }

    /* warn about a conflict (the earlier action wins) but accept anyway */
    for (int i = 0; i < SC_N_ACTIONS; i++) {
        if (i != r->action &&
            r->b->sc[i].keyval == k && r->b->sc[i].mods == mods) {
            char *warn = g_strdup_printf("Conflicts with “%s” — the earlier "
                                         "action wins", SC_DEFS[i].label);
            if (r->status)
                gtk_label_set_text(GTK_LABEL(r->status), warn);
            g_free(warn);
            break;
        }
    }
    sc_set_key(r, k, mods, NULL);
    sc_capture_stop(entry);
    return TRUE;
}

static void on_manage_shortcuts_clicked(GtkButton *btn, Browser *b)
{
    (void)btn;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Keyboard shortcuts",
                                                    GTK_WINDOW(win_toplevel(b)),
                                                    GTK_DIALOG_MODAL,
                                                    "_Close", GTK_RESPONSE_CLOSE,
                                                    NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 560, 560);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(box), 8);
    gtk_box_set_spacing(GTK_BOX(box), 6);

    GtkWidget *hint = gtk_label_new(
        "Click a shortcut field and press a key combination. Escape cancels, "
        "Delete restores the default. Tab jumps (Ctrl+1–9, Alt+1–0) are fixed "
        "and cannot be remapped.");
    gtk_label_set_line_wrap(GTK_LABEL(hint), TRUE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

    GPtrArray *rows = g_ptr_array_new();
    GtkWidget *status = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(status), 0.0);
    gtk_widget_set_halign(status, GTK_ALIGN_START);

    for (int i = 0; i < SC_N_ACTIONS; i++) {
        GtkWidget *lbl = gtk_label_new(SC_DEFS[i].label);
        gtk_widget_set_halign(lbl, GTK_ALIGN_START);
        GtkWidget *entry = gtk_entry_new();
        gtk_entry_set_text(GTK_ENTRY(entry), sc_text(&b->sc[i]));
        gtk_widget_set_size_request(entry, 150, -1);
        ScRow *r = g_new0(ScRow, 1);
        r->entry = entry;
        r->action = i;
        r->b = b;
        r->status = status;
        g_ptr_array_add(rows, r);
        g_signal_connect(entry, "key-press-event",
                         G_CALLBACK(sc_capture_key_press), r);
        GtkWidget *reset = gtk_button_new_with_label("Reset");
        gtk_widget_set_tooltip_text(reset, "Restore the default shortcut");
        g_signal_connect(reset, "clicked", G_CALLBACK(on_sc_reset_clicked), r);
        gtk_grid_attach(GTK_GRID(grid), lbl, 0, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), entry, 1, i, 1, 1);
        gtk_grid_attach(GTK_GRID(grid), reset, 2, i, 1, 1);
    }

    GtkWidget *sw = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(sw), grid);
    gtk_box_pack_start(GTK_BOX(box), sw, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), status, FALSE, FALSE, 0);

    GtkWidget *reset_all = gtk_button_new_with_label("Reset all to defaults");
    gtk_widget_set_halign(reset_all, GTK_ALIGN_START);
    g_signal_connect(reset_all, "clicked", G_CALLBACK(on_sc_reset_all_clicked), rows);
    gtk_box_pack_start(GTK_BOX(box), reset_all, FALSE, FALSE, 0);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    g_ptr_array_free(rows, TRUE);
    gtk_widget_destroy(dialog);
}

/* ---------- control socket (single-instance + agent CLI) ---------- */

static gchar *control_socket_path(void)
{
    const char *xdg = g_getenv("XDG_RUNTIME_DIR");
    if (xdg && *xdg)
        return g_build_filename(xdg, "gtkbrowser.sock", NULL);
    return g_strdup_printf("/tmp/gtkbrowser-%d.sock", (int)getuid());
}

/* Send a command to a running instance. Returns TRUE if a running instance
 * handled it (caller should exit). On success, *response receives the reply. */
static gboolean send_command(const char *path, const char *cmd, char **response)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return FALSE;

    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    g_strlcpy(sun.sun_path, path, sizeof(sun.sun_path));

    if (connect(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
        close(fd);
        return FALSE;
    }

    char *line = g_strdup_printf("%s\n", cmd);
    ssize_t w = write(fd, line, strlen(line));
    g_free(line);
    if (w < 0) {
        close(fd);
        return FALSE;
    }

    char buf[8192];
    size_t total = 0;
    for (;;) {
        if (total >= sizeof(buf) - 1)
            break;
        ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
        if (n <= 0)
            break;
        total += (size_t)n;
    }
    close(fd);
    if (total == 0)
        return FALSE;
    buf[total] = '\0';
    if (response)
        *response = g_strdup(buf);
    return TRUE;
}

/* Execute a control command on the running instance. Returns a heap-allocated
 * response string (caller frees). */
static char *handle_control_command(Browser *b, const char *cmd)
{
    Win *win = active_window(b);
    if (g_str_has_prefix(cmd, "open ")) {
        const char *url = cmd + 5;
        if (*url && win) {
            char *uri = normalize_uri(url);
            new_tab(win, uri);
            g_free(uri);
        }
        return g_strdup("ok");
    }
    if (strcmp(cmd, "list") == 0) {
        GtkNotebook *nb = win ? GTK_NOTEBOOK(win->notebook) : NULL;
        int n = nb ? gtk_notebook_get_n_pages(nb) : 0;
        GString *s = g_string_new(NULL);
        for (int i = 0; i < n; i++) {
            GtkWidget *page = gtk_notebook_get_nth_page(nb, i);
            Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
            const char *uri = tab ? webkit_web_view_get_uri(tab->view) : NULL;
            const char *title = tab ? webkit_web_view_get_title(tab->view) : NULL;
            g_string_append_printf(s, "%d\t%s\t%s\n", i,
                                   uri ? uri : "", title ? title : "");
        }
        return g_string_free(s, FALSE);
    }
    if (strcmp(cmd, "url") == 0) {
        Tab *tab = win ? current_tab(win) : NULL;
        const char *uri = tab ? webkit_web_view_get_uri(tab->view) : NULL;
        return g_strdup(uri ? uri : "");
    }
    if (strcmp(cmd, "title") == 0) {
        Tab *tab = win ? current_tab(win) : NULL;
        const char *title = tab ? webkit_web_view_get_title(tab->view) : NULL;
        return g_strdup(title ? title : "");
    }
    if (strcmp(cmd, "blocked") == 0) {
        guint64 count = b->proxy ? adb_proxy_blocked_count(b->proxy) : 0;
        return g_strdup_printf("%llu", (unsigned long long)count);
    }
    if (g_str_has_prefix(cmd, "focus ")) {
        int n = atoi(cmd + 6);
        if (win) {
            GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
            if (n >= 0 && n < gtk_notebook_get_n_pages(nb))
                gtk_notebook_set_current_page(nb, n);
        }
        return g_strdup("ok");
    }
    if (strcmp(cmd, "close") == 0 || g_str_has_prefix(cmd, "close ")) {
        if (win) {
            GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
            int n = (cmd[5] == '\0') ? gtk_notebook_get_current_page(nb) : atoi(cmd + 6);
            if (n >= 0 && n < gtk_notebook_get_n_pages(nb)) {
                GtkWidget *page = gtk_notebook_get_nth_page(nb, n);
                close_tab(win, page);
            }
        }
        return g_strdup("ok");
    }
    if (strcmp(cmd, "quit") == 0) {
        gtk_main_quit();
        return g_strdup("ok");
    }
    return g_strdup("error: unknown command");
}

static gboolean on_control_connected(GSocketService *service, GSocketConnection *conn,
                                     GObject *source, gpointer user_data)
{
    Browser *b = user_data;
    /* The socket is owned by the connection (and thus the service); we only
     * borrow it for the duration of this handler, so no ref/unref needed. */
    GSocket *sock = g_socket_connection_get_socket(conn);
    g_socket_set_blocking(sock, TRUE);
    g_socket_set_timeout(sock, 2);
    int fd = g_socket_get_fd(sock);

    char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        char *eol = strchr(buf, '\n');
        if (eol)
            *eol = '\0';

        char *response = handle_control_command(b, buf);
        if (response) {
            char *line = g_strdup_printf("%s\n", response);
            write(fd, line, strlen(line));
            g_free(line);
            g_free(response);
        }
    }
    return TRUE;
}

/* ---------- window / cleanup ---------- */

/* The focused window, or the most recent one if none is tracked yet. */
static Win *active_window(Browser *b)
{
    if (b->active)
        return b->active;
    return (b->windows && b->windows->len)
        ? g_ptr_array_index(b->windows, b->windows->len - 1) : NULL;
}

/* The active window's toplevel, or NULL if none: the parent for modal
 * dialogs. */
static GtkWidget *win_toplevel(Browser *b)
{
    Win *w = active_window(b);
    return w ? w->window : NULL;
}

static void on_window_focus_in(GtkWidget *w, GdkEvent *e, Win *win)
{
    (void)w; (void)e;
    win->browser->active = win;
    /* When the toplevel regains X focus, GTK re-focuses its focus widget,
     * but for the NO_WINDOW WebKit view that re-focus doesn't always reach
     * WebCore, so the text caret can stop blinking. Leave the focus alone
     * when a visible non-view widget (URL bar, find bar, start page search
     * box) holds it; otherwise force a focus round-trip to the widget this
     * tab should have focused (the view, or the search box when the start
     * page is shown) so it gets a fresh focus-in. */
    Tab *tab = current_tab(win);
    if (!tab)
        return;
    GtkWidget *fw = gtk_window_get_focus(GTK_WINDOW(win->window));
    if (fw && fw != GTK_WIDGET(tab->view) && gtk_widget_is_visible(fw))
        return;
    GtkWidget *want = tab->on_start_page
        ? find_named(tab->start_page, "start-search")
        : GTK_WIDGET(tab->view);
    if (!want)
        return;
    gtk_window_set_focus(GTK_WINDOW(win->window), NULL);
    gtk_window_set_focus(GTK_WINDOW(win->window), want);
}

static void close_window(Win *win)
{
    /* destroying the toplevel fires "destroy" -> on_window_destroy, which
     * detaches the window from the app and quits when it is the last one */
    gtk_widget_destroy(win->window);
}

/* A window's toplevel went away: drop it from the app, and shut the whole
 * browser down if it was the last one. */
static void on_window_destroy(GtkWidget *w, Win *win)
{
    Browser *b = win->browser;
    (void)w;
    if (b->active == win)
        b->active = NULL;
    g_ptr_array_remove(b->windows, win);
    g_free(win);
    if (b->windows->len > 0) {
        b->active = g_ptr_array_index(b->windows, b->windows->len - 1);
        return;
    }
    /* last window closed: tear down the whole app */
    if (b->control_service) {
        g_socket_service_stop(b->control_service);
        g_object_unref(b->control_service);
        b->control_service = NULL;
    }
    if (b->control_socket_path) {
        unlink(b->control_socket_path);
        g_free(b->control_socket_path);
        b->control_socket_path = NULL;
    }
    if (b->proxy) {
        adb_proxy_free(b->proxy);
        b->proxy = NULL;
    }
    secure_free(&b->kp_password);
    g_free(b->kp_db_path);
    g_free(b->custom_search_base);
    g_free(b->home_url);
    g_free(b->home_file);
    g_free(b->new_tab_url);
    g_free(b->user_agent);
    g_free(b->blocklist_file);
    history_save(b);
    bookmarks_save(b);
    session_save(b);
    if (b->history) {
        g_ptr_array_set_free_func(b->history, g_free);
        g_ptr_array_unref(b->history);
        b->history = NULL;
    }
    if (b->bookmarks) {
        for (guint i = 0; i < b->bookmarks->len; i++) {
            Bookmark *bm = g_ptr_array_index(b->bookmarks, i);
            g_free(bm->uri);
            g_free(bm->title);
            g_free(bm);
        }
        g_ptr_array_unref(b->bookmarks);
        b->bookmarks = NULL;
    }
    if (b->closed_tabs) {
        for (guint i = 0; i < b->closed_tabs->len; i++) {
            ClosedTab *ct = g_ptr_array_index(b->closed_tabs, i);
            g_free(ct->uri);
            g_free(ct->title);
            g_free(ct);
        }
        g_ptr_array_unref(b->closed_tabs);
        b->closed_tabs = NULL;
    }
    if (b->downloads_window) {
        gtk_widget_destroy(b->downloads_window); /* handler clears the field */
        b->downloads_window = NULL;
    }
    if (b->downloads_model) {
        g_object_unref(b->downloads_model);
        b->downloads_model = NULL;
    }
    if (b->downloads) {
        for (guint i = 0; i < b->downloads->len; i++) {
            DlItem *it = g_ptr_array_index(b->downloads, i);
            g_clear_object(&it->dl);
            g_free(it->name);
            g_free(it->dest_path);
            g_free(it->dest_uri);
            g_free(it);
        }
        g_ptr_array_unref(b->downloads);
        b->downloads = NULL;
    }
    g_free(b->download_dir);
    if (b->permissions) {
        g_hash_table_unref(b->permissions);
        b->permissions = NULL;
    }
    g_free(b);
    gtk_main_quit();
}

/* Build a fresh browser window (toplevel + toolbar + notebook + find bar).
 * Tabs are added afterwards via new_tab(). */
static Win *new_window(Browser *b)
{
    Win *win = g_new0(Win, 1);
    win->browser = b;

    win->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win->window), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(win->window), 1100, 700);
    /* drops on the toolbar / tab strip open the link or file in a new
     * tab; drops on the web view are handled by the per-view fallback */
    gtk_drag_dest_set(win->window, GTK_DEST_DEFAULT_ALL,
                      WINDOW_DROP_TARGETS, G_N_ELEMENTS(WINDOW_DROP_TARGETS),
                      GDK_ACTION_COPY);
    g_signal_connect(win->window, "drag-data-received",
                     G_CALLBACK(on_window_drag_data_received), win);
    g_signal_connect(win->window, "destroy", G_CALLBACK(on_window_destroy), win);
    g_signal_connect(win->window, "focus-in-event", G_CALLBACK(on_window_focus_in), win);
    g_signal_connect(win->window, "key-press-event", G_CALLBACK(on_key_press), win);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(win->window), vbox);

    /* toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "toolbar");
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);

    win->back_button = gtk_button_new_from_icon_name("go-previous", GTK_ICON_SIZE_LARGE_TOOLBAR);
    win->forward_button = gtk_button_new_from_icon_name("go-next", GTK_ICON_SIZE_LARGE_TOOLBAR);
    win->reload_button = gtk_button_new_from_icon_name("view-refresh", GTK_ICON_SIZE_LARGE_TOOLBAR);
    win->home_button = gtk_button_new_from_icon_name("go-home", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_sensitive(win->back_button, FALSE);
    gtk_widget_set_sensitive(win->forward_button, FALSE);
    gtk_widget_set_tooltip_text(win->back_button, "Back");
    gtk_widget_set_tooltip_text(win->forward_button, "Forward");
    gtk_widget_set_tooltip_text(win->reload_button, "Reload / Stop (F5)");
    gtk_widget_set_tooltip_text(win->home_button, "Home (Ctrl+H)");

    win->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->url_entry), "Search or enter address");
    gtk_widget_set_hexpand(win->url_entry, TRUE);
    gtk_widget_set_tooltip_text(win->url_entry, "Ctrl+L to focus");
    /* GTK3 gives every entry a ~150px built-in minimum width (MIN_ENTRY_WIDTH
     * in gtkentry.c; ~168px with padding/border in most themes, and CSS
     * min-width cannot lower it). That floor, added to the toolbar buttons,
     * pinned the window's minimum width to the full toolbar, so the window
     * could not be resized much narrower. width-chars 0 lets the entry shrink
     * down to just its padlock icon; it still expands to fill the toolbar. */
    gtk_entry_set_width_chars(GTK_ENTRY(win->url_entry), 0);
    GtkEntryCompletion *completion = gtk_entry_completion_new();
    gtk_entry_completion_set_model(completion, GTK_TREE_MODEL(b->url_model));
    gtk_entry_completion_set_text_column(completion, 0);
    gtk_entry_completion_set_minimum_key_length(completion, 2);
    gtk_entry_set_completion(GTK_ENTRY(win->url_entry), completion);

    GtkWidget *settings_button = gtk_button_new_from_icon_name("preferences-system",
                                                               GTK_ICON_SIZE_LARGE_TOOLBAR);
    GtkWidget *new_tab_button = gtk_button_new_from_icon_name("list-add", GTK_ICON_SIZE_LARGE_TOOLBAR);
    GtkWidget *close_tab_button = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_tooltip_text(settings_button, "Settings");
    gtk_widget_set_tooltip_text(new_tab_button, "New tab (Ctrl+T)");
    gtk_widget_set_tooltip_text(close_tab_button, "Close tab (Ctrl+W)");

    win->blocked_button = gtk_button_new();
    {
        GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *bimg = gtk_image_new_from_icon_name("security-high",
                                                       GTK_ICON_SIZE_SMALL_TOOLBAR);
        win->blocked_label = gtk_label_new("0");
        gtk_box_pack_start(GTK_BOX(bb), bimg, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bb), win->blocked_label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(win->blocked_button), bb);
    }
    gtk_widget_set_tooltip_text(win->blocked_button,
                                "Trackers blocked — click for details");
    g_signal_connect(win->blocked_button, "clicked",
                     G_CALLBACK(on_blocked_clicked), b);

    gtk_box_pack_start(GTK_BOX(toolbar), win->back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), win->forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), win->reload_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), win->home_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), win->url_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), win->blocked_button, FALSE, FALSE, 0);

    win->kp_fill_button = gtk_button_new_from_icon_name("dialog-password",
                                                      GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_tooltip_text(win->kp_fill_button,
                                "Fill login form from KeePass (Ctrl+Shift+L)");
    g_signal_connect(win->kp_fill_button, "clicked",
                     G_CALLBACK(on_fill_button_clicked), b);
    gtk_box_pack_start(GTK_BOX(toolbar), win->kp_fill_button, FALSE, FALSE, 0);

    win->bookmarks_button = gtk_button_new_from_icon_name("starred",
                                                       GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_tooltip_text(win->bookmarks_button,
                                "Bookmarks — Ctrl+D toggles the current page");
    g_signal_connect(win->bookmarks_button, "clicked",
                     G_CALLBACK(on_bookmarks_clicked), b);
    gtk_box_pack_start(GTK_BOX(toolbar), win->bookmarks_button, FALSE, FALSE, 0);

    win->downloads_button = gtk_button_new_from_icon_name("browser-download",
                                                          GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_tooltip_text(win->downloads_button,
                                "Downloads — recent files and their progress");
    g_signal_connect(win->downloads_button, "clicked",
                     G_CALLBACK(on_downloads_clicked), b);
    gtk_box_pack_start(GTK_BOX(toolbar), win->downloads_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar), settings_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), new_tab_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), close_tab_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    /* thin progress bar between the toolbar and the tabs */
    win->progress = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(win->progress), "thin");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(win->progress), 0);
    gtk_box_pack_start(GTK_BOX(vbox), win->progress, FALSE, FALSE, 0);

    win->notebook = gtk_notebook_new();
    gtk_notebook_set_show_border(GTK_NOTEBOOK(win->notebook), FALSE);
    /* Scrollable so many tabs use arrows instead of forcing the window's
     * minimum size to grow with each new tab. */
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(win->notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), win->notebook, TRUE, TRUE, 0);

    /* find-in-page bar (Ctrl+F), hidden until requested */
    win->find_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_container_set_border_width(GTK_CONTAINER(win->find_bar), 4);
    gtk_box_pack_start(GTK_BOX(win->find_bar), gtk_label_new("Find:"), FALSE, FALSE, 0);
    win->find_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->find_entry), "Find in page");
    gtk_widget_set_tooltip_text(win->find_entry,
                                "Enter next · Shift+Enter previous · Esc close");
    gtk_box_pack_start(GTK_BOX(win->find_bar), win->find_entry, TRUE, TRUE, 0);
    win->find_label = gtk_label_new("");
    gtk_box_pack_start(GTK_BOX(win->find_bar), win->find_label, FALSE, FALSE, 0);
    GtkWidget *find_prev_btn = gtk_button_new_from_icon_name("go-up", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(find_prev_btn, "Previous match");
    GtkWidget *find_next_btn = gtk_button_new_from_icon_name("go-down", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(find_next_btn, "Next match");
    GtkWidget *find_close_btn = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_BUTTON);
    gtk_widget_set_tooltip_text(find_close_btn, "Close (Esc)");
    g_signal_connect(find_prev_btn, "clicked", G_CALLBACK(on_find_prev_clicked), win);
    g_signal_connect(find_next_btn, "clicked", G_CALLBACK(on_find_next_clicked), win);
    g_signal_connect(find_close_btn, "clicked", G_CALLBACK(on_find_close_clicked), win);
    g_signal_connect(win->find_entry, "changed", G_CALLBACK(on_find_changed), win);
    g_signal_connect(win->find_entry, "key-press-event", G_CALLBACK(on_find_key_press), win);
    gtk_box_pack_start(GTK_BOX(win->find_bar), find_prev_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(win->find_bar), find_next_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(win->find_bar), find_close_btn, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(vbox), win->find_bar, FALSE, FALSE, 0);

    /* 0x0 stand-in tab label that hides the whole tab strip (single tab);
     * an empty box has 0 natural size, so the tab area collapses to 0 */
    win->empty_tab_label = g_object_ref_sink(gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0));

    g_signal_connect(win->back_button, "clicked", G_CALLBACK(on_back_clicked), win);
    g_signal_connect(win->forward_button, "clicked", G_CALLBACK(on_forward_clicked), win);
    g_signal_connect(win->reload_button, "clicked", G_CALLBACK(on_reload_clicked), win);
    g_signal_connect_swapped(win->home_button, "clicked",
                             G_CALLBACK(home_goto), win);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(on_settings_clicked), b);
    g_signal_connect(new_tab_button, "clicked", G_CALLBACK(on_new_tab_clicked), win);
    g_signal_connect(close_tab_button, "clicked", G_CALLBACK(on_close_tab_clicked), win);
    g_signal_connect(win->url_entry, "activate", G_CALLBACK(on_entry_activate), win);
    g_signal_connect(win->notebook, "switch-page", G_CALLBACK(on_switch_page), win);

    g_ptr_array_add(b->windows, win);
    b->active = win;
    gtk_widget_show_all(win->window);
    gtk_widget_hide(win->find_bar);
    update_tabbar(win);
    return win;
}

/* ---------- CSS ---------- */

static void apply_css(void)
{
    GtkCssProvider *provider = gtk_css_provider_new();
    const char *css =
        "progressbar.thin {"
        "  min-height: 2px;"
        "  margin: 0;"
        "  padding: 0;"
        "}"
        "progressbar.thin trough {"
        "  min-height: 2px;"
        "  margin: 0;"
        "  padding: 0;"
        "  border: none;"
        "  box-shadow: none;"
        "  background-color: alpha(@theme_fg_color, 0.08);"
        "}"
        "progressbar.thin progress {"
        "  min-height: 2px;"
        "  margin: 0;"
        "  padding: 0;"
        "  border: none;"
        "  box-shadow: none;"
        "  background-image: none;"
        "  background-color: @theme_selected_bg_color;"
        "}"
        "notebook.single-tab header,"
        "notebook.single-tab tabs,"
        "notebook.single-tab tab {"
        "  min-height: 0;"
        "  min-width: 0;"
        "  padding: 0;"
        "  margin: 0;"
        "  border-width: 0;"
        "  border: none;"
        "  box-shadow: none;"
        "  background: none;"
        "}"
        "notebook.single-tab header tab > * {"
        "  min-height: 0;"
        "  min-width: 0;"
        "}"
        "box.start-page {"
        "  padding: 16px;"
        "}"
        "button.start-tile {"
        "  padding: 10px 12px;"
        "  border-radius: 8px;"
        "}";
    gtk_css_provider_load_from_data(provider, css, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    /* ---- parse arguments (no GTK needed yet) ---- */
    const char *initial = NULL;
    gboolean no_sandbox = FALSE;
    const char *blocklist = NULL;
    char *cli_cmd = NULL;

    for (int i = 1; i < argc; i++) {
        if (g_strcmp0(argv[i], "--no-sandbox") == 0) {
            no_sandbox = TRUE;
        } else if (g_strcmp0(argv[i], "--blocklist") == 0 && i + 1 < argc) {
            blocklist = argv[++i];
        } else if (g_strcmp0(argv[i], "--list") == 0) {
            cli_cmd = g_strdup("list");
        } else if (g_strcmp0(argv[i], "--url") == 0) {
            cli_cmd = g_strdup("url");
        } else if (g_strcmp0(argv[i], "--title") == 0) {
            cli_cmd = g_strdup("title");
        } else if (g_strcmp0(argv[i], "--blocked") == 0) {
            cli_cmd = g_strdup("blocked");
        } else if (g_strcmp0(argv[i], "--focus") == 0 && i + 1 < argc) {
            cli_cmd = g_strdup_printf("focus %s", argv[++i]);
        } else if (g_strcmp0(argv[i], "--close") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-')
                cli_cmd = g_strdup_printf("close %s", argv[++i]);
            else
                cli_cmd = g_strdup("close");
        } else if (g_strcmp0(argv[i], "--quit") == 0) {
            cli_cmd = g_strdup("quit");
        } else if (initial == NULL) {
            initial = argv[i];
        }
    }

    /* ---- if a command or URL was given, try the running instance ---- */
    if (cli_cmd || initial) {
        gchar *path = control_socket_path();
        char *cmd = cli_cmd ? g_strdup(cli_cmd) : g_strdup_printf("open %s", initial);
        char *response = NULL;
        if (send_command(path, cmd, &response)) {
            if (response) {
                g_print("%s\n", response);
                g_free(response);
            }
            g_free(cmd);
            g_free(path);
            g_free(cli_cmd);
            return 0;
        }
        g_free(cmd);
        g_free(path);
    }
    g_free(cli_cmd);

    /* ---- no running instance: start the browser ---- */
    /* Disable WebKit's DMABUF GPU renderer: on some systems (e.g. multi-GPU
     * NVIDIA, VMs, containers) it picks a GPU it lacks DRM permission for and
     * fails with "DRM_IOCTL_MODE_CREATE_DUMB: Permission denied", leaving the
     * web view unrendered (blank/white page, no blinking text caret).
     * Software rendering is the reliable fallback; remove this call and
     * rebuild to try GPU rendering. See WebKit bug 259644/261874. */
    g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", TRUE);
    gtk_init(&argc, &argv);

    atom_text_uri_list  = gdk_atom_intern("text/uri-list", FALSE);
    atom_text_plain     = gdk_atom_intern("text/plain", FALSE);
    atom_text_utf8      = gdk_atom_intern("UTF8_STRING", FALSE);
    atom_text_string    = gdk_atom_intern("STRING", FALSE);
    atom_text_text      = gdk_atom_intern("TEXT", FALSE);
    atom_text_html      = gdk_atom_intern("text/html", FALSE);
    atom_netscape_url   = gdk_atom_intern("_NETSCAPE_URL", FALSE);

    if (no_sandbox)
        webkit_web_context_set_sandbox_enabled(webkit_web_context_get_default(), FALSE);

    apply_css();

    Browser *b = g_new0(Browser, 1);
    b->blocklist_file = blocklist ? g_strdup(blocklist) : NULL;
    b->adblock_enabled = TRUE;
    b->devtools_enabled = TRUE;
    b->default_zoom = 100;
    b->restore_session = TRUE;
    b->hide_tabbar = TRUE;
    sc_init(b); /* before load_settings: sc_load() only overrides saved keys */
    perm_load(b);
    load_settings(b);
    apply_cookie_policy(b);
    load_history(b);
    load_bookmarks(b);

    b->windows = g_ptr_array_new();
    b->url_model = gtk_list_store_new(1, G_TYPE_STRING);
    refresh_url_model(b);
    b->downloads = g_ptr_array_new();
    b->downloads_model = gtk_list_store_new(4, G_TYPE_POINTER,
                                            G_TYPE_STRING, G_TYPE_STRING,
                                            G_TYPE_STRING);
    g_timeout_add_seconds(1, on_dl_stall_check, b);

    Win *w0 = new_window(b);

    g_signal_connect(webkit_web_context_get_default(), "download-started",
                     G_CALLBACK(on_download_started), b);

    /* adblock proxy (on by default) */
    set_adblock_enabled(b, TRUE);

    /* control socket for the single-instance / agent CLI */
    b->control_socket_path = control_socket_path();
    unlink(b->control_socket_path); /* remove a stale socket from a crashed run */
    b->control_service = g_socket_service_new();
    GSocketAddress *caddr = g_unix_socket_address_new(b->control_socket_path);
    if (caddr &&
        g_socket_listener_add_address(G_SOCKET_LISTENER(b->control_service), caddr,
                                      G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_DEFAULT,
                                      NULL, NULL, NULL)) {
        g_signal_connect(b->control_service, "incoming",
                         G_CALLBACK(on_control_connected), b);
        g_socket_service_start(b->control_service);
    } else {
        g_object_unref(b->control_service);
        b->control_service = NULL;
    }
    g_clear_object(&caddr);

    /* first tab: URL from the command line, or the last session, or home */
    if (initial || !b->restore_session || !session_restore(b, w0))
        new_tab(w0, initial);

    update_blocked_label(b);
    g_timeout_add(1000, update_blocked_label, b);

    gtk_main();
    return 0;
}
