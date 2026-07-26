# AGENTS.md — qi text editor

## Project Overview

**qi** (pronounced *key*) is a lightweight, ncurses-based terminal text editor written in C. It targets macOS and Linux and emphasises simplicity, low memory use, and support for files of any size.

- **Current version**: 1.1.45
- **License**: GPL v3 (as of v1.1.9; earlier versions are proprietary)
- **Author**: Christopher Camacho
- **Repository**: https://github.com/quatscho/qi

---

## Standing Rules

- **Always bump the version** (`VERSION` constant in `qi.c` and the comment at the top) for every substantive code change, unless the user explicitly says "don't bump this version" — that instruction applies **only to that specific instance**.
- **Always produce a versioned tarball** named `qi-X.Y.Z.tar.gz` that extracts to `qi/` after any change that warrants a new tarball.
- **UTF-8 rule**: The editor is byte-indexed internally. A `utf8_display_width()` helper exists for display-column calculations. When editing any feature that uses `strlen()` for visual-width purposes, convert that call to `utf8_display_width()` / `utf8_display_width_n()` as part of the same change.
- **Color pairs** are defined in `color.h` / `color.c` via named constants (e.g. `PAIR_YELLOW`, `PAIR_RED`). Do not use raw `COLOR_PAIR(n)` integers in `qi.c` — always use the named constants.
- **Do not add vertical lines** between the gutter and the text area; the separator is a single `│` character drawn as part of the gutter.

---

## Source File Structure

| File | Purpose |
|---|---|
| `qi.c` | Main editor: globals, rendering, input loop, all editing operations |
| `syntax.c` / `syntax.h` | Syntax highlighting engine; language detection and span generation |
| `tracker.c` / `tracker.h` | Per-line dirty-tracking (marks modified lines yellow) |
| `color.c` / `color.h` | ncurses color-pair initialisation and named constants |
| `Makefile` | Build system; auto-detects Homebrew on macOS |
| `CHANGELOG.md` | Version history |
| `tag_releases.sh` | Helper: creates annotated git tags from version-shaped commit messages |
| `LICENSE` | GPL v3 |
| `README.md` | User-facing build and usage instructions |

**Build command**: `make`
**Install**: `make install` (to `/usr/local/bin`) or `make install LOCAL=1` (to `~/bin`)
**Sources**: `qi.c tracker.c syntax.c color.c`

---

## Key Constants and Globals (qi.c)

| Symbol | Value / Type | Purpose |
|---|---|---|
| `VERSION` | `"1.1.45"` | Version string shown in title bar and `--version` |
| `MAX_LINE_LEN` | 512 | Maximum bytes per line |
| `MAX_UNDO` / `UNDO_CAP` | 500 | Undo/redo ring buffer capacity |
| `read_only_mode` | `int` | 1 = read-only; toggled by F3 or `-r` flag |
| `overwrite_mode` | `int` | 1 = overwrite; toggled by Ctrl+X |
| `gutter_visible` | `int` | 1 = show line numbers; toggled by F4 or `-L` flag |
| `syntax_highlight_enabled` | `int` | 1 = syntax on; toggled by F5 |
| `create_backup` | `int` | 1 = create `.bak` before save; set by `-b` flag |
| `file_mtime` | `time_t` | mtime captured on load/save; used for external-change detection |
| `got_fatal_signal` | `volatile sig_atomic_t` | Set by SIGTERM/SIGHUP handler; triggers safe exit |
| `clipboard_line` | `char *` | Internal clipboard for cut/copy/paste line operations |

---

## Command-Line Flags

| Flag | Effect |
|---|---|
| `-h` / `--help` | Print usage and exit |
| `-v` / `--version` | Print version and exit |
| `-r` / `--read-only` | Open in read-only mode |
| `-L` / `--no-line-numbers` | Start with gutter hidden |
| `-b` / `--backup` | Create `.bak` backup before each save |
| `+N` or `:N` | Open at line N |

Invoking the binary as `view` or `roqi` also enables read-only mode automatically.

---

## Keybindings

### File Operations
| Key | Action |
|---|---|
| Ctrl+S | Save |
| Alt/Opt+S | Save As |
| Ctrl+O | Open file |
| Ctrl+Q | Quit (prompts if unsaved) |

### Navigation
| Key | Action |
|---|---|
| Arrow keys | Move cursor |
| Ctrl+T | Jump to top of file |
| Ctrl+B | Jump to bottom of file |
| Ctrl+A | Smart line start (first non-whitespace / column 0 toggle) |
| Ctrl+E / End | End of line |
| Home | Beginning of line |
| PgUp / PgDn | Page up / page down |
| Ctrl+Left / Word-left | Jump word left |
| Ctrl+Right / Word-right | Jump word right |
| Ctrl+G | Go to line number |
| Alt/Opt+B | Jump to matching bracket |

### Editing
| Key | Action |
|---|---|
| Ctrl+N | Duplicate current line |
| Ctrl+D | Delete line(s) interactive (supports `!` for force-delete) |
| Ctrl+K / Ctrl+Shift+K | Cut line(s) |
| Ctrl+C | Copy line(s) |
| Ctrl+P | Paste line(s) |
| Ctrl+W | Delete word left |
| Alt/Opt+J | Move line down |
| Alt/Opt+K | Move line up |
| Tab | Indent (4 spaces; 1 true tab in Makefiles) |
| Shift+Tab | Dedent |
| Ctrl+X | Toggle overwrite mode |

### Search & Replace
| Key | Action |
|---|---|
| Ctrl+F | Find (case-insensitive, continues from cursor) |
| Ctrl+R | Find & Replace (with confirmation prompts) |

### Undo / Redo
| Key | Action |
|---|---|
| Ctrl+U | Undo (up to 500 operations) |
| Ctrl+Y | Redo |

### View Toggles
| Key | Action |
|---|---|
| F3 | Toggle read-only mode |
| F4 | Toggle gutter (line numbers) + col-81 guide |
| F5 | Toggle syntax highlighting |

### Help / About
| Key | Action |
|---|---|
| Ctrl+/ | Show help window |
| Alt/Opt+A | Show about window |

---

## Syntax Highlighting

Supported languages (detected by file extension):

| Language | Extensions |
|---|---|
| C / C++ | `.c`, `.h`, `.cpp`, `.hpp`, `.cc` |
| Python | `.py` |
| Shell / Bash / Zsh | `.sh`, `.bash`, `.zsh`, `.ksh`, dotfiles (`.zshrc`, `.bashrc`, etc.) |
| Makefile | `Makefile`, `makefile`, `*.mk` |
| Markdown | `.md`, `.markdown` |
| Lua | `.lua` |
| JavaScript / TypeScript | `.js`, `.ts`, `.jsx`, `.tsx`, `.mjs` |
| Rust | `.rs` |
| PHP | `.php`, `.php3`, `.php4`, `.php5`, `.phtml` |
| CSS | `.css`, `.scss`, `.less` |

Toggle with **F5**.

---

## Status Bar Indicators

The status bar (bottom two rows) shows:
- Filename, modified flag (`*`), line count
- `Ln:` / `Col:` cursor position
- Mode badge: **RO** (read-only, red), **OW** (overwrite, yellow), **RW** (read-write, green)
- Auto-save countdown / last-save time
- Status messages (search results, errors, etc.)

The title bar (top row) shows the current directory path.

---

## Help Window

The help window is a scrollable ncurses popup. The top border uses `ACS_RTEE` (`┤`) and `ACS_LTEE` (`├`) flanking the version string; the bottom border uses the same flanking the copyright string. This is the approved appearance — do not change it to a tab/bump shape.

---

## Notable Implementation Details

- **UTF-8**: `utf8_display_width(str)` and `utf8_display_width_n(str, n)` use `wcwidth()` for correct column counting. `setlocale(LC_ALL, "")` is called at startup. `_XOPEN_SOURCE 700` is defined at the top of `qi.c` to expose `wcwidth()`.
- **Undo system**: Delta-based ring buffer (`MAX_UNDO = 500`). Operations: `OP_CHAR_INS`, `OP_CHAR_DEL`, `OP_LINE_SPLIT`, `OP_LINE_JOIN`, `OP_BULK`. Paste batching groups rapid insertions into a single undo record.
- **External file change detection**: `stat()` is called each main loop iteration; if `st_mtime` differs from `file_mtime`, the user is prompted to reload.
- **SIGTERM / SIGHUP**: Handler sets `got_fatal_signal`; main loop saves if modified and exits cleanly.
- **Bracketed paste**: Enabled via `\033[?2004h`; `is_pasting` flag suppresses per-character undo records during paste.
- **Mouse**: `BUTTON1_PRESSED` for click-to-position; `BUTTON4_PRESSED` / `BUTTON5_PRESSED` for scroll wheel. Terminal native selection requires Shift+click (ncurses captures unmodified mouse events).
- **Backup**: When `-b` is set, `save_file()` copies the existing file to `filename.bak` before writing. Checks for existing `.bak` before overwriting.
- **Swap recovery**: On open, qi checks for a `.swp` file and offers recovery.
- **Gutter width**: Scales dynamically with line count (number of digits + 3 for ` │ ` separator). When `gutter_visible = 0`, all width calculations use full `COLS`.
- **Auto-save**: Periodic auto-save with configurable interval; status bar shows countdown.
- **Makefile tab handling**: Tab key inserts a true `\t` character (not spaces) when editing a file whose name contains `Makefile`.

---

## System-Level Changes (Cloud Computer)

The following packages were installed on this persistent VM during initial setup:

- `build-essential` (gcc, make, etc.) — installed for building qi
- `libncurses-dev` — ncurses development headers required by qi

Installed via: `sudo apt-get install -y build-essential libncurses-dev`

---

## Release Workflow

1. Make changes, bump `VERSION` in `qi.c`.
2. Build: `make`
3. Package: `tar -czf qi-X.Y.Z.tar.gz -C /path/to/parent qi/` (extracts to `qi/`)
4. Commit with message format: `X.Y.Z: description of change`
5. Run `tag_releases.sh` to create annotated git tags, then `git push origin --tags`.
