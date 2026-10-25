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
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <jsc/jsc.h>
#include "adblock.h"
#include "keepass.h"
#include <string.h>
#include <stdlib.h>
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
    gchar *new_tab_url;      /* page for new tabs (settings) */
    int default_zoom;        /* percent, applied to new tabs */
    gchar *user_agent;       /* custom user agent (settings) */
    GPtrArray *closed_tabs;  /* most-recent-closed first (Ctrl+Shift+T) */
    gboolean restore_session; /* reopen last session's tabs at start */
    gboolean hide_tabbar;    /* hide the tab strip in single-tab windows */
};

/* Per-window state: the toplevel and everything that lives inside it. */
struct Win {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *url_entry;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *reload_button;
    GtkWidget *progress;
    GtkWidget *blocked_button;
    GtkWidget *blocked_label;
    GtkWidget *kp_fill_button;
    GtkWidget *bookmarks_button;
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
    gchar *source_file;      /* temp file behind a Ctrl+U view-source load */
    /* pending drag & drop state; see the "drag & drop" section */
    GdkDragContext *drop_ctx;     /* drag currently over the view (ref) */
    gboolean drop_shell_accepted; /* shell took the drop (page refused) */
    GPtrArray *drop_uris;         /* char* URIs from text/uri-list */
    gchar *drop_text;             /* text from the text targets */
    guint drop_timer;             /* fallback-delay source id, 0 = none */
    guint drop_leave_timer;       /* leave-vs-drop resolver id, 0 = none */
    guint drop_status_timer;      /* COPY re-assert id, 0 = none */
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
static void history_add(Browser *b, const char *uri);
static void refresh_url_model(Browser *b);
static void bookmark_toggle(Browser *b, const char *uri, const char *title);
static void cycle_tab(Win *win, int delta);
static void on_engine_changed(GtkComboBox *combo, Browser *b);
static void on_custom_search_changed(GtkEntry *entry, Browser *b);
static void on_home_changed(GtkEntry *entry, Browser *b);
static void on_new_tab_url_changed(GtkEntry *entry, Browser *b);
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
    g_clear_object(&tab->tab_label);
    g_free(tab);
}

/* ---------- web view signal handlers ---------- */

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, Tab *tab)
{
    Win *win = tab->win;
    Browser *b = win->browser;
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

static void on_download_started(WebKitWebContext *context, WebKitDownload *download, Browser *b)
{
    WebKitURIRequest *request = webkit_download_get_request(download);
    const char *uri = request ? webkit_uri_request_get_uri(request) : NULL;
    const char *suggested = uri ? g_path_get_basename(uri) : "download";

    Win *aw = active_window(b);
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Save file",
                                                    aw ? GTK_WINDOW(aw->window) : NULL,
                                                    GTK_FILE_CHOOSER_ACTION_SAVE,
                                                    "_Cancel", GTK_RESPONSE_CANCEL,
                                                    "_Save", GTK_RESPONSE_ACCEPT,
                                                    NULL);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggested);

    int response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        char *save_uri = gtk_file_chooser_get_uri(GTK_FILE_CHOOSER(dialog));
        if (save_uri) {
            webkit_download_set_destination(download, save_uri);
            g_free(save_uri);
        } else {
            webkit_download_cancel(download);
        }
    } else {
        webkit_download_cancel(download);
    }
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

    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), tab);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_notify_title), tab);
    g_signal_connect(view, "notify::uri", G_CALLBACK(on_notify_uri), tab);
    g_signal_connect(view, "notify::favicon", G_CALLBACK(on_notify_favicon), tab);
    g_signal_connect(view, "notify::loading", G_CALLBACK(on_notify_loading), tab);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_notify_progress), tab);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), tab);
    g_signal_connect(view, "context-menu", G_CALLBACK(on_view_context_menu), tab);
    /* shell-level drop fallback; runs before WebKit's own handlers, so
     * a drop the web page refused can be opened here as new tabs */
    g_signal_connect(view, "drag-motion", G_CALLBACK(on_view_drag_motion), tab);
    g_signal_connect(view, "drag-leave", G_CALLBACK(on_view_drag_leave), tab);
    g_signal_connect(view, "drag-data-received", G_CALLBACK(on_view_drag_data_received), tab);
    g_signal_connect(view, "drag-drop", G_CALLBACK(on_view_drag_drop), tab);
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), page);
    g_signal_connect(label_box, "button-press-event", G_CALLBACK(on_tab_button_press), page);

    const char *start_url = uri && *uri ? uri
                                        : (b->new_tab_url && *b->new_tab_url
                                           ? b->new_tab_url : HOME_URL);
    char *load_uri_str = normalize_uri(start_url);
    webkit_web_view_load_uri(view, load_uri_str);
    g_free(load_uri_str);

    gtk_notebook_set_current_page(GTK_NOTEBOOK(win->notebook),
                                  gtk_notebook_page_num(GTK_NOTEBOOK(win->notebook), page));
    update_tabbar(win);
    update_url_entry(win, tab);
    update_nav_buttons(win, tab);
    update_window_title(win, tab);
    /* give the new tab's view the input focus so its text caret is drawn;
     * a WebKit view that never receives a focus-in doesn't blink its caret */
    gtk_widget_grab_focus(GTK_WIDGET(view));
    return page;
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
    /* give the page the input focus so its text caret shows; keep the URL bar
     * focused when it already is (e.g. cycling tabs while typing a URL) */
    if (tab && !gtk_widget_has_focus(win->url_entry))
        gtk_widget_grab_focus(GTK_WIDGET(tab->view));
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
}

static void on_new_tab_url_changed(GtkEntry *entry, Browser *b)
{
    const char *t = gtk_entry_get_text(GTK_ENTRY(entry));
    g_free(b->new_tab_url);
    b->new_tab_url = (t && *t) ? g_strdup(t) : NULL;
    save_settings(b);
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

    setting_entry_row(content, "Home page:", b->home_url, HOME_URL,
                      G_CALLBACK(on_home_changed), b);
    setting_entry_row(content, "New tab page:", b->new_tab_url, HOME_URL,
                      G_CALLBACK(on_new_tab_url_changed), b);

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

    kp_build_settings_section(b, content);

    g_signal_connect(adblock_check, "toggled", G_CALLBACK(on_adblock_toggled), b);
    g_signal_connect(devtools_check, "toggled", G_CALLBACK(on_devtools_toggled), b);
    g_signal_connect(engine_combo, "changed", G_CALLBACK(on_engine_changed), b);
    g_signal_connect(custom_entry, "changed", G_CALLBACK(on_custom_search_changed), b);
    g_signal_connect(zoom_spin, "value-changed", G_CALLBACK(on_zoom_changed), b);
    g_signal_connect(restore_check, "toggled", G_CALLBACK(on_restore_toggled), b);
    g_signal_connect(hide_tabbar_check, "toggled",
                     G_CALLBACK(on_hide_tabbar_toggled), b);

    gtk_widget_show_all(dialog);
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
        save_settings(b);
    gtk_widget_destroy(dialog);
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
        b->home_url = g_key_file_get_string(kf, "general", "home", NULL);
        b->new_tab_url = g_key_file_get_string(kf, "general", "newtab", NULL);
        b->user_agent = g_key_file_get_string(kf, "general", "user_agent", NULL);
        if (!b->blocklist_file)
            b->blocklist_file = g_key_file_get_string(kf, "adblock", "blocklist", NULL);
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
        gtk_label_set_selectable(GTK_LABEL(lbl), TRUE);
        g_free(text);
        gtk_widget_set_hexpand(lbl, TRUE);
        GtkWidget *remove = gtk_button_new_with_mnemonic("_Remove");
        gtk_box_pack_start(GTK_BOX(row), lbl, TRUE, TRUE, 0);
        gtk_box_pack_start(GTK_BOX(row), remove, FALSE, FALSE, 0);
        g_object_set_data_full(G_OBJECT(row), "uri", g_strdup(bm->uri),
                               g_free);
        g_signal_connect(remove, "clicked",
                         G_CALLBACK(on_bookmark_remove_clicked), list);
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

static void on_bookmark_row_activated(GtkListBox *list, GtkListBoxRow *row,
                                      Browser *b)
{
    (void)list;
    GtkWidget *inner = gtk_bin_get_child(GTK_BIN(row));
    char *uri = inner ? g_object_get_data(G_OBJECT(inner), "uri") : NULL;
    if (uri) {
        Win *w = active_window(b);
        if (w)
            new_tab(w, uri);
    }
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
        "Double-click a bookmark to open it in a new tab. "
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

/* ---------- keyboard shortcuts ---------- */

static gboolean on_key_press(GtkWidget *w, GdkEventKey *event, Win *win)
{
    (void)w;
    Browser *b = win->browser;
    Tab *tab = current_tab(win);
    guint keyval = event->keyval;
    GdkModifierType mods = event->state & gtk_accelerator_get_default_mod_mask();

    if (keyval == GDK_KEY_F5) {
        if (tab)
            webkit_web_view_reload(tab->view);
        return TRUE;
    }
    if (keyval == GDK_KEY_F11) {
        if (win->fullscreen)
            gtk_window_unfullscreen(GTK_WINDOW(win->window));
        else
            gtk_window_fullscreen(GTK_WINDOW(win->window));
        win->fullscreen = !win->fullscreen;
        return TRUE;
    }
    if ((mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) &&
        (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
        fill_login(b);
        return TRUE;
    }

    /* Ctrl+Shift+T reopens the most recently closed tab. */
    if (mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK) && keyval == GDK_KEY_t) {
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
        return TRUE;
    }
    /* Alt+Left / Alt+Right navigate back / forward. */
    if (mods == GDK_MOD1_MASK && tab) {
        if (keyval == GDK_KEY_Left && webkit_web_view_can_go_back(tab->view)) {
            webkit_web_view_go_back(tab->view);
            return TRUE;
        }
        if (keyval == GDK_KEY_Right && webkit_web_view_can_go_forward(tab->view)) {
            webkit_web_view_go_forward(tab->view);
            return TRUE;
        }
        /* Alt+1..0 jump to a tab (Alt+0 -> first). */
        if (keyval == GDK_KEY_0 ||
            (keyval >= GDK_KEY_1 && keyval <= GDK_KEY_9)) {
            GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
            int n = gtk_notebook_get_n_pages(nb);
            int idx = (keyval == GDK_KEY_0) ? 0 : (keyval - GDK_KEY_1);
            if (n > 0)
                gtk_notebook_set_current_page(nb, MIN(idx, n - 1));
            return TRUE;
        }
        return FALSE;
    }
    /* Ctrl+Tab / Ctrl+Shift+Tab cycle tabs; Shift+Tab arrives as
     * ISO_Left_Tab on some keymaps. */
    if ((keyval == GDK_KEY_Tab || keyval == GDK_KEY_ISO_Left_Tab) &&
        (mods & GDK_CONTROL_MASK)) {
        cycle_tab(win, (mods & GDK_SHIFT_MASK) ? -1 : 1);
        return TRUE;
    }
    if (mods != GDK_CONTROL_MASK)
        return FALSE;

    switch (keyval) {
    case GDK_KEY_l:
        gtk_widget_grab_focus(win->url_entry);
        gtk_editable_select_region(GTK_EDITABLE(win->url_entry), 0, -1);
        return TRUE;
    case GDK_KEY_t:
        new_tab(win, NULL);
        return TRUE;
    case GDK_KEY_w: {
        GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
        GtkWidget *page = gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb));
        if (page)
            close_tab(win, page);
        return TRUE;
    }
    case GDK_KEY_f:
        find_open(win);
        return TRUE;
    case GDK_KEY_r:
        if (tab)
            webkit_web_view_reload(tab->view);
        return TRUE;
    case GDK_KEY_h: {
        const char *home = b->home_url && *b->home_url ? b->home_url : HOME_URL;
        char *home_uri = normalize_uri(home);
        if (tab)
            webkit_web_view_load_uri(tab->view, home_uri);
        g_free(home_uri);
        return TRUE;
    }
    case GDK_KEY_plus:
    case GDK_KEY_equal:
        if (tab)
            webkit_web_view_set_zoom_level(tab->view,
                                           webkit_web_view_get_zoom_level(tab->view) + 0.1);
        return TRUE;
    case GDK_KEY_minus:
        if (tab)
            webkit_web_view_set_zoom_level(tab->view,
                                           webkit_web_view_get_zoom_level(tab->view) - 0.1);
        return TRUE;
    case GDK_KEY_0:
        if (tab)
            webkit_web_view_set_zoom_level(tab->view, 1.0);
        return TRUE;
    case GDK_KEY_d:
        if (tab)
            bookmark_toggle(b, webkit_web_view_get_uri(tab->view),
                            webkit_web_view_get_title(tab->view));
        return TRUE;
    case GDK_KEY_u:
        if (tab)
            view_source_current(tab->view, tab);
        return TRUE;
    case GDK_KEY_s:
        if (tab)
            save_page_current(tab->view, tab);
        return TRUE;
    case GDK_KEY_1:
    case GDK_KEY_2:
    case GDK_KEY_3:
    case GDK_KEY_4:
    case GDK_KEY_5:
    case GDK_KEY_6:
    case GDK_KEY_7:
    case GDK_KEY_8:
    case GDK_KEY_9: {
        GtkNotebook *nb = GTK_NOTEBOOK(win->notebook);
        int n = gtk_notebook_get_n_pages(nb);
        int idx = keyval - GDK_KEY_1;
        if (n > 0)
            gtk_notebook_set_current_page(nb, MIN(idx, n - 1));
        return TRUE;
    }
    }
    return FALSE;
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
     * WebCore, so the text caret can stop blinking. If the window's focus is
     * nominally on the view (or nowhere at all), force a focus round-trip so
     * the view gets a fresh focus-in; leave it alone when another widget
     * (e.g. the URL bar) holds the focus. */
    Tab *tab = current_tab(win);
    if (!tab)
        return;
    GtkWidget *fw = gtk_window_get_focus(GTK_WINDOW(win->window));
    if (!fw || fw == GTK_WIDGET(tab->view)) {
        gtk_window_set_focus(GTK_WINDOW(win->window), NULL);
        gtk_window_set_focus(GTK_WINDOW(win->window), GTK_WIDGET(tab->view));
    }
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
    gtk_widget_set_sensitive(win->back_button, FALSE);
    gtk_widget_set_sensitive(win->forward_button, FALSE);
    gtk_widget_set_tooltip_text(win->back_button, "Back");
    gtk_widget_set_tooltip_text(win->forward_button, "Forward");
    gtk_widget_set_tooltip_text(win->reload_button, "Reload / Stop (F5)");

    win->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(win->url_entry), "Search or enter address");
    gtk_widget_set_hexpand(win->url_entry, TRUE);
    gtk_widget_set_tooltip_text(win->url_entry, "Ctrl+L to focus");
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
    load_settings(b);
    load_history(b);
    load_bookmarks(b);

    b->windows = g_ptr_array_new();
    b->url_model = gtk_list_store_new(1, G_TYPE_STRING);
    refresh_url_model(b);

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