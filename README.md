A Simple text editor written in C

# CCode Editor

**CCode** is a lightweight, terminal-based text editor written in C, developed as a diploma project.

## Features

- Line numbering with visual alignment
- Syntax highlighting for C/C++, Python, and JavaScript
- Search with visual match highlighting and arrow-key navigation through results
- Word-level undo/redo — `Ctrl+Z` undoes whole words, not individual characters
- Visual selection with `Shift+Arrow` keys; cyan highlight shows selected region
- Clipboard operations: `Ctrl+C` copy, `Ctrl+X` cut, `Ctrl+V` paste
- Auto-indentation — `Enter` carries the leading whitespace of the current line
- Mouse click to position cursor
- Config file (`~/.ccoderc`) for persistent settings
- Status bar showing filename, line count, filetype, and cursor position

## Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `Ctrl+S` | Save |
| `Ctrl+Q` | Quit (press 3× if unsaved) |
| `Ctrl+F` | Find |
| `Ctrl+Z` | Undo |
| `Ctrl+Y` | Redo |
| `Ctrl+C` | Copy selection |
| `Ctrl+X` | Cut selection |
| `Ctrl+V` | Paste |
| `Shift+Arrow` | Extend selection |
| `Arrow keys` | Move cursor |
| `Home` / `End` | Start / end of line |
| `Page Up/Down` | Scroll by screen |

## Configuration

Create `~/.ccoderc` to override defaults:

```
# CCode configuration
tab_stop = 4
```

Supported options:

| Key | Default | Description |
|-----|---------|-------------|
| `tab_stop` | `4` | Number of spaces a tab character renders as (1–16) |

## Build Instructions

```bash
gcc -o ccode ccode.c -Wall -Wextra -pedantic -std=c99
```

## Usage

```bash
./ccode [filename]
```
