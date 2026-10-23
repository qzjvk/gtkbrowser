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

- Tabs: `Ctrl+T` new, `Ctrl+W` close, double-click a tab to close it, tabs are reorderable
- Back / forward / reload-stop buttons with a load progress bar
- URL bar with search fallback (DuckDuckGo) — `Ctrl+L` to focus
- Favicon in the tab label and window title
- `window.open()` and `target="_blank"` links open in new tabs
- Downloads prompt a save-file dialog
- Zoom: `Ctrl+=` / `Ctrl+-` / `Ctrl+0`
- Home: `Ctrl+H`, reload: `F5` / `Ctrl+R`

## Notes

- If pages fail to load with sandbox errors (e.g. missing bubblewrap), run with
  `./gtkbrowser --no-sandbox`.
- Developer tools are enabled: right-click → Inspect Element.