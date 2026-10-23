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
#include "adblock.h"
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
#define TAB_LABEL_MAX_CHARS 24
/* Measured per-tab chrome (icon + close button + spacing + notebook tab
 * border) that is not part of the title label. */
#define TAB_OVERHEAD 40
/* Never shrink a tab label below this width. */
#define TAB_LABEL_FLOOR 60

typedef struct {
    GtkWidget *window;
    GtkWidget *notebook;
    GtkWidget *url_entry;
    GtkWidget *back_button;
    GtkWidget *forward_button;
    GtkWidget *reload_button;
    GtkWidget *progress;
    AdbProxy *proxy;
    gboolean adblock_enabled;
    gboolean devtools_enabled;
    gchar *blocklist_file;
    GSocketService *control_service;
    gchar *control_socket_path;
} Browser;

typedef struct {
    Browser *browser;
    WebKitWebView *view;
    GtkWidget *icon_image;
    GtkWidget *title_label;
    int full_label_width;    /* natural width of the title text */
    int applied_label_width; /* last size_request applied to the label */
} Tab;

/* forward declarations */
static GtkWidget *new_tab(Browser *b, const char *uri);
static void close_tab(Browser *b, GtkWidget *page);
static void update_url_entry(Browser *b, Tab *tab);
static void update_nav_buttons(Browser *b, Tab *tab);
static void update_window_title(Browser *b, Tab *tab);
static void relayout_tabs(Browser *b);
static char *normalize_uri(const char *s);
static void set_adblock_enabled(Browser *b, gboolean enabled);

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

/* Measure the natural pixel width of the tab's title text (clamped to
 * TAB_LABEL_MAX_CHARS) and store it in tab->full_label_width. */
static void measure_tab_label(Tab *tab)
{
    const char *text = gtk_label_get_text(GTK_LABEL(tab->title_label));
    if (!text || !*text)
        text = "New Tab";

    int w, h;
    PangoLayout *layout = gtk_widget_create_pango_layout(tab->title_label, text);
    pango_layout_get_pixel_size(layout, &w, &h);
    g_object_unref(layout);

    char *max_str = g_strnfill(TAB_LABEL_MAX_CHARS, '0');
    PangoLayout *max_layout = gtk_widget_create_pango_layout(tab->title_label, max_str);
    int max_w, dummy;
    pango_layout_get_pixel_size(max_layout, &max_w, &dummy);
    g_object_unref(max_layout);
    g_free(max_str);

    if (w > max_w)
        w = max_w;
    tab->full_label_width = w;
}

/* Scale every tab's title label so all tabs fit within the window width.
 * When there is plenty of room each tab keeps its natural width; when the
 * window is narrow the labels shrink proportionally (down to TAB_LABEL_FLOOR)
 * instead of forcing the window to grow. Only touches a label when the
 * computed width actually changes (avoids a size-allocation feedback loop). */
static void relayout_tabs(Browser *b)
{
    GtkNotebook *nb = GTK_NOTEBOOK(b->notebook);
    int n = gtk_notebook_get_n_pages(nb);
    if (n == 0)
        return;

    int win_w = gtk_widget_get_allocated_width(b->notebook);
    if (win_w <= 0)
        win_w = gtk_widget_get_allocated_width(b->window);
    if (win_w <= 0)
        return;

    int available = win_w - n * TAB_OVERHEAD;
    if (available < n * TAB_LABEL_FLOOR)
        available = n * TAB_LABEL_FLOOR;

    int sum_full = 0;
    for (int i = 0; i < n; i++) {
        Tab *tab = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(nb, i)), "tab");
        if (tab)
            sum_full += tab->full_label_width;
    }
    if (sum_full <= 0)
        return;

    double scale = 1.0;
    if (sum_full > available)
        scale = (double)available / (double)sum_full;

    for (int i = 0; i < n; i++) {
        Tab *tab = g_object_get_data(G_OBJECT(gtk_notebook_get_nth_page(nb, i)), "tab");
        if (!tab)
            continue;
        int w = (int)(tab->full_label_width * scale);
        if (w < TAB_LABEL_FLOOR)
            w = TAB_LABEL_FLOOR;
        if (w != tab->applied_label_width) {
            gtk_widget_set_size_request(tab->title_label, w, -1);
            tab->applied_label_width = w;
        }
    }
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
    else
        relayout_tabs(b);
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
    gtk_label_set_text(GTK_LABEL(tab->title_label), text);
    gtk_widget_set_tooltip_text(tab->title_label, text);
    measure_tab_label(tab);
    relayout_tabs(tab->browser);
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
    measure_tab_label(tab);
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
    relayout_tabs(b);
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
    gtk_box_pack_start(GTK_BOX(content), devtools_check, FALSE, FALSE, 0);

    g_signal_connect(adblock_check, "toggled", G_CALLBACK(on_adblock_toggled), b);
    g_signal_connect(devtools_check, "toggled", G_CALLBACK(on_devtools_toggled), b);

    gtk_widget_show_all(dialog);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
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

static gboolean on_configure(GtkWidget *w, GdkEventConfigure *e, Browser *b)
{
    relayout_tabs(b);
    return FALSE;
}

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

    b->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(b->window), APP_NAME);
    gtk_window_set_default_size(GTK_WINDOW(b->window), 1100, 700);
    g_signal_connect(b->window, "destroy", G_CALLBACK(on_destroy), b);
    g_signal_connect(b->window, "key-press-event", G_CALLBACK(on_key_press), b);
    g_signal_connect(b->window, "configure-event", G_CALLBACK(on_configure), b);

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

    gtk_box_pack_start(GTK_BOX(toolbar), b->back_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->forward_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->reload_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(toolbar), b->url_entry, TRUE, TRUE, 0);
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

    gtk_widget_show_all(b->window);
    gtk_main();
    return 0;
}