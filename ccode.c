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

/*** * Defines: ***/
#define CCODE_VERSION "1.0.0"
#define CCODE_TAB_STOP 4
#define CCODE_QUIT_TIMES 3
#define LINENUM_WIDTH 5

#define CTRL_KEY(k) ((k) & 0x1f)

enum key_config
{
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DELETE_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum highlight_config {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MLCOMMENT,
    HL_KEYWORD_1,
    HL_KEYWORD_2,
    HL_STRING,
    HL_NUMBER,
    HL_FIND
};

// Bit flags for syntax highlighting
#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*** Data: ***/

struct syntax_config {
    char *filetype;
    char **filematch;
    char **keywords;
    char *sl_comment_start;
    char *multiline_comment_start;
    char *multiline_comment_end;
    int flags;
};

// editor row
typedef struct editor_row {
    int index;
    int size;
    int rsize;
    char *chars;
    char *render;
    unsigned char *highlight;
    int hl_open_comment; // highlight_
} editor_row;

struct editor_settings
{
    int cursor_x, cursor_y;
    int rx; // index into render field
    int rowoff; // row offset
    int coloff; // column offset
    int screenrows;
    int screencols;
    int numrows;
    editor_row *row;
    int dirty; // If file has been modified since opening or saving
    char *filename;
    char status_prompt[85];
    time_t status_prompt_time;
    struct syntax_config *syntax;
    struct termios terminal_settings;
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

char *C_HL_types[] = { ".c", ".h", ".cpp", ".php", ".js", ".py", NULL };
// The two types of keywords are separated with a | (pipe)
char *C_HL_keywords[] = {
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case", "define",
    "#define", "include", "#include",

    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", "var|", NULL
};

// Highlight DataBase
struct syntax_config HLDB[] = {
    {
        "c",
        C_HL_types,
        C_HL_keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    }
};

// Stores the length of HLDB array
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** Prototypes ***/

void set_prompt_message(const char *fmt, ...);
void refresh_screen();
char *get_user_input(char *prompt, void (*callback)(char *, int));

/*** Terminal ***/
void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2j]", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disable_rawmode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.terminal_settings) == -1)
        die("tcsetattr");
}

void enable_rawmode()
{
    if (tcgetattr(STDIN_FILENO, &E.terminal_settings) == -1)
        die("tcgetattr");
    atexit(disable_rawmode);

    struct termios raw = E.terminal_settings;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_iflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int read_keypress()
{
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) != 1)
        if (nread == -1 && errno != EAGAIN)
            die("read");

    if (c == '\x1b')
    {
        char seq[3];

        if (read(STDIN_FILENO, &seq[0], 1) != 1)
            return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1)
            return '\x1b';

        if (seq[0] == '[')
        {
            if (seq[1] >= '0' && seq[1] <= '9')
            {
                if (read(STDIN_FILENO, &seq[2], 1) != 1)
                    return '\x1b';
                if (seq[2] == '~')
                {
                    switch (seq[1])
                    {
                    case '1':
                        return HOME_KEY;
                    case '3':
                        return DELETE_KEY;
                    case '4':
                        return END_KEY;
                    case '5':
                        return PAGE_UP;
                    case '6':
                        return PAGE_DOWN;
                    case '7':
                        return HOME_KEY;
                    case '8':
                        return END_KEY;
                    }
                }
            }
            else
            {
                switch (seq[1])
                {
                case 'A':
                    return ARROW_UP;
                case 'B':
                    return ARROW_DOWN;
                case 'C':
                    return ARROW_RIGHT;
                case 'D':
                    return ARROW_LEFT;
                case 'H':
                    return HOME_KEY;
                case 'F':
                    return END_KEY;
                }
            }
        }
        else if (seq[0] == 'O')
        {
            switch (seq[1])
            {
            case 'H':
                return HOME_KEY;
            case 'F':
                return END_KEY;
            }
        }
        return '\x1b';
    }
    else
    {
        return c;
    }
}

int get_cursor_position(int *rows, int *cols)
{
    char buf[32];
    unsigned int i = 0;

    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1)
    {
        if (read(STDIN_FILENO, &buf[i], 1) != 1)
        {
            break;
        }
        if (buf[i] == 'R')
        {
            break;
        }
        i++;
    }
    buf[i] = '\0';

    if (buf[0] != '\x1b' || buf[1] != '[')
        return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
        return -1;
    return 0;
}

int get_windows_size(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        // Moves the cursor to the bottom right corner of the screen
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return get_cursor_position(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** syntax highlight ***/
int is_separator(int c) {
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void update_syntax_highlight(editor_row *row) {
    row->highlight = realloc(row->highlight, row->rsize);
    memset(row->highlight, HL_NORMAL, row->rsize);

    if (E.syntax == NULL) return;

    char **keywords = E.syntax->keywords;

    // scs - singleline comment start
    // mcs - multiline comment start
    // mce - multiline comment end
    char *scs = E.syntax->sl_comment_start;
    char *mcs = E.syntax->multiline_comment_start;
    char *mce = E.syntax->multiline_comment_end;

    int scs_len = scs ? strlen(scs) : 0;
    int mcs_len = mcs ? strlen(mcs) : 0;
    int mce_len = mce ? strlen(mce) : 0;


    int prev_separator = 1;
    int in_string = 0;
    // Initialize to true if the previous row has an unclosed multi-line comment
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
            if ((isdigit(c) && (prev_separator || prev_highlight == HL_NUMBER)) || (c == '.' && prev_highlight == HL_NUMBER)) {
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

    /** Set current row's hl_highlight_comment to whatever state in_comment got left
     * in after processing the entire row. That tells us whether the row ended as an
     * unclosed multi-line comment or not.
     */
    int changed = (row->hl_open_comment != in_comment);
    row->hl_open_comment = in_comment;
    if (changed && row->index + 1 < E.numrows) {
        update_syntax_highlight(&E.row[row->index + 1]);
    }
}

/** returns the color code(foreground), reference:
 * https://en.wikipedia.org/wiki/ANSI_escape_code#Select_Graphic_Rendition_parameters
 */
int highlight_to_color(int highlight) {
    switch (highlight) {
        case HL_COMMENT:
        case HL_MLCOMMENT: return 90;
        case HL_STRING:     return 92;
        case HL_KEYWORD_1:  return 94;
        case HL_KEYWORD_2:  return 95;
        case HL_NUMBER:     return 91;
        case HL_FIND:       return 1000;
        default:            return 97;
    }
}

// Will try to match current filename to one of the filematch fields in HLDB
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

                int fileditor_row ;
                for (fileditor_row = 0; fileditor_row < E.numrows; fileditor_row++) {
                    update_syntax_highlight(&E.row[fileditor_row]);
                }

                return;
            }
            i++;
        }
    }
}

/*** row operations ***/

int row_cx_to_rx(editor_row *row, int cx) {
    int rx = 0;
    int j;
    for (j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (CCODE_TAB_STOP - 1) - (rx % CCODE_TAB_STOP);
        rx++;
    }
    return rx;
}

// Convert the render index into a chars index
int row_rx_to_cx(editor_row *row, int rx) {
    int cur_rx = 0;
    int cx;
    for (cx = 0; cx < row->size; cx++) {
        if (row->chars[cx] == '\t')
            cur_rx += (CCODE_TAB_STOP - 1) - (cur_rx % CCODE_TAB_STOP);
        cur_rx++;

        if (cur_rx > rx) return cx;
    }
    return cx;
}

void update_row(editor_row *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            tabs++;
        }
    }

    free(row->render);
    row->render = malloc(row->size + tabs*(CCODE_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % CCODE_TAB_STOP != 0) row->render[idx++] = ' ';
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
    for (int j = at + 1; j <= E.numrows; j++) {
        E.row[j].index++;
    }

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

/**
 * Deletes a row at the specified index.
 * First we validate the at index. Then we free the memory owned by the row
 * using free_row(). We then use memmove() to overwrite the deleted row
 * struct with the rest of the rows that come after it, and decrement numrows.
 * Finally, we increment E.dirty.
 *
 * @param int at The index of the row to delete.
 */
void delete_row(int at) {
    if (at < 0 || at >= E.numrows) return;

    free_row(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(editor_row) * (E.numrows - at - 1));

    for (int j = at; j < E.numrows - 1; j++) {
        E.row[j].index--;
    }

    E.numrows--;
    E.dirty++;
}

/**
 * Inserts a character into a row at the specified position.
 *
 * This function uses memmove() to safely handle overlapping source and
 * destination arrays. It validates the insertion index, allowing insertion
 * at the end of the string, then allocates additional memory for the
 * character and the null terminator, shifts existing chars to make
 * room for the new char, and updates the row size. Finally, it assigns
 * the character to the specified position and updates the row's render and
 * size fields.
 *
 * @param editor_row The row where the character will be inserted.
 * @param at The index at which to insert the character.
 * @param c The character to be inserted.
 */
void insert_char_in_row(editor_row *row, int at, int c) {
    if (at < 0 || at > row->size)
        at = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    update_row(row);
    E.dirty++;
}

/**
 * Appends a string to a row.
 *
 * This function reallocates memory for the row's chars field, copying the
 * specified string to the end of the row's existing chars, updating the
 * row's size, and adding a null terminator. It then updates the row's
 * render and size fields and increments dirty flag.
 *
 * @param row The row to which the string will be appended.
 * @param s The string to append.
 * @param len The length of the string to append.
 */
void row_add_string(editor_row *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);

    row->size += len;
    row->chars[row->size] = '\0';

    update_row(row);
    E.dirty++;
}

/**
 * Deletes a character from a row at the specified position.
 *
 * This function validates the deletion index, allowing deletion at the
 * end of the string, then uses memmove() to safely handle overlapping
 * source and destination arrays. It shifts existing chars to overwrite
 * the deleted char, decrements the row size, and updates the row's render
 * and size fields.
 *
 * @param row The row from which to delete the character.
 * @param at The index of the character to delete.
 */
void row_delete_char(editor_row *row, int at) {
    if (at < 0 || at >= row->size) return;

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    update_row(row);
    E.dirty++;
}

/*** editor operations ***/

void undo_operation() {
    if (undo_len == 0) return;

    undo_t op = undo_stack[--undo_len];
    redo_stack[redo_len++] = op;

    E.cursor_x = op.x;
    E.cursor_y = op.y;

    switch (op.type) {
        case UNDO_INSERT:
            for (int i = 0; i < op.len; i++) {
                if (E.cursor_y == E.numrows)
                    insert_row(E.numrows, "", 0);
                insert_char_in_row(&E.row[E.cursor_y], E.cursor_x + i, op.text[i]);
            }
            E.cursor_x += op.len;
            break;

        case UNDO_DELETE:
            for (int i = 0; i < op.len; i++) {
                row_delete_char(&E.row[E.cursor_y], E.cursor_x);
            }
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
                if (E.cursor_y == E.numrows)
                    insert_row(E.numrows, "", 0);
                insert_char_in_row(&E.row[E.cursor_y], E.cursor_x + i, op.text ? op.text[i] : '?');
            }
            E.cursor_x += op.len;
            break;

        case UNDO_INSERT:
            for (int i = 0; i < op.len; i++) {
                row_delete_char(&E.row[E.cursor_y], E.cursor_x);
            }
            break;

        default:
            break;
    }
}

void insert_char(int c) {
    if(E.cursor_y == E.numrows)
        insert_row(E.numrows, "", 0);

    char *copy = malloc(2);
    copy[0] = c;
    copy[1] = '\0';

    // UNDO for insert: we store DELETE at current position
    if (undo_len < MAX_UNDO) {
        undo_stack[undo_len++] = (undo_t){ UNDO_DELETE, E.cursor_x, E.cursor_y, copy, 1 };
        redo_len = 0;
    }

    insert_char_in_row(&E.row[E.cursor_y], E.cursor_x, c);
    E.cursor_x++;
}

void insert_new_line() {
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
}

/**
 * Deletes a character to the left of the cursor.
 *
 * If the cursor’s past the end of the file, then there is nothing to delete,
 * and we return immediately. Otherwise, we get the editor_row the cursor is on,
 * and if there is a character to the left of the cursor, we delete it and
 * move the cursor one to the left.
 */
void delete_char() {
    if (E.cursor_y == E.numrows) return;
    if(E.cursor_x == 0 && E.cursor_y == 0) return;

    editor_row *row = &E.row[E.cursor_y];
    if (E.cursor_x > 0) {
        // store deleted character
        char deleted = row->chars[E.cursor_x - 1];
        char *copy = malloc(2);
        copy[0] = deleted;
        copy[1] = '\0';

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


/*** file i/o ***/

/**
 * Converts the rows of the editor into a single string.
 *
 * This function calculates the total length of a concatenated string
 * from multiple rows of text, including newline characters at the end
 * of each row. It saves the total length into `buflen` to inform the
 * caller of the string's length.
 *
 * After allocating the required memory, it loops through the rows,
 * using `memcpy()` to copy the contents of each row to the buffer,
 * appending a newline character after each row.
 *
 * The function returns the buffer, and it is the caller's responsibility
 * to free the allocated memory.
 *
 * @param buflen Pointer where the total length of the resulting string will be stored.
 * @return Pointer to the newly allocated string.
 */
char *rows_to_string(int *buflen) {
    int totallen = 0;
    int j;

    for (j = 0; j < E.numrows; j++)
        totallen += E.row[j].size + 1;
    *buflen = totallen;

    char *buf = malloc(totallen);
    char *p = buf;

    for (j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }

    return buf;
}

void open_editor(char *filename) {
    free(E.filename);
    // copies a given str, allocating required memory
    E.filename = strdup(filename);

    select_highlight();

    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = 0;
    size_t linecap = 0;
    ssize_t linelen;

    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while(linelen > 0 && (line[linelen - 1] == '\n' ||
                              line[linelen - 1] == '\r'))
            linelen--;
        insert_row(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

/**
 * @brief Saves the current content of the editor to a file.
 *
 * The function converts the editor's rows into a string and writes it to the file.
 * The file is opened with read and write permissions (`O_RDWR`),
 * and if it doesn't exist, it is created with standard permissions (`0644`).
 * The file size is adjusted to match the content length using `ftruncate()`,
 * ensuring that no leftover data remains. Truncating ourselves
 * (not using O_TRUNC flag in open()) is safer in case the ftruncate() call succeeds
 * but the write() call fails. In that case, the file would still contain
 * most of the data it had before.
 * After writing the content to the file, the allocated buffer is freed.
 */
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
        if (ftruncate(file, len) != -1){
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
        memcpy(E.row[saved_highlight_row].highlight, saved_highlight, E.row[saved_highlight_row].rsize);
        free(saved_highlight);
        saved_highlight = NULL;
    }

    if (key == '\r' || key == '\x1b') {
        last_match = -1;
        direction = 1;
        return;
    } else if (key == ARROW_RIGHT || key == ARROW_DOWN ) {
        direction = 1;
    } else if (key == ARROW_LEFT || key == ARROW_UP) {
        direction = -1;
    } else {
        last_match = -1;
        direction = 1;
    }

    if (last_match == -1) direction = 1;
    int current = last_match;

    int i;
    for (i = 0; i < E.numrows; i++) {
        current += direction;
        if (current == -1) current = E.numrows -1;
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
            memcpy(saved_highlight, row->highlight, row->size);
            memset(&row->highlight[match - row->render], HL_FIND, strlen(query));
            break;
        }
    }
}

void find() {
    int saved_cursor_x = E.cursor_x;
    int saved_cursor_y = E.cursor_y;
    int saved_colloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = get_user_input("Search: %s (ESC/Arrows/Enter)", find_callback);

    if (query) {
        free(query);
    } else {
        E.cursor_x = saved_cursor_x;
        E.cursor_y = saved_cursor_y;
        E.coloff = saved_colloff;
        E.rowoff = saved_rowoff;
    }
}

/*** Append buffer ***/
struct abuf
{
    char *b;
    int len;
};
#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf *ab, const char *s, int len)
{
    char *new = realloc(ab->b, ab->len + len);

    if (new == NULL)
        return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab)
{
    free(ab->b);
}

/*** Output ***/

void scroll() {
    E.rx = 0;
    if (E.cursor_y < E.numrows) {
        E.rx = row_cx_to_rx(&E.row[E.cursor_y], E.cursor_x);
    }

    if (E.cursor_y < E.rowoff) {
        E.rowoff = E.cursor_y;
    }
    if (E.cursor_y >= E.rowoff + E.screenrows) {
        E.rowoff = E.cursor_y - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void draw_rows(struct abuf *ab)
{
    int i;
    for (i = 0; i < E.screenrows; i++)
    {
        int fileditor_row = i + E.rowoff;

        if (fileditor_row < E.numrows) {
            char linenum[16];
            snprintf(linenum, sizeof(linenum), "%4d ", fileditor_row + 1);
            abAppend(ab, "\x1b[90m", LINENUM_WIDTH);
            abAppend(ab, linenum, strlen(linenum));
            abAppend(ab, "\x1b[39m", LINENUM_WIDTH);
        } else {
	    abAppend(ab, "     ", LINENUM_WIDTH);
	}

        if(fileditor_row >= E.numrows) {
            if (E.numrows == 0 && i == E.screenrows / 3)
            {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                                        "CCode editor -- version %s", CCODE_VERSION);
                if (welcomelen > E.screencols)
                    welcomelen = E.screencols;
                int padding = (E.screencols - welcomelen) / 2;
                if (padding)
                {
                    abAppend(ab, "-", 1);
                    padding--;
                }
                while (padding--)
                    abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                abAppend(ab, "-", 1);
            }
        } else {
            int len = E.row[fileditor_row].rsize - E.coloff;
            if (len < 0) len = 0;
            if(len > E.screencols) len = E.screencols;

            char *c = &E.row[fileditor_row].render[E.coloff];
            unsigned char *highlight = &E.row[fileditor_row].highlight[E.coloff];
            int current_color = -1; // This will be -1 for default color

            int j;
            for (j = 0; j < len; j++) {
                if (highlight[j] == HL_FIND) {
                    // Special case for HL_FIND: yellow background, black text
                    abAppend(ab, "\x1b[43m\x1b[30m", 10);
                    abAppend(ab, &c[j], 1);
                    abAppend(ab, "\x1b[49m\x1b[39m", 10); // reset bg and fg
                    current_color = -1;
                    continue;
                }

                if (iscntrl(c[j])) {
                    /** Check if the current character is a control character. If so, we translate it into a printable character
                     * by adding its value to '@' (in ASCII, the capital letters of the alphabet come after the @ character),
                     * or using the '?' character if it’s not in the alphabetic range.
                    */
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 1);
                    abAppend(ab, "\x1b[m", 3);
                    if (current_color != -1) {
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", current_color);
                        abAppend(ab, buf, clen);
                    }
                }
                else if (highlight[j] == HL_NORMAL) {
                    if (current_color != -1) {
                        abAppend(ab, "\x1b[39m", 5);
                        current_color = -1;
                    }
                    abAppend(ab, &c[j], 1);
                } else {
                    int color = highlight_to_color(highlight[j]);
                    if (color != current_color) {
                        current_color = color;
                        char buf[16];
                        int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, clen);
                    }
                    abAppend(ab, &c[j], 1);
                }
            }
            abAppend(ab, "\x1b[39m", 5);
        }
        abAppend(ab, "\x1b[K]", 3);
        abAppend(ab, "\r\n", 2);
    }
}

/**
 * The m command (Selected Graphic Rendition) is used to change the text's
 * appearance in the terminal - bold (1), underline (4) or inverted (7).
 * Effects can be combined, reseting text back - <esc>[m.
 * http://vt100.net/docs/vt100-ug/chapter3.html#SGR
 *
 * Current line is in E.cursor_y. Aligning the 2nd string,
 * spaces are printed until the right edge of screen.
 */
void draw_status_bar(struct abuf *ab) {
    // Switch to inverted colors with: <esc>[7m
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
        } else {
            abAppend(ab, " ", 1);
            len++;
        }
    }

    // Switch back to normal colors(formatting) with: <esc>[m
    abAppend(ab, "\x1b[m", 3);
    abAppend(ab, "\r\n", 2);
}

/**
 * Clear the msg bar with <esc>[K. We make sure msg fits the
 * with of screen and then display msg, only if it is 5 sec old.
 */
void draw_prompt_bar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.status_prompt);
    if (msglen > E.screencols) msglen = E.screencols;

    if (msglen && time(NULL) - E.status_prompt_time < 5)
        abAppend(ab, E.status_prompt, msglen);
}

void refresh_screen()
{
    scroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    draw_rows(&ab);
    draw_status_bar(&ab);
    draw_prompt_bar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.rowoff) + 1,
                                              (E.rx - E.coloff) + 1 + LINENUM_WIDTH);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // Show the cursor

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/**
 * Like printf() style function for printing status message.
 * This is a variadic function, takes any number of args.
 * https://en.wikipedia.org/wiki/Variadic_function
 */
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

void move_cursor(int key)
{
    editor_row *row = (E.cursor_y > E.numrows) ? NULL : &E.row[E.cursor_y];

    switch (key)
    {
    case ARROW_LEFT:
        if (E.cursor_x != 0) {
            E.cursor_x--;
        } else if (E.cursor_y > 0) {
            E.cursor_y--;
            E.cursor_x = E.row[E.cursor_y].size;
        }
        break;
    case ARROW_RIGHT:
        if(row && E.cursor_x < row->size) {
            E.cursor_x++;
        } else if (row && E.cursor_x == row->size) {
            E.cursor_y++;
            E.cursor_x = 0;
        }
        break;
    case ARROW_UP:
        if (E.cursor_y != 0)
            E.cursor_y--;
        break;
    case ARROW_DOWN:
        if (E.cursor_y < E.numrows - 1)
            E.cursor_y++;
        break;
    }

    row = (E.cursor_y >= E.numrows) ? NULL : &E.row[E.cursor_y];
    int rowlen = row ? row->size : 0;
    if (E.cursor_x > rowlen) {
        E.cursor_x = rowlen;
    }
}

void process_keypress()
{
    static int quit_times = CCODE_QUIT_TIMES;

    int c = read_keypress();

    switch (c) {
        case '\r': // Enter key
            insert_new_line();
            break;
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                set_prompt_message("Warning!!! File was not saved! "
                    "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2j]", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            save();
            break;
        case CTRL_KEY('z'):
            undo_operation();
            break;
        case CTRL_KEY('y'):
            redo_operation();
            break;
        case HOME_KEY:
            E.cursor_x = 0;
            break;
        case END_KEY:
            if (E.cursor_y < E.numrows) {
                E.cursor_x = E.row[E.cursor_y].size;
            }
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
        case PAGE_DOWN:
            {
                if (c == PAGE_UP) {
                    E.cursor_y = E.rowoff;
                } else if (c == PAGE_DOWN) {
                    E.cursor_y = E.rowoff + E.screenrows - 1;
                    if (E.cursor_y > E.numrows) E.cursor_y = E.numrows;
                }
                int times = E.screenrows;
                while (times--)
                    move_cursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            move_cursor(c);
            break;
        case CTRL_KEY('l'): // Tipically used to refresh screen
        case '\x1b': // Escape key F1-F12 included
            break;
        default:
            insert_char(c);
            break;
    }
    quit_times = CCODE_QUIT_TIMES;
}

/*** Init ***/

void init()
{
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

    if (get_windows_size(&E.screenrows, &E.screencols) == -1) die("get_windows_size");
    E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
    enable_rawmode();
    init();
    if (argc >= 2) {
        open_editor(argv[1]);
    }

    set_prompt_message("HELP: ^S = save ^Q = quit ^F = find ^Z = undo ^Y = Redo");

    while (1)
    {
        refresh_screen();
        process_keypress();
    }
    return 0;
}
