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
#define SEARCH_URL "https://duckduckgo.com/?q=%s"
/* Cap on the visible tab title length (the tooltip keeps the full text).
 * Bounding the natural label width keeps the tab strip predictable. */
#define TAB_LABEL_MAX_CHARS 24

typedef struct {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *url_entry;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *reload_button;
    GtkWidget *progress;
    GtkWidget *blocked_button;
    GtkWidget *blocked_label;
    AdbProxy *proxy;
    gboolean adblock_enabled;
    gboolean devtools_enabled;
    gchar *blocklist_file;
    GtkWidget *kp_fill_button;
    gchar *kp_db_path;   /* configured .kdbx path (persisted) */
    gchar *kp_password;  /* session cache of the db password */
    GSocketService *control_service;
    gchar *control_socket_path;
} Browser;

typedef struct {
    Browser *browser;
    WebKitWebView *view;
    GtkWidget *icon_image;
    GtkWidget *title_label;
} Tab;

/* forward declarations */
static GtkWidget *new_tab(Browser *b, const char *uri);
static void close_tab(Browser *b, GtkWidget *page);
static void update_url_entry(Browser *b, Tab *tab);
static void update_nav_buttons(Browser *b, Tab *tab);
static void update_window_title(Browser *b, Tab *tab);
static char *normalize_uri(const char *s);
static void set_adblock_enabled(Browser *b, gboolean enabled);
static gboolean update_blocked_label(gpointer data);
static void fill_login(Browser *b);
static void choose_kp_database(GtkButton *btn, Browser *b);
static void kp_build_settings_section(Browser *b, GtkWidget *content);
static void on_blocked_clicked(GtkButton *btn, Browser *b);

/* ---------- helpers ---------- */

static Tab *current_tab(Browser *b)
{
    GtkNotebook *nb = GTK_NOTEBOOK(b->notebook);
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

static void update_url_entry(Browser *b, Tab *tab)
{
    if (!tab)
        return;
    const char *uri = webkit_web_view_get_uri(tab->view);
    gtk_entry_set_text(GTK_ENTRY(b->url_entry), uri ? uri : "");
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

static void update_nav_buttons(Browser *b, Tab *tab)
{
    if (!tab)
        return;
    gtk_widget_set_sensitive(b->back_button, webkit_web_view_can_go_back(tab->view));
    gtk_widget_set_sensitive(b->forward_button, webkit_web_view_can_go_forward(tab->view));
    const char *icon = webkit_web_view_is_loading(tab->view) ? "process-stop" : "view-refresh";
    gtk_button_set_image(GTK_BUTTON(b->reload_button),
                         gtk_image_new_from_icon_name(icon, GTK_ICON_SIZE_LARGE_TOOLBAR));
}

static void update_window_title(Browser *b, Tab *tab)
{
    if (!tab) {
        gtk_window_set_title(GTK_WINDOW(b->window), APP_NAME);
        return;
    }
    const char *title = webkit_web_view_get_title(tab->view);
    if (title && *title) {
        char *t = g_strdup_printf("%s — %s", title, APP_NAME);
        gtk_window_set_title(GTK_WINDOW(b->window), t);
        g_free(t);
    } else {
        gtk_window_set_title(GTK_WINDOW(b->window), APP_NAME);
    }
}

static void load_uri(Browser *b, const char *uri)
{
    Tab *tab = current_tab(b);
    if (tab)
        webkit_web_view_load_uri(tab->view, uri);
}

static void navigate_from_entry(Browser *b)
{
    const char *text = gtk_entry_get_text(GTK_ENTRY(b->url_entry));
    if (!text || !*text)
        return;
    if (looks_like_url(text)) {
        char *uri = normalize_uri(text);
        load_uri(b, uri);
        g_free(uri);
    } else {
        char *q = g_uri_escape_string(text, NULL, TRUE);
        char *url = g_strdup_printf(SEARCH_URL, q);
        g_free(q);
        load_uri(b, url);
        g_free(url);
    }
}

/* ---------- tabs ---------- */

static void close_tab(Browser *b, GtkWidget *page)
{
    GtkNotebook *nb = GTK_NOTEBOOK(b->notebook);
    gtk_notebook_remove_page(nb, gtk_notebook_page_num(nb, page));
    if (gtk_notebook_get_n_pages(nb) == 0)
        gtk_widget_destroy(b->window);
}

static void on_tab_close_clicked(GtkWidget *page, gpointer unused)
{
    Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
    close_tab(tab->browser, page);
}

static gboolean on_tab_button_press(GtkWidget *w, GdkEventButton *e, gpointer page)
{
    if (e->type == GDK_2BUTTON_PRESS) {
        Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
        close_tab(tab->browser, page);
        return TRUE;
    }
    return FALSE;
}

/* ---------- web view signal handlers ---------- */

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, Tab *tab)
{
    Browser *b = tab->browser;
    if (current_tab(b) == tab) {
        update_url_entry(b, tab);
        update_nav_buttons(b, tab);
        update_window_title(b, tab);
    }
}

static void on_notify_title(WebKitWebView *view, GParamSpec *ps, Tab *tab)
{
    const char *title = webkit_web_view_get_title(view);
    const char *uri = webkit_web_view_get_uri(view);
    const char *text = (title && *title) ? title : (uri ? uri : "New Tab");
    gtk_label_set_text(GTK_LABEL(tab->title_label), truncate_tab_title(text));
    gtk_widget_set_tooltip_text(tab->title_label, text);
    if (current_tab(tab->browser) == tab)
        update_window_title(tab->browser, tab);
}

static void on_notify_uri(WebKitWebView *view, GParamSpec *ps, Tab *tab)
{
    if (current_tab(tab->browser) == tab)
        update_url_entry(tab->browser, tab);
}

static void on_notify_loading(WebKitWebView *view, GParamSpec *ps, Tab *tab)
{
    Browser *b = tab->browser;
    if (current_tab(b) != tab)
        return;
    if (!webkit_web_view_is_loading(view))
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(b->progress), 0);
}

static void on_notify_progress(WebKitWebView *view, GParamSpec *ps, Tab *tab)
{
    Browser *b = tab->browser;
    if (current_tab(b) != tab)
        return;
    if (webkit_web_view_is_loading(view))
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(b->progress),
                                      webkit_web_view_get_estimated_load_progress(view));
    else
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(b->progress), 0);
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
    if (current_tab(tab->browser) == tab)
        gtk_window_set_icon(GTK_WINDOW(tab->browser->window), pixbuf);
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
            new_tab(tab->browser, uri);
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

    GtkWidget *dialog = gtk_file_chooser_dialog_new("Save file",
                                                    GTK_WINDOW(b->window),
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

static GtkWidget *new_tab(Browser *b, const char *uri)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    WebKitWebView *view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    Tab *tab = g_new0(Tab, 1);
    tab->browser = b;
    tab->view = view;

    WebKitSettings *settings = webkit_web_view_get_settings(view);
    webkit_settings_set_enable_developer_extras(settings, b->devtools_enabled);
    webkit_settings_set_enable_smooth_scrolling(settings, TRUE);

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

    g_object_set_data_full(G_OBJECT(page), "tab", tab, g_free);
    gtk_notebook_append_page(GTK_NOTEBOOK(b->notebook), page, label_box);
    gtk_notebook_set_tab_reorderable(GTK_NOTEBOOK(b->notebook), page, TRUE);

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
    g_signal_connect_swapped(close_btn, "clicked", G_CALLBACK(on_tab_close_clicked), page);
    g_signal_connect(label_box, "button-press-event", G_CALLBACK(on_tab_button_press), page);

    char *load_uri_str = normalize_uri(uri && *uri ? uri : HOME_URL);
    webkit_web_view_load_uri(view, load_uri_str);
    g_free(load_uri_str);

    gtk_notebook_set_current_page(GTK_NOTEBOOK(b->notebook),
                                  gtk_notebook_page_num(GTK_NOTEBOOK(b->notebook), page));
    update_url_entry(b, tab);
    update_nav_buttons(b, tab);
    update_window_title(b, tab);
    return page;
}

/* ---------- toolbar / entry handlers ---------- */

static void on_back_clicked(GtkButton *btn, Browser *b)
{
    Tab *tab = current_tab(b);
    if (tab && webkit_web_view_can_go_back(tab->view))
        webkit_web_view_go_back(tab->view);
}

static void on_forward_clicked(GtkButton *btn, Browser *b)
{
    Tab *tab = current_tab(b);
    if (tab && webkit_web_view_can_go_forward(tab->view))
        webkit_web_view_go_forward(tab->view);
}

static void on_reload_clicked(GtkButton *btn, Browser *b)
{
    Tab *tab = current_tab(b);
    if (!tab)
        return;
    if (webkit_web_view_is_loading(tab->view))
        webkit_web_view_stop_loading(tab->view);
    else
        webkit_web_view_reload(tab->view);
}

static void on_entry_activate(GtkEntry *entry, Browser *b)
{
    navigate_from_entry(b);
}

static void on_new_tab_clicked(GtkButton *btn, Browser *b)
{
    new_tab(b, NULL);
}

static void on_close_tab_clicked(GtkButton *btn, Browser *b)
{
    GtkNotebook *nb = GTK_NOTEBOOK(b->notebook);
    GtkWidget *page = gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb));
    if (page)
        close_tab(b, page);
}

/*
 * NOTE: during the "switch-page" signal, gtk_notebook_get_current_page()
 * still reports the OLD page. The new page is passed as the `page`
 * argument, so we must use that — reading current_page here is what used
 * to make the URL bar / nav buttons / title show the previous tab.
 */
static void on_switch_page(GtkNotebook *nb, GtkWidget *page, guint num, Browser *b)
{
    Tab *tab = page ? g_object_get_data(G_OBJECT(page), "tab") : NULL;
    update_url_entry(b, tab);
    update_nav_buttons(b, tab);
    update_window_title(b, tab);
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

static void on_devtools_toggled(GtkToggleButton *btn, Browser *b)
{
    b->devtools_enabled = gtk_toggle_button_get_active(btn);
    GtkNotebook *nb = GTK_NOTEBOOK(b->notebook);
    int n = gtk_notebook_get_n_pages(nb);
    for (int i = 0; i < n; i++) {
        GtkWidget *page = gtk_notebook_get_nth_page(nb, i);
        Tab *tab = g_object_get_data(G_OBJECT(page), "tab");
        if (tab) {
            WebKitSettings *settings = webkit_web_view_get_settings(tab->view);
            webkit_settings_set_enable_developer_extras(settings, b->devtools_enabled);
        }
    }
}

/* Refresh the toolbar "trackers blocked" counter. Runs from a 1s timeout
 * so it stays live while pages load. */
static gboolean update_blocked_label(gpointer data)
{
    Browser *b = data;
    if (!b->blocked_label)
        return G_SOURCE_CONTINUE;
    guint64 count = b->proxy ? adb_proxy_blocked_count(b->proxy) : 0;
    char buf[48];
    g_snprintf(buf, sizeof(buf), "%llu", (unsigned long long)count);
    gtk_label_set_text(GTK_LABEL(b->blocked_label), buf);
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
    GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(b->window),
                                               GTK_DIALOG_MODAL,
                                               GTK_MESSAGE_INFO,
                                               GTK_BUTTONS_OK, "%s", buf);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}

static void on_settings_clicked(GtkButton *btn, Browser *b)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Settings",
                                                    GTK_WINDOW(b->window),
                                                    GTK_DIALOG_MODAL,
                                                    "_Close", GTK_RESPONSE_CLOSE,
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

    kp_build_settings_section(b, content);

    g_signal_connect(adblock_check, "toggled", G_CALLBACK(on_adblock_toggled), b);
    g_signal_connect(devtools_check, "toggled", G_CALLBACK(on_devtools_toggled), b);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
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
    if (g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL))
        b->kp_db_path = g_key_file_get_string(kf, "keepass", "db", NULL);
    g_key_file_unref(kf);
    g_free(path);
}

static void save_settings(Browser *b)
{
    GKeyFile *kf = g_key_file_new();
    if (b->kp_db_path)
        g_key_file_set_string(kf, "keepass", "db", b->kp_db_path);
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
                                                    GTK_WINDOW(b->window),
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
        d = gtk_message_dialog_new(GTK_WINDOW(fr->b->window), GTK_DIALOG_MODAL,
                                   GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", buf);
    } else {
        d = gtk_message_dialog_new(GTK_WINDOW(fr->b->window), GTK_DIALOG_MODAL,
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
    Tab *tab = current_tab(b);
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
                                                    GTK_WINDOW(b->window),
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
        d = gtk_message_dialog_new(GTK_WINDOW(b->window), GTK_DIALOG_MODAL,
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
        d = gtk_message_dialog_new(GTK_WINDOW(b->window), GTK_DIALOG_MODAL,
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
        d = gtk_message_dialog_new(GTK_WINDOW(b->window), GTK_DIALOG_MODAL,
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
    Tab *tab = current_tab(b);
    if (!tab)
        return;
    GtkWidget *d;
    if (!b->kp_db_path) {
        d = gtk_message_dialog_new(GTK_WINDOW(b->window), GTK_DIALOG_MODAL,
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
        d = gtk_message_dialog_new(GTK_WINDOW(b->window), GTK_DIALOG_MODAL,
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
                                                    GTK_WINDOW(b->window),
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
        GtkWidget *d = gtk_message_dialog_new(GTK_WINDOW(b->window),
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

/* ---------- keyboard shortcuts ---------- */

static gboolean on_key_press(GtkWidget *w, GdkEventKey *event, Browser *b)
{
    Tab *tab = current_tab(b);
    guint keyval = event->keyval;
    GdkModifierType mods = event->state & gtk_accelerator_get_default_mod_mask();

    if (keyval == GDK_KEY_F5) {
        if (tab)
            webkit_web_view_reload(tab->view);
        return TRUE;
    }
    if ((mods == (GDK_CONTROL_MASK | GDK_SHIFT_MASK)) &&
        (keyval == GDK_KEY_l || keyval == GDK_KEY_L)) {
        fill_login(b);
        return TRUE;
    }
    if (mods != GDK_CONTROL_MASK)
        return FALSE;

    switch (keyval) {
    case GDK_KEY_l:
        gtk_widget_grab_focus(b->url_entry);
        gtk_editable_select_region(GTK_EDITABLE(b->url_entry), 0, -1);
        return TRUE;
    case GDK_KEY_t:
        new_tab(b, NULL);
        return TRUE;
    case GDK_KEY_w: {
        GtkNotebook *nb = GTK_NOTEBOOK(b->notebook);
        GtkWidget *page = gtk_notebook_get_nth_page(nb, gtk_notebook_get_current_page(nb));
        if (page)
            close_tab(b, page);
        return TRUE;
    }
    case GDK_KEY_r:
        if (tab)
            webkit_web_view_reload(tab->view);
        return TRUE;
    case GDK_KEY_h:
        if (tab)
            webkit_web_view_load_uri(tab->view, HOME_URL);
        return TRUE;
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
    if (g_str_has_prefix(cmd, "open ")) {
        const char *url = cmd + 5;
        if (*url) {
            char *uri = normalize_uri(url);
            new_tab(b, uri);
            g_free(uri);
        }
        return g_strdup("ok");
    }
    if (strcmp(cmd, "list") == 0) {
        GtkNotebook *nb = GTK_NOTEBOOK(b->notebook);
        int n = gtk_notebook_get_n_pages(nb);
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
        Tab *tab = current_tab(b);
        const char *uri = tab ? webkit_web_view_get_uri(tab->view) : NULL;
        return g_strdup(uri ? uri : "");
    }
    if (strcmp(cmd, "title") == 0) {
        Tab *tab = current_tab(b);
        const char *title = tab ? webkit_web_view_get_title(tab->view) : NULL;
        return g_strdup(title ? title : "");
    }
    if (strcmp(cmd, "blocked") == 0) {
        guint64 count = b->proxy ? adb_proxy_blocked_count(b->proxy) : 0;
        return g_strdup_printf("%llu", (unsigned long long)count);
    }
    if (g_str_has_prefix(cmd, "focus ")) {
        int n = atoi(cmd + 6);
        GtkNotebook *nb = GTK_NOTEBOOK(b->notebook);
        if (n >= 0 && n < gtk_notebook_get_n_pages(nb))
            gtk_notebook_set_current_page(nb, n);
        return g_strdup("ok");
    }
    if (strcmp(cmd, "close") == 0 || g_str_has_prefix(cmd, "close ")) {
        GtkNotebook *nb = GTK_NOTEBOOK(b->notebook);
        int n = (cmd[5] == '\0') ? gtk_notebook_get_current_page(nb) : atoi(cmd + 6);
        if (n >= 0 && n < gtk_notebook_get_n_pages(nb)) {
            GtkWidget *page = gtk_notebook_get_nth_page(nb, n);
            close_tab(b, page);
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

static void on_destroy(GtkWidget *w, Browser *b)
{
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
    gtk_main_quit();
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
    gtk_init(&argc, &argv);

    /* Force the software renderer. When the GPU/DMABUF renderer cannot get
     * permission to create GPU buffers (VMs, containers, headless setups),
     * WebKit leaves the page blank; software rendering always works. */
    setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);

    if (no_sandbox)
        webkit_web_context_set_sandbox_enabled(webkit_web_context_get_default(), FALSE);

    apply_css();

    Browser *b = g_new0(Browser, 1);
    b->blocklist_file = blocklist ? g_strdup(blocklist) : NULL;
    b->adblock_enabled = TRUE;
    b->devtools_enabled = TRUE;
    load_settings(b);

    b->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(b->window), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(b->window), 1100, 700);
    g_signal_connect(b->window, "destroy", G_CALLBACK(on_destroy), b);
    g_signal_connect(b->window, "key-press-event", G_CALLBACK(on_key_press), b);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(b->window), vbox);

    /* toolbar */
    GtkWidget *toolbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_style_context_add_class(gtk_widget_get_style_context(toolbar), "toolbar");
    gtk_container_set_border_width(GTK_CONTAINER(toolbar), 4);

    b->back_button = gtk_button_new_from_icon_name("go-previous", GTK_ICON_SIZE_LARGE_TOOLBAR);
    b->forward_button = gtk_button_new_from_icon_name("go-next", GTK_ICON_SIZE_LARGE_TOOLBAR);
    b->reload_button = gtk_button_new_from_icon_name("view-refresh", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_sensitive(b->back_button, FALSE);
    gtk_widget_set_sensitive(b->forward_button, FALSE);
    gtk_widget_set_tooltip_text(b->back_button, "Back");
    gtk_widget_set_tooltip_text(b->forward_button, "Forward");
    gtk_widget_set_tooltip_text(b->reload_button, "Reload / Stop (F5)");

    b->url_entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(b->url_entry), "Search or enter address");
    gtk_widget_set_hexpand(b->url_entry, TRUE);
    gtk_widget_set_tooltip_text(b->url_entry, "Ctrl+L to focus");

    GtkWidget *settings_button = gtk_button_new_from_icon_name("preferences-system",
                                                               GTK_ICON_SIZE_LARGE_TOOLBAR);
    GtkWidget *new_tab_button = gtk_button_new_from_icon_name("list-add", GTK_ICON_SIZE_LARGE_TOOLBAR);
    GtkWidget *close_tab_button = gtk_button_new_from_icon_name("window-close", GTK_ICON_SIZE_LARGE_TOOLBAR);
    gtk_widget_set_tooltip_text(settings_button, "Settings");
    gtk_widget_set_tooltip_text(new_tab_button, "New tab (Ctrl+T)");
    gtk_widget_set_tooltip_text(close_tab_button, "Close tab (Ctrl+W)");

    b->blocked_button = gtk_button_new();
    {
        GtkWidget *bb = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        GtkWidget *bimg = gtk_image_new_from_icon_name("security-high",
                                                       GTK_ICON_SIZE_SMALL_TOOLBAR);
        b->blocked_label = gtk_label_new("0");
        gtk_box_pack_start(GTK_BOX(bb), bimg, FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(bb), b->blocked_label, FALSE, FALSE, 0);
        gtk_container_add(GTK_CONTAINER(b->blocked_button), bb);
    }
    gtk_widget_set_tooltip_text(b->blocked_button,
                                "Trackers blocked — click for details");
    g_signal_connect(b->blocked_button, "clicked",
                     G_CALLBACK(on_blocked_clicked), b);

    gtk_box_pack_start(GTK_BOX(toolbar), b->back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->reload_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->url_entry, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->blocked_button, FALSE, FALSE, 0);

    b->kp_fill_button = gtk_button_new_from_icon_name("dialog-password",
                                                      GTK_ICON_SIZE_SMALL_TOOLBAR);
    gtk_widget_set_tooltip_text(b->kp_fill_button,
                                "Fill login form from KeePass (Ctrl+Shift+L)");
    g_signal_connect(b->kp_fill_button, "clicked",
                     G_CALLBACK(on_fill_button_clicked), b);
    gtk_box_pack_start(GTK_BOX(toolbar), b->kp_fill_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(toolbar), settings_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), new_tab_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), close_tab_button, FALSE, FALSE, 0);

    gtk_box_pack_start(GTK_BOX(vbox), toolbar, FALSE, FALSE, 0);

    /* thin progress bar between the toolbar and the tabs */
    b->progress = gtk_progress_bar_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(b->progress), "thin");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(b->progress), 0);
    gtk_box_pack_start(GTK_BOX(vbox), b->progress, FALSE, FALSE, 0);

    b->notebook = gtk_notebook_new();
    gtk_notebook_set_show_border(GTK_NOTEBOOK(b->notebook), FALSE);
    /* Scrollable so many tabs use arrows instead of forcing the window's
     * minimum size to grow with each new tab. */
    gtk_notebook_set_scrollable(GTK_NOTEBOOK(b->notebook), TRUE);
    gtk_box_pack_start(GTK_BOX(vbox), b->notebook, TRUE, TRUE, 0);

    g_signal_connect(b->back_button, "clicked", G_CALLBACK(on_back_clicked), b);
    g_signal_connect(b->forward_button, "clicked", G_CALLBACK(on_forward_clicked), b);
    g_signal_connect(b->reload_button, "clicked", G_CALLBACK(on_reload_clicked), b);
    g_signal_connect(settings_button, "clicked", G_CALLBACK(on_settings_clicked), b);
    g_signal_connect(new_tab_button, "clicked", G_CALLBACK(on_new_tab_clicked), b);
    g_signal_connect(close_tab_button, "clicked", G_CALLBACK(on_close_tab_clicked), b);
    g_signal_connect(b->url_entry, "activate", G_CALLBACK(on_entry_activate), b);
    g_signal_connect(b->notebook, "switch-page", G_CALLBACK(on_switch_page), b);
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

    /* first tab: URL from the command line, or the home page */
    new_tab(b, initial);

    update_blocked_label(b);
    g_timeout_add(1000, update_blocked_label, b);

    gtk_widget_show_all(b->window);
    gtk_main();
    return 0;
}