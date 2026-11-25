# xcat

A small cross-platform X11 program that sits in a Unix text pipeline, in the role of `cat`.

It concatenates its inputs, shows the text in a window, lets you edit it, then:

- **OK** (or Ctrl+Enter / Ctrl+S) writes the buffer to standard output and exits 0
- **Cancel** (or Esc, or closing the window) writes `xcat: cancelled` to the controlling terminal (`/dev/tty`, not the pipe) and exits 1

Nothing is written to stdout on cancel, so a downstream command does not see a partial result.

With no file arguments it reads standard input, and leaves the buffer empty if stdin is a terminal. File arguments are concatenated in order, like `cat`. A lone `-` means standard input at that point. An unreadable file is an error before the window opens.

## Build

```sh
# Debian/Ubuntu
sudo apt install build-essential libx11-dev libxft-dev pkg-config

# Fedora
sudo dnf install gcc libX11-devel libXft-devel pkgconf-pkg-config

make
sudo make install   # optional, installs to /usr/local/bin
```

The source is POSIX C99 plus Xlib and Xft. It builds on Linux, the BSDs, illumos/Solaris, and macOS with XQuartz (`/opt/X11`). It does not use Linux-only APIs. Under Wayland it runs through XWayland like any other X11 client.

## Use

```sh
echo 'fix me' | xcat | tee out.txt

# concatenate files, edit, then hand the result to the next stage
xcat a.txt b.txt | patch

# stdin in the middle, same as cat
xcat header.txt - footer.txt > out.txt

# with pipefail, Cancel fails the pipeline
set -o pipefail
generate-report | xcat > report.txt
```

If there are no file arguments and stdin is a terminal, the buffer starts empty (so launching `xcat` does not swallow the keyboard). Otherwise the whole stream is read before the window opens. Input must be finite: do not use it as a coprocess that waits on `xcat`'s stdout before closing the pipe.

```sh
xcat [-t title] [-fn font] [-g WIDTHxHEIGHT] [file ...]
```

`-fn` is an Xft font name, for example `DejaVu Sans Mono:size=14`.

## Editing

- Type, Backspace, Delete, Enter, Tab
- Arrows, Home, End, Page Up/Down; Ctrl+Home/End move through the document
- Shift+arrows selects; drag with the mouse selects
- Ctrl+A select all, Ctrl+C copy, Ctrl+X cut, Ctrl+V paste
- Middle-click pastes PRIMARY
- Ctrl+K deletes to end of line
- Ctrl+Z undo
- Mouse wheel scrolls; Shift+wheel scrolls horizontally

The buffer is kept as bytes. Valid UTF-8 is edited a character at a time; a bad byte is one unit. OK writes those bytes unchanged, including a missing final newline.

## Exit status

| code | meaning                                      |
|------|----------------------------------------------|
| 0    | accepted, buffer written to stdout           |
| 1    | cancelled; message on the controlling terminal |
| 2    | could not open the display, read, or write   |

