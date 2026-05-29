# LSP Integration Plan

## Overview

LSP (Language Server Protocol) is a JSON-RPC protocol that lets an editor
communicate with a language-specific server process (clangd, pylsp,
typescript-language-server) to get diagnostics, completions, hover info,
and go-to-definition. The editor sends requests and notifications; the server
responds and can also push unsolicited notifications (e.g. diagnostics).

Target features in priority order:
1. **Diagnostics** — errors/warnings shown in the gutter and status bar
2. **Completion** — popup list triggered on typing or `Ctrl+Space`
3. **Hover** — type/documentation shown on `Ctrl+K`
4. **Go to definition** — jump to symbol source on `Ctrl+]`

---

## Architecture

### The core problem: async responses

The current event loop is:
```c
while (1) {
    refresh_screen();
    process_keypress();   // blocks up to 100 ms (VTIME=1)
}
```

LSP responses arrive from a subprocess pipe at any time, independently of
keypresses. We must multiplex two file descriptors: `STDIN_FILENO` (keypresses)
and the read end of the LSP pipe.

**Solution — `poll()` in `read_keypress()`:**

Replace the inner `read()` retry loop with `poll()`:

```c
struct pollfd fds[2];
fds[0].fd = STDIN_FILENO;   fds[0].events = POLLIN;
fds[1].fd = lsp.read_fd;    fds[1].events = POLLIN;

int n = poll(fds, lsp.running ? 2 : 1, 100 /*ms*/);

if (n > 0 && (fds[1].revents & POLLIN))
    lsp_process_incoming();           // drain server output
if (n > 0 && (fds[0].revents & POLLIN))
    /* read and return the keypress */
```

This keeps the 100 ms cadence the editor already has (VTIME=1) and is
non-breaking: if LSP is not running, `poll()` degrades to a plain timeout
on stdin — identical to today.

---

## Components

### 1. JSON-RPC Transport (`lsp_rpc.c`)

LSP messages are framed with an HTTP-style header:
```
Content-Length: 97\r\n
\r\n
{"jsonrpc":"2.0","id":1,"method":"initialize","params":{...}}
```

**Sending:** A small `sb_*` (string builder) helper builds JSON with
`sprintf`-style calls, then `lsp_send()` writes the header + body to the
server's stdin pipe.

**Receiving:** `lsp_read_message()` reads `Content-Length`, allocates a
buffer, reads exactly that many bytes, then passes the raw JSON string to
the dispatcher.

**JSON parsing:** Embed [jsmn](https://github.com/zserge/jsmn) — a
single-file, zero-allocation tokenizer (~500 lines). It produces an array
of `jsmntok_t` tokens that index into the raw buffer; no heap allocation.

### 2. Process Management (`lsp_proc.c`)

```c
typedef struct {
    int    running;
    pid_t  pid;
    int    write_fd;    // editor → server
    int    read_fd;     // server → editor
    int    initialized;
    int    next_id;
} lsp_proc;
```

`lsp_start(const char *argv[])` — `fork()` + two `pipe()` pairs + `execvp()`.
The child redirects its stdin/stdout to the pipes.

`lsp_stop()` — sends `shutdown` + `exit`, then `waitpid()`. Called from
`disable_rawmode()` so it runs on any exit path.

Server selection is driven by the filetype already detected by `select_highlight()`:

| Filetype  | Server binary            |
|-----------|--------------------------|
| c / c++   | `clangd`                 |
| python    | `pylsp` or `pyright-langserver` |
| javascript / typescript | `typescript-language-server --stdio` |

If the binary is not found, LSP is silently skipped for that file — the
editor continues to work normally.

### 3. LSP Lifecycle

```
editor                          server
  │── initialize ──────────────►│
  │◄── result (capabilities) ───│
  │── initialized (notif.) ────►│
  │── textDocument/didOpen ────►│
  │                             │
  │  [on every edit:]           │
  │── textDocument/didChange ──►│
  │◄── publishDiagnostics ──────│  (async, any time)
  │                             │
  │── shutdown ────────────────►│
  │◄── result ──────────────────│
  │── exit (notif.) ───────────►│
```

Document sync uses `TextDocumentSyncKind.Full` (kind 1): the entire file
content is sent on every `didChange`. This is simpler than incremental and
perfectly fine at this scale.

**URI encoding:** LSP addresses files as `file:///absolute/path`. A small
helper `path_to_uri()` prefixes the absolute path.

**UTF-16 positions:** LSP specifies line/character offsets in UTF-16 code
units. For pure ASCII files (the common case for C) these equal byte
offsets. A `utf16_offset()` conversion function handles the general case.

### 4. Diagnostics

**Storage:** a flat array attached to the global state:

```c
typedef struct {
    int  line;           // 0-indexed
    int  col;
    int  severity;       // 1=error 2=warning 3=info 4=hint
    char message[256];
} lsp_diag;

lsp_diag *diagnostics;
int       num_diagnostics;
```

Cleared and repopulated on every `publishDiagnostics` notification.

**Gutter indicator:** Expand `LINENUM_WIDTH` from 5 to 6, adding a
one-character severity column before the line number:

```
      1  #include <stdio.h>
E     2  int man() {          // error on this line
      3      return 0;
W     4  }                    // warning on this line
```

Colors: `E` in red (91), `W` in yellow (93), space otherwise.

**Status bar message:** When the cursor sits on a line that has a
diagnostic, show the message in the prompt bar automatically (replacing
the help hint). It disappears when the cursor moves away.

**Gutter highlight:** Optionally underline characters in the diagnostic
range using `\x1b[4m` (underline) with a red or yellow foreground.

### 5. Completion

**Trigger conditions:**
- Automatically after typing `.`, `->`, `::`, or `(`
- Manually with `Ctrl+Space`

**Request:** `textDocument/completion` with cursor position.

**UI — popup overlay:**

```
  int result = my_func|
               ┌──────────────────────┐
               │ my_function(a, b)    │  ← selected (reverse video)
               │ my_func_other()      │
               │ my_func_2(x)         │
               └──────────────────────┘
```

Drawn in `draw_rows()` as an overlay after the normal row content.
Maximum 8 items visible; scrollable.

**Navigation:**
- `Tab` / `↓` — next item
- `Shift+Tab` / `↑` — previous item
- `Enter` — confirm (replace the partial word before cursor)
- `Esc` or any non-nav key — dismiss

**State:**

```c
typedef struct {
    int    active;
    char **items;         // label strings
    int    count;
    int    selected;
    int    anchor_x, anchor_y;   // cursor position when triggered
    char   prefix[64];           // text already typed
} lsp_completion;
```

### 6. Hover

**Trigger:** `Ctrl+K`

**Request:** `textDocument/hover` with cursor position.

**Display:** The `contents` field (MarkupContent or string) is stripped of
Markdown and shown as a single line in the prompt bar. This reuses the
existing 5-second message system — no new UI needed.

Example:
```
[prompt bar]  int my_function(int a, int b)  — myheader.h:12
```

### 7. Go to Definition

**Trigger:** `Ctrl+]`

**Request:** `textDocument/definition`

**Response handling:**
1. Parse the returned `Location` (uri + range).
2. Convert `file://` URI back to a path.
3. If it is the current file, move cursor to the target line/col.
4. If it is a different file, call `open_editor()` with the new path (this
   replaces the current buffer — a "jump" model, no split panes yet).
5. Push the original position onto a small jump stack so `Ctrl+O` can
   return.

---

## Data Structures Summary

Add a new top-level `lsp_state` global (parallel to `E`):

```c
struct lsp_state {
    /* process */
    int       running;
    pid_t     pid;
    int       write_fd, read_fd;
    int       initialized;
    int       next_id;

    /* diagnostics */
    lsp_diag *diags;
    int       num_diags;

    /* completion */
    lsp_completion comp;

    /* jump stack (go-to-def / back) */
    struct { char path[512]; int x, y; } jump_stack[32];
    int jump_top;
};
struct lsp_state LSP;
```

---

## Implementation Phases

### Phase 1 — Foundation (no visible change to the user)
- [ ] Embed jsmn header
- [ ] `lsp_rpc.c`: string builder, `lsp_send()`, `lsp_read_message()`
- [ ] `lsp_proc.c`: `lsp_start()`, `lsp_stop()`
- [ ] `poll()`-based event loop in `read_keypress()`
- [ ] LSP lifecycle: `initialize` → `initialized` → `shutdown` → `exit`
- [ ] `textDocument/didOpen` and `textDocument/didChange`

### Phase 2 — Diagnostics
- [ ] Dispatcher for `textDocument/publishDiagnostics`
- [ ] Parse severity, line, col, message from JSON
- [ ] Expand gutter to 6 cols, add `E`/`W` indicator
- [ ] Prompt bar shows diagnostic when cursor is on that line

### Phase 3 — Completion
- [ ] Auto-trigger logic (`.`, `->`, `::`, Ctrl+Space)
- [ ] `textDocument/completion` request + response parser
- [ ] Popup rendering in `draw_rows()` overlay
- [ ] Tab/Enter/Esc handling in `process_keypress()`
- [ ] Word replacement on confirm

### Phase 4 — Hover & Go to Definition
- [ ] `Ctrl+K` → `textDocument/hover` → prompt bar
- [ ] `Ctrl+]` → `textDocument/definition` → jump
- [ ] Jump stack + `Ctrl+O` to go back

---

## File Layout

Rather than growing `ccode.c` further, split into:

```
ccode.c          — main editor logic (current file, refactored)
lsp.h            — shared types and the LSP global declaration
lsp_rpc.c        — JSON-RPC framing + jsmn wrapper
lsp_proc.c       — process spawn/stop + lifecycle messages
lsp_features.c   — diagnostics, completion, hover, definition handlers
jsmn.h           — embedded jsmn (vendored, unmodified)
```

The Makefile links all `.c` files together:
```make
SRCS = ccode.c lsp_rpc.c lsp_proc.c lsp_features.c
```

---

## Known Limitations & Future Work

| Limitation | Notes |
|------------|-------|
| UTF-16 offsets | Conversion helper provided; edge cases with multi-byte chars need testing |
| Single buffer | Go-to-def in another file replaces the current buffer; no tabs/splits yet |
| Full document sync | Sends entire file on every keystroke; fine up to ~10k lines |
| No `workspace/symbol` | Global symbol search not planned for now |
| No code actions | `Ctrl+.` / quick-fix not in scope for initial version |
| ASCII-only completion filter | Prefix matching is byte-level for now |
| X10 mouse in completion | Clicking popup items not supported; keyboard only |
