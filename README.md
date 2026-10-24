# GtkBrowser

A minimal tabbed web browser built with **GTK3** and **WebKitGTK** (`webkit2gtk-4.1`).

## Build

```sh
sudo apt install libgtk-3-dev libwebkit2gtk-4.1-dev meson ninja
meson setup build
ninja -C build
```

Run it straight from the build tree:

```sh
./build/gtkbrowser [url]
```

## Install (optional)

```sh
meson install -C build
```

Installs:
- `gtkbrowser` → `/usr/local/bin`
- `gtkbrowser.desktop` → `/usr/local/share/applications` (generated at build
  time with the correct absolute path, so it shows up in your app menu)

## Features

- Tabs: `Ctrl+T` new, `Ctrl+W` close, double-click or middle-click a tab to
  close it, tabs are reorderable, `Ctrl+1`…`Ctrl+9` or `Alt+1`…`Alt+0` jump to
  a tab, `Ctrl+Tab` / `Ctrl+Shift+Tab` cycle, `Ctrl+Shift+T` reopens the most
  recently closed tab
- Windows: right-click a tab → "Move to New Window" splits it into its own
  window (the page and its state are kept — no reload); the app quits when
  the last window closes. The tab strip is fully hidden in a single-tab
  window (toggle in Settings)
- Back / forward / reload-stop buttons with a thin load progress bar;
  `Alt+Left` / `Alt+Right` navigate back / forward
- URL bar with search fallback and history/bookmark autocomplete — `Ctrl+L`
  to focus; search engine is selectable in Settings (incl. a custom engine)
- Padlock in the URL bar: green for HTTPS, red for plain HTTP
- Find in page: `Ctrl+F` — Enter next match, Shift+Enter previous, Esc close;
  shows an `N/M` match counter
- Page source: `Ctrl+U`; save page: `Ctrl+S`
- Page context menu: right-click a page → open a link / image / media in a new
  tab or window, copy its address, cut / copy / paste / select-all in a form
  field, copy or search selected text, view source, save page, or reload
  (replaces WebKit's default menu)
- Fullscreen: `F11`
- Zoom: `Ctrl+=` / `Ctrl+-` / `Ctrl+0`; default zoom for new tabs in Settings
- Home: `Ctrl+H`, reload: `F5` / `Ctrl+R`
- History and bookmarks: `Ctrl+D` toggles the current page; the star button
  opens a bookmark panel (double-click a row to open it); Settings →
  "Manage history and bookmarks" lets you open, delete, or clear entries
- Session restore: previous session's tabs and windows are reopened at start
  (toggleable in Settings)
- Ad blocking (uBlock-like) via a local proxy, with a live blocked-request
  counter in the toolbar; toggle and custom blocklist in Settings
- KeePass autofill: `Ctrl+Shift+L` fills the current page's login via
  `keepassxc-cli`; database chosen in Settings
- `window.open()` and `target="_blank"` links open in new tabs
- Downloads prompt a save-file dialog
- Drag & drop: drop a link or URL anywhere on the window (toolbar, tab
  strip, or page) and it opens — in a new tab, or the current tab if the
  page accepts the drop. A dropped file is handed to the page when the page
  handles drops itself, otherwise it opens in a new tab
- New tab page and custom user agent configurable in Settings

## Notes

- If pages fail to load with sandbox errors (e.g. missing bubblewrap), run with
  `./gtkbrowser --no-sandbox`.
- Developer tools are enabled: right-click → Inspect Element.
- WebKitGTK does not expose dropped file contents to web pages (file access
  is disabled in the GTK port — https://webkit.org/b/271957), so sites such
  as Discord cannot upload files dragged from the file manager, even though
  the drop event itself reaches the page.