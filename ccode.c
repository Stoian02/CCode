/**
 * Includes:
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/**
 * Helps:
 * ESC [ Pn ; Pn R
 * Docs - https://vt100.net/docs/vt100-ug/chapter3.html#CPR
 * VT models - https://vt100.net/docs/vt510-rm/DECTCEM.html
 *
 */

/*** Defines ***/
#define CCODE_VERSION "1.0.0"
#define CCODE_QUIT_TIMES 3
#define LINENUM_WIDTH 5

#define CTRL_KEY(k) ((k) & 0x1f)

int g_tab_stop = 4;

enum key_config {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DELETE_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN,
    SHIFT_LEFT,
    SHIFT_RIGHT,
    SHIFT_UP,
    SHIFT_DOWN,
    MOUSE_CLICK,
};

enum highlight_config {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD_1,
    HL_KEYWORD_2,
    HL_STRING,
    HL_NUMBER,
    HL_FIND,
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** Data ***/

struct syntax_config {
    char *filetype;
    char **filematch;
    char **keywords;
    char *sl_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

typedef struct editor_row {
    int index;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *highlight;
    int hl_open_comment;
} editor_row;

struct editor_settings {
    int cursor_x, cursor_y;
    int rx;
    int rowoff, coloff;
    int screenrows, screencols;
    int numrows;
    editor_row *row;
    int dirty;
    char *filename;
    char status_prompt[85];
    time_t status_prompt_time;
    struct syntax_config *syntax;
    struct termios terminal_settings;
    int sel_active;
    int sel_x, sel_y;
    char *clipboard;
    int clipboard_len;
    int mouse_x, mouse_y;
};
struct editor_settings E;

enum undo_type {
    UNDO_INSERT,
    UNDO_DELETE,
    UNDO_SPLIT,
    UNDO_JOIN
};

typedef struct undo_t {
    enum undo_type type;
    int x, y;
    char *text;
    int len;
} undo_t;

#define MAX_UNDO 1000

undo_t undo_stack[MAX_UNDO];
int undo_len = 0;
undo_t redo_stack[MAX_UNDO];
int redo_len = 0;

/* Filetypes */

char *C_HL_types[]  = { ".c", ".h", ".cpp", ".cc", ".php", NULL };
char *PY_HL_types[] = { ".py", NULL };
char *JS_HL_types[] = { ".js", ".mjs", ".jsx", ".ts", ".tsx", NULL };

char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "#define", "#include",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", "size_t|", "ssize_t|", NULL
};

char *PYTHON_HL_keywords[] = {
    "False", "None", "True", "and", "as", "assert", "async", "await",
    "break", "class", "continue", "def", "del", "elif", "else", "except",
    "finally", "for", "from", "global", "if", "import", "in", "is",
    "lambda", "nonlocal", "not", "or", "pass", "raise", "return",
    "try", "while", "with", "yield",
    "int|", "float|", "str|", "bool|", "list|", "dict|", "tuple|",
    "set|", "bytes|", "type|", "len|", "range|", "print|", NULL
};

char *JS_HL_keywords[] = {
    "break", "case", "catch", "continue", "debugger", "default", "delete",
    "do", "else", "finally", "for", "function", "if", "in", "instanceof",
    "new", "return", "switch", "this", "throw", "try", "typeof", "var",
    "void", "while", "with", "class", "const", "enum", "export", "extends",
    "import", "super", "let", "of", "async", "await", "from", "static", "yield",
    "true", "false", "null", "undefined",
    "Array|", "Object|", "String|", "Number|", "Boolean|", "Promise|",
    "Map|", "Set|", "Error|", "Date|", "Math|", "JSON|", "console|", NULL
};

struct syntax_config HLDB[] = {
    { "c",          C_HL_types,  C_HL_keywords,      "//", "/*", "*/", HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS },
    { "python",     PY_HL_types, PYTHON_HL_keywords,  "#",  NULL, NULL, HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS },
    { "javascript", JS_HL_types, JS_HL_keywords,      "//", "/*", "*/", HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS },
};

#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** Prototypes ***/
void set_prompt_message(const char *fmt, ...);
void refresh_screen();
char *get_user_input(char *prompt, void (*callback)(char *, int));
void insert_new_line();
void delete_selection();

/*** Config ***/
void load_config() {
    char *home = getenv("HOME");
    if (!home) return;
    char path[256];
    snprintf(path, sizeof(path), "%s/.ccoderc", home);
    FILE *fp = fopen(path, "r");
    if (!fp) return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        char key[64], val[64];
        if (sscanf(p, "%63[^= \t\n] = %63s", key, val) == 2) {
            if (strcmp(key, "tab_stop") == 0) {
                int ts = atoi(val);
                if (ts >= 1 && ts <= 16) g_tab_stop = ts;
            }
        }
    }
    fclose(fp);
}

/*** Terminal ***/
void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disable_rawmode() {
    write(STDOUT_FILENO, "\x1b[?1000l", 8);
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.terminal_settings) == -1)
        die("tcsetattr");
}

void enable_rawmode() {
    if (tcgetattr(STDIN_FILENO, &E.terminal_settings) == -1)
        die("tcgetattr");
    atexit(disable_rawmode);

    struct termios raw = E.terminal_settings;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int read_keypress() {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
        if (nread == -1 && errno != EAGAIN) die("read");

    if (c == '\x1b') {
        char seq[5];

        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] == 'M') {
                char mb[3];
                if (read(STDIN_FILENO, &mb[0], 1) != 1) return '\x1b';
                if (read(STDIN_FILENO, &mb[1], 1) != 1) return '\x1b';
                if (read(STDIN_FILENO, &mb[2], 1) != 1) return '\x1b';
                int btn = (unsigned char)mb[0] - 32;
                E.mouse_x = (unsigned char)mb[1] - 32 - 1;
                E.mouse_y = (unsigned char)mb[2] - 32 - 1;
                if (btn == 0) return MOUSE_CLICK;
                return '\x1b';
            } else if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                    case '1': return HOME_KEY;
                    case '3': return DELETE_KEY;
                    case '4': return END_KEY;
                    case '5': return PAGE_UP;
                    case '6': return PAGE_DOWN;
                    case '7': return HOME_KEY;
                    case '8': return END_KEY;
                    }
                } else if (seq[2] == ';') {
                    if (read(STDIN_FILENO, &seq[3], 1) != 1) return '\x1b';
                    if (read(STDIN_FILENO, &seq[4], 1) != 1) return '\x1b';
                    if (seq[1] == '1' && seq[3] == '2') {
                        switch (seq[4]) {
                        case 'A': return SHIFT_UP;
                        case 'B': return SHIFT_DOWN;
                        case 'C': return SHIFT_RIGHT;
                        case 'D': return SHIFT_LEFT;
                        }
                    }
                }
            } else {
                switch (seq[1]) {
                case 'A': return ARROW_UP;
                case 'B': return ARROW_DOWN;
                case 'C': return ARROW_RIGHT;
                case 'D': return ARROW_LEFT;
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
            case 'H': return HOME_KEY;
            case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    return c;
}

int get_cursor_position(int *rows, int *cols) {
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int get_windows_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1;
        return get_cursor_position(rows, cols);
    }
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/*** Syntax highlight ***/
int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void update_syntax_highlight(editor_row *row) {
    row->highlight = realloc(row->highlight, row->rsize);
    memset(row->highlight, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) return;

    char **keywords = E.syntax->keywords;
    char *scs = E.syntax->sl_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;

    int prev_separator = 1;
    int in_string = 0;
    int in_comment = (row->index > 0 && E.row[row->index - 1].hl_open_comment);

    int i = 0;
    while (i < row->rsize) {
        char c = row->render[i];
        unsigned char prev_highlight = (i > 0) ? row->highlight[i - 1] : HL_NORMAL;

        if (scs_len && !in_string && !in_comment) {
            if (!strncmp(&row->render[i], scs, scs_len)) {
                memset(&row->highlight[i], HL_COMMENT, row->rsize - i);
                break;
            }
        }

        if (mcs_len && mce_len && !in_string) {
            if (in_comment) {
                row->highlight[i] = HL_MLCOMMENT;
                if (!strncmp(&row->render[i], mce, mce_len)) {
                    memset(&row->highlight[i], HL_MLCOMMENT, mce_len);
                    i += mce_len;
                    in_comment = 0;
                    prev_separator = 1;
                    continue;
                } else {
                    i++;
                    continue;
                }
            } else if (!strncmp(&row->render[i], mcs, mcs_len)) {
                memset(&row->highlight[i], HL_MLCOMMENT, mcs_len);
                i += mcs_len;
                in_comment = 1;
                continue;
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
            if (in_string) {
                row->highlight[i] = HL_STRING;
                if (c == '\\' && i + 1 < row->rsize) {
                    row->highlight[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (c == in_string) in_string = 0;
                i++;
                prev_separator = 1;
                continue;
            } else {
                if (c == '"' || c == '\'') {
                    in_string = c;
                    row->highlight[i] = HL_STRING;
                    i++;
                    continue;
                }
            }
        }

        if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
            if ((isdigit(c) && (prev_separator || prev_highlight == HL_NUMBER)) ||
                (c == '.' && prev_highlight == HL_NUMBER)) {
                row->highlight[i] = HL_NUMBER;
                i++;
                prev_separator = 0;
                continue;
            }
        }

        if (prev_separator) {
            int j;
            for (j = 0; keywords[j]; j++) {
                int klen = strlen(keywords[j]);
                int kw2 = keywords[j][klen - 1] == '|';
                if (kw2) klen--;
                if (!strncmp(&row->render[i], keywords[j], klen) &&
                    is_separator(row->render[i + klen])) {
                    memset(&row->highlight[i], kw2 ? HL_KEYWORD_2 : HL_KEYWORD_1, klen);
                    i += klen;
                    break;
                }
            }
            if (keywords[j] != NULL) {
                prev_separator = 0;
                continue;
            }
        }

        prev_separator = is_separator(c);
        i++;
    }

    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->index + 1 < E.numrows)
        update_syntax_highlight(&E.row[row->index + 1]);
}

int highlight_to_color(int highlight) {
    switch (highlight) {
    case HL_COMMENT:
    case HL_MLCOMMENT: return 90;
    case HL_STRING:    return 92;
    case HL_KEYWORD_1: return 94;
    case HL_KEYWORD_2: return 95;
    case HL_NUMBER:    return 91;
    default:           return 97;
    }
}

void select_highlight() {
    E.syntax = NULL;
    if (E.filename == NULL) return;
    char *ftype = strrchr(E.filename, '.');
    for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
        struct syntax_config *s = &HLDB[j];
        unsigned int i = 0;
        while (s->filematch[i]) {
            int is_type = (s->filematch[i][0] == '.');
            if ((is_type && ftype && !strcmp(ftype, s->filematch[i])) ||
                (!is_type && strstr(E.filename, s->filematch[i]))) {
                E.syntax = s;
                for (int fr = 0; fr < E.numrows; fr++)
                    update_syntax_highlight(&E.row[fr]);
                return;
            }
            i++;
        }
    }
}

/*** Row operations ***/

int row_cx_to_rx(editor_row *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (g_tab_stop - 1) - (rx % g_tab_stop);
        rx++;
    }
    return rx;
}

int row_rx_to_cx(editor_row *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (g_tab_stop - 1) - (cur_rx % g_tab_stop);
        cur_rx++;
        if (cur_rx > rx) return cx;
    }
    return cx;
}

void update_row(editor_row *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (g_tab_stop - 1) + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % g_tab_stop != 0) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
    update_syntax_highlight(row);
}

void insert_row(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(editor_row) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(editor_row) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].index++;

    E.row[at].index = at;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';
    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    E.row[at].highlight = NULL;
    E.row[at].hl_open_comment = 0;
    update_row(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void free_row(editor_row *row) {
    free(row->render);
    free(row->chars);
    free(row->highlight);
}

void delete_row(int at) {
    if (at < 0 || at >= E.numrows) return;
    free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(editor_row) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].index--;
    E.numrows--;
    E.dirty++;
}

void insert_char_in_row(editor_row *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    update_row(row);
    E.dirty++;
}

void row_add_string(editor_row *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += len;
    row->chars[row->size] = '\0';
    update_row(row);
    E.dirty++;
}

void row_delete_char(editor_row *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    update_row(row);
    E.dirty++;
}

/*** Selection ***/

static void sel_range(int *sy, int *sx, int *ey, int *ex) {
    if (E.sel_y < E.cursor_y || (E.sel_y == E.cursor_y && E.sel_x <= E.cursor_x)) {
        *sy = E.sel_y; *sx = E.sel_x;
        *ey = E.cursor_y; *ex = E.cursor_x;
    } else {
        *sy = E.cursor_y; *sx = E.cursor_x;
        *ey = E.sel_y; *ex = E.sel_x;
    }
}

char *get_selection_text(int *len) {
    if (!E.sel_active) { *len = 0; return NULL; }
    int sy, sx, ey, ex;
    sel_range(&sy, &sx, &ey, &ex);

    int total = 0;
    for (int y = sy; y <= ey; y++) {
        int x0 = (y == sy) ? sx : 0;
        int x1 = (y == ey) ? ex : E.row[y].size;
        total += x1 - x0;
        if (y < ey) total++;
    }

    char *buf = malloc(total + 1);
    char *p = buf;
    for (int y = sy; y <= ey; y++) {
        int x0 = (y == sy) ? sx : 0;
        int x1 = (y == ey) ? ex : E.row[y].size;
        memcpy(p, &E.row[y].chars[x0], x1 - x0);
        p += x1 - x0;
        if (y < ey) *p++ = '\n';
    }
    *p = '\0';
    *len = total;
    return buf;
}

void delete_selection() {
    if (!E.sel_active) return;
    int sy, sx, ey, ex;
    sel_range(&sy, &sx, &ey, &ex);
    E.sel_active = 0;
    E.cursor_y = sy;
    E.cursor_x = sx;

    if (sy == ey) {
        int count = ex - sx;
        for (int i = 0; i < count; i++)
            row_delete_char(&E.row[sy], sx);
    } else {
        E.row[sy].size = sx;
        E.row[sy].chars[sx] = '\0';
        update_row(&E.row[sy]);
        row_add_string(&E.row[sy], &E.row[ey].chars[ex], E.row[ey].size - ex);
        for (int y = ey; y > sy; y--)
            delete_row(y);
    }
}

void paste_clipboard() {
    if (!E.clipboard || E.clipboard_len == 0) return;
    for (int i = 0; i < E.clipboard_len; i++) {
        if (E.clipboard[i] == '\n') {
            insert_new_line();
        } else {
            if (E.cursor_y == E.numrows) insert_row(E.numrows, "", 0);
            insert_char_in_row(&E.row[E.cursor_y], E.cursor_x, E.clipboard[i]);
            E.cursor_x++;
        }
    }
}

/*** Editor operations ***/

void undo_operation() {
    if (undo_len == 0) return;

    undo_t op = undo_stack[--undo_len];
    redo_stack[redo_len++] = op;

    E.cursor_x = op.x;
    E.cursor_y = op.y;

    switch (op.type) {
    case UNDO_INSERT:
        for (int i = 0; i < op.len; i++) {
            if (E.cursor_y == E.numrows) insert_row(E.numrows, "", 0);
            insert_char_in_row(&E.row[E.cursor_y], E.cursor_x + i, op.text[i]);
        }
        E.cursor_x += op.len;
        break;

    case UNDO_DELETE:
        for (int i = 0; i < op.len; i++)
            row_delete_char(&E.row[E.cursor_y], E.cursor_x);
        break;

    default:
        break;
    }
}

void redo_operation() {
    if (redo_len == 0) return;

    undo_t op = redo_stack[--redo_len];
    undo_stack[undo_len++] = op;

    E.cursor_x = op.x;
    E.cursor_y = op.y;

    switch (op.type) {
    case UNDO_DELETE:
        for (int i = 0; i < op.len; i++) {
            if (E.cursor_y == E.numrows) insert_row(E.numrows, "", 0);
            insert_char_in_row(&E.row[E.cursor_y], E.cursor_x + i, op.text ? op.text[i] : '?');
        }
        E.cursor_x += op.len;
        break;

    case UNDO_INSERT:
        for (int i = 0; i < op.len; i++)
            row_delete_char(&E.row[E.cursor_y], E.cursor_x);
        break;

    default:
        break;
    }
}

void insert_char(int c) {
    if (E.sel_active) delete_selection();
    if (E.cursor_y == E.numrows) insert_row(E.numrows, "", 0);

    /* Coalesce consecutive non-separator inserts into one undo entry
     * so that Ctrl+Z undoes whole words rather than single characters. */
    int coalesced = 0;
    if (undo_len > 0 && !is_separator(c)) {
        undo_t *last = &undo_stack[undo_len - 1];
        if (last->type == UNDO_DELETE &&
            last->y == E.cursor_y &&
            last->x + last->len == E.cursor_x) {
            last->text = realloc(last->text, last->len + 2);
            last->text[last->len] = c;
            last->text[last->len + 1] = '\0';
            last->len++;
            redo_len = 0;
            coalesced = 1;
        }
    }

    if (!coalesced && undo_len < MAX_UNDO) {
        char *copy = malloc(2);
        copy[0] = c; copy[1] = '\0';
        undo_stack[undo_len++] = (undo_t){ UNDO_DELETE, E.cursor_x, E.cursor_y, copy, 1 };
        redo_len = 0;
    }

    insert_char_in_row(&E.row[E.cursor_y], E.cursor_x, c);
    E.cursor_x++;
}

void insert_new_line() {
    /* Auto-indent: count leading whitespace on the current line. */
    int indent = 0;
    if (E.cursor_y < E.numrows) {
        char *line = E.row[E.cursor_y].chars;
        while (indent < E.row[E.cursor_y].size &&
               (line[indent] == ' ' || line[indent] == '\t'))
            indent++;
        if (indent > E.cursor_x) indent = E.cursor_x;
    }

    if (E.cursor_x == 0) {
        insert_row(E.cursor_y, "", 0);
    } else {
        editor_row *row = &E.row[E.cursor_y];
        insert_row(E.cursor_y + 1, &row->chars[E.cursor_x], row->size - E.cursor_x);
        row = &E.row[E.cursor_y];
        row->size = E.cursor_x;
        row->chars = realloc(row->chars, row->size + 1);
        row->chars[row->size] = '\0';
        update_row(row);
    }
    E.cursor_y++;
    E.cursor_x = 0;

    /* Insert the same leading whitespace as the line above. */
    if (indent > 0 && E.cursor_y < E.numrows) {
        char *prev_line = E.row[E.cursor_y - 1].chars;
        for (int i = 0; i < indent; i++) {
            insert_char_in_row(&E.row[E.cursor_y], E.cursor_x, prev_line[i]);
            E.cursor_x++;
        }
    }
}

void delete_char() {
    if (E.sel_active) { delete_selection(); return; }
    if (E.cursor_y == E.numrows) return;
    if (E.cursor_x == 0 && E.cursor_y == 0) return;

    editor_row *row = &E.row[E.cursor_y];
    if (E.cursor_x > 0) {
        char deleted = row->chars[E.cursor_x - 1];
        char *copy = malloc(2);
        copy[0] = deleted; copy[1] = '\0';
        if (undo_len < MAX_UNDO) {
            undo_stack[undo_len++] = (undo_t){ UNDO_INSERT, E.cursor_x - 1, E.cursor_y, copy, 1 };
            redo_len = 0;
        }
        row_delete_char(row, E.cursor_x - 1);
        E.cursor_x--;
    } else {
        E.cursor_x = E.row[E.cursor_y - 1].size;
        row_add_string(&E.row[E.cursor_y - 1], row->chars, row->size);
        delete_row(E.cursor_y);
        E.cursor_y--;
    }
}

/*** File I/O ***/

char *rows_to_string(int *buflen) {
    int totallen = 0;
    for (int j = 0; j < E.numrows; j++)
        totallen += E.row[j].size + 1;
    *buflen = totallen;

    char *buf = malloc(totallen);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void open_editor(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);
    select_highlight();

    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;

    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
            linelen--;
        insert_row(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

void save() {
    if (E.filename == NULL) {
        E.filename = get_user_input("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            set_prompt_message("Save aborted");
            return;
        }
        select_highlight();
    }

    int len;
    char *buf = rows_to_string(&len);

    int file = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (file != -1) {
        if (ftruncate(file, len) != -1) {
            if (write(file, buf, len) == len) {
                close(file);
                free(buf);
                E.dirty = 0;
                set_prompt_message("%d bytes written to disk", len);
                return;
            }
        }
        close(file);
    }
    free(buf);
    set_prompt_message("Can't save! I/O error: %s", strerror(errno));
}

/*** Find ***/
void find_callback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;
    static int saved_highlight_row;
    static char *saved_highlight = NULL;

    if (saved_highlight) {
        memcpy(E.row[saved_highlight_row].highlight, saved_highlight,
               E.row[saved_highlight_row].rsize);
        free(saved_highlight);
        saved_highlight = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;

    for (int i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) current = E.numrows - 1;
        else if (current == E.numrows) current = 0;

        editor_row *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cursor_y = current;
            E.cursor_x = row_rx_to_cx(row, match - row->render);
            E.rowoff = E.numrows;

            saved_highlight_row = current;
            saved_highlight = malloc(row->rsize);
            memcpy(saved_highlight, row->highlight, row->rsize);
            memset(&row->highlight[match - row->render], HL_FIND, strlen(query));
            break;
        }
    }
}

void find() {
    int saved_cursor_x = E.cursor_x;
    int saved_cursor_y = E.cursor_y;
    int saved_coloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = get_user_input("Search: %s (ESC/Arrows/Enter)", find_callback);

    if (query) {
        free(query);
    } else {
        E.cursor_x = saved_cursor_x;
        E.cursor_y = saved_cursor_y;
        E.coloff = saved_coloff;
        E.rowoff = saved_rowoff;
    }
}

/*** Append buffer ***/
struct abuf {
    char *b;
    int len;
};
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (new == NULL) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** Output ***/

void scroll() {
    E.rx = 0;
    if (E.cursor_y < E.numrows)
        E.rx = row_cx_to_rx(&E.row[E.cursor_y], E.cursor_x);

    if (E.cursor_y < E.rowoff) E.rowoff = E.cursor_y;
    if (E.cursor_y >= E.rowoff + E.screenrows)
        E.rowoff = E.cursor_y - E.screenrows + 1;
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols)
        E.coloff = E.rx - E.screencols + 1;
}

void draw_rows(struct abuf *ab) {
    for (int i = 0; i < E.screenrows; i++) {
        int fileditor_row = i + E.rowoff;

        if (fileditor_row < E.numrows) {
            char linenum[16];
            snprintf(linenum, sizeof(linenum), "%4d ", fileditor_row + 1);
            abAppend(ab, "\x1b[90m", 5);
            abAppend(ab, linenum, strlen(linenum));
            abAppend(ab, "\x1b[39m", 5);
        } else {
            abAppend(ab, "     ", LINENUM_WIDTH);
        }

        if (fileditor_row >= E.numrows) {
            if (E.numrows == 0 && i == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                          "CCode editor -- version %s", CCODE_VERSION);
                if (welcomelen > E.screencols) welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding) { abAppend(ab, "-", 1); padding--; }
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            } else {
                abAppend(ab, "-", 1);
            }
        } else {
            int len = E.row[fileditor_row].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;

            char *c = &E.row[fileditor_row].render[E.coloff];
            unsigned char *hl = &E.row[fileditor_row].highlight[E.coloff];
            int current_color = -1;
            int in_sel_bg = 0;

            /* Precompute selection render range for this row. */
            int sel_start_rx = -1, sel_end_rx = -1;
            if (E.sel_active) {
                int sy, sx, ey, ex;
                sel_range(&sy, &sx, &ey, &ex);
                if (fileditor_row >= sy && fileditor_row <= ey) {
                    sel_start_rx = (fileditor_row == sy)
                        ? row_cx_to_rx(&E.row[fileditor_row], sx) : 0;
                    sel_end_rx = (fileditor_row == ey)
                        ? row_cx_to_rx(&E.row[fileditor_row], ex)
                        : E.row[fileditor_row].rsize;
                }
            }

            for (int j = 0; j < len; j++) {
                int abs_j = j + E.coloff;
                int want_sel = (sel_start_rx >= 0 &&
                                abs_j >= sel_start_rx &&
                                abs_j < sel_end_rx &&
                                hl[j] != HL_FIND);

                if (want_sel != in_sel_bg) {
                    abAppend(ab, want_sel ? "\x1b[46m" : "\x1b[49m", 5);
                    in_sel_bg = want_sel;
                }

                if (hl[j] == HL_FIND) {
                    if (in_sel_bg) { abAppend(ab, "\x1b[49m", 5); in_sel_bg = 0; }
                    abAppend(ab, "\x1b[43m\x1b[30m", 10);
                    abAppend(ab, &c[j], 1);
                    abAppend(ab, "\x1b[49m\x1b[39m", 10);
                    current_color = -1;
                    continue;
                }

                if (iscntrl(c[j])) {
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                } else if (hl[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = highlight_to_color(hl[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            if (in_sel_bg) abAppend(ab, "\x1b[49m", 5);
            abAppend(ab, "\x1b[39m", 5);
        }
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void draw_status_bar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
        E.syntax ? E.syntax->filetype : "no ft", E.cursor_y + 1, E.numrows);

    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abAppend(ab, rstatus, rlen);
            break;
        }
        abAppend(ab, " ", 1);
        len++;
    }

    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

void draw_prompt_bar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.status_prompt);
    if (msglen > E.screencols) msglen = E.screencols;
    if (msglen && time(NULL) - E.status_prompt_time < 5)
        abAppend(ab, E.status_prompt, msglen);
}

void refresh_screen() {
    scroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    draw_rows(&ab);
    draw_status_bar(&ab);
    draw_prompt_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH",
             (E.cursor_y - E.rowoff) + 1,
             (E.rx - E.coloff) + 1 + LINENUM_WIDTH);
    abAppend(&ab, buf, strlen(buf));
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

void set_prompt_message(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.status_prompt, sizeof(E.status_prompt), fmt, ap);
    va_end(ap);
    E.status_prompt_time = time(NULL);
}

/*** Input ***/
char *get_user_input(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);
    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        set_prompt_message(prompt, buf);
        refresh_screen();

        int c = read_keypress();
        if (c == DELETE_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            set_prompt_message("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                set_prompt_message("");
                if (callback) callback(buf, c);
                return buf;
            }
        } else if (!iscntrl(c) && c < 128) {
            if (buflen == bufsize - 1) {
                bufsize *= 2;
                buf = realloc(buf, bufsize);
            }
            buf[buflen++] = c;
            buf[buflen] = '\0';
        }
        if (callback) callback(buf, c);
    }
}

void move_cursor(int key) {
    editor_row *row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];

    switch (key) {
    case ARROW_LEFT:
        if (E.cursor_x != 0) {
            E.cursor_x--;
        } else if (E.cursor_y > 0) {
            E.cursor_y--;
            E.cursor_x = E.row[E.cursor_y].size;
        }
        break;
    case ARROW_RIGHT:
        if (row && E.cursor_x < row->size) {
            E.cursor_x++;
        } else if (row && E.cursor_x == row->size) {
            E.cursor_y++;
            E.cursor_x = 0;
        }
        break;
    case ARROW_UP:
        if (E.cursor_y != 0) E.cursor_y--;
        break;
    case ARROW_DOWN:
        if (E.cursor_y < E.numrows - 1) E.cursor_y++;
        break;
    }

    row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
    int rowlen = row ? row->size : 0;
    if (E.cursor_x > rowlen) E.cursor_x = rowlen;
}

void process_keypress() {
    static int quit_times = CCODE_QUIT_TIMES;

    int c = read_keypress();

    switch (c) {
    case '\r':
        insert_new_line();
        break;

    case CTRL_KEY('q'):
        if (E.dirty && quit_times > 0) {
            set_prompt_message("Warning!!! File was not saved! "
                "Press Ctrl-Q %d more times to quit.", quit_times);
            quit_times--;
            return;
        }
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);

    case CTRL_KEY('s'):
        save();
        break;

    case CTRL_KEY('z'):
        undo_operation();
        break;

    case CTRL_KEY('y'):
        redo_operation();
        break;

    case CTRL_KEY('c'):
        if (E.sel_active) {
            free(E.clipboard);
            E.clipboard = get_selection_text(&E.clipboard_len);
            set_prompt_message("Copied %d chars", E.clipboard_len);
        }
        break;

    case CTRL_KEY('x'):
        if (E.sel_active) {
            free(E.clipboard);
            E.clipboard = get_selection_text(&E.clipboard_len);
            delete_selection();
            set_prompt_message("Cut %d chars", E.clipboard_len);
        }
        break;

    case CTRL_KEY('v'):
        paste_clipboard();
        break;

    case HOME_KEY:
        E.cursor_x = 0;
        E.sel_active = 0;
        break;

    case END_KEY:
        if (E.cursor_y < E.numrows) E.cursor_x = E.row[E.cursor_y].size;
        E.sel_active = 0;
        break;

    case CTRL_KEY('f'):
        find();
        break;

    case BACKSPACE:
    case CTRL_KEY('h'):
    case DELETE_KEY:
        if (c == DELETE_KEY) move_cursor(ARROW_RIGHT);
        delete_char();
        break;

    case PAGE_UP:
    case PAGE_DOWN: {
        if (c == PAGE_UP) {
            E.cursor_y = E.rowoff;
        } else {
            E.cursor_y = E.rowoff + E.screenrows - 1;
            if (E.cursor_y > E.numrows) E.cursor_y = E.numrows;
        }
        int times = E.screenrows;
        while (times--)
            move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        E.sel_active = 0;
        break;
    }

    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        E.sel_active = 0;
        move_cursor(c);
        break;

    case SHIFT_LEFT:
    case SHIFT_RIGHT:
    case SHIFT_UP:
    case SHIFT_DOWN:
        if (!E.sel_active) {
            E.sel_active = 1;
            E.sel_x = E.cursor_x;
            E.sel_y = E.cursor_y;
        }
        move_cursor(c == SHIFT_LEFT  ? ARROW_LEFT  :
                    c == SHIFT_RIGHT ? ARROW_RIGHT :
                    c == SHIFT_UP    ? ARROW_UP    : ARROW_DOWN);
        break;

    case MOUSE_CLICK: {
        int row = E.mouse_y + E.rowoff;
        int col = E.mouse_x - LINENUM_WIDTH;
        if (col < 0) col = 0;
        if (row >= E.numrows) row = E.numrows > 0 ? E.numrows - 1 : 0;
        E.cursor_y = row;
        if (E.numrows > 0)
            E.cursor_x = row_rx_to_cx(&E.row[E.cursor_y], col + E.coloff);
        E.sel_active = 0;
        break;
    }

    case CTRL_KEY('l'):
    case '\x1b':
        break;

    default:
        insert_char(c);
        break;
    }

    quit_times = CCODE_QUIT_TIMES;
}

/*** Init ***/

void init() {
    load_config();

    E.cursor_x = 0;
    E.cursor_y = 0;
    E.rx = 0;
    E.rowoff = 0;
    E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.status_prompt[0] = '\0';
    E.status_prompt_time = 0;
    E.syntax = NULL;
    E.sel_active = 0;
    E.sel_x = 0;
    E.sel_y = 0;
    E.clipboard = NULL;
    E.clipboard_len = 0;
    E.mouse_x = 0;
    E.mouse_y = 0;

    if (get_windows_size(&E.screenrows, &E.screencols) == -1) die("get_windows_size");
    E.screenrows -= 2;

    write(STDOUT_FILENO, "\x1b[?1000h", 8);
}

int main(int argc, char *argv[]) {
    enable_rawmode();
    init();
    if (argc >= 2) open_editor(argv[1]);

    set_prompt_message(
        "HELP: ^S=save ^Q=quit ^F=find ^Z=undo ^Y=redo ^C=copy ^X=cut ^V=paste");

    while (1) {
        refresh_screen();
        process_keypress();
    }
    return 0;
}
