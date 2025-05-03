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
 * // TODO: Going to end of file is not 1 row after last line - FIX
 */

/*** * Defines: ***/
#define CCODE_VERSION "0.0.1"
#define CCODE_TAB_STOP 8
#define CCODE_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey
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

/*** Data: ***/

// editor row
typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

struct editorConfig
{
    int cursor_x, cursor_y;
    int rx; // index into render field
    int rowoff; // row offset
    int coloff; // column offset
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty; // If file has been modified since opening or saving
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
};
struct editorConfig E;

/*** Prototypes ***/

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen();
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** Terminal ***/
void die(const char *s)
{
    write(STDOUT_FILENO, "\x1b[2j]", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode()
{
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        die("tcsetattr");
}

void enableRawMode()
{
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
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

int editorReadKey()
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

int getCursorPosition(int *rows, int *cols)
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

int getWindowSize(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
    {
        // Moves the cursor to the bottom right corner of the screen
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    }
    else
    {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
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
int editorRowRxToCx(erow *row, int rx) {
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

void editorUpdateRow(erow *row) {
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
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;

    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);

    E.numrows++;
    E.dirty++;
}

void editorFreeRow(erow *row) {
    free(row->render);
    free(row->chars);
}

/**
 * Deletes a row at the specified index.
 * First we validate the at index. Then we free the memory owned by the row
 * using editorFreeRow(). We then use memmove() to overwrite the deleted row
 * struct with the rest of the rows that come after it, and decrement numrows.
 * Finally, we increment E.dirty.
 *
 * @param int at The index of the row to delete.
 */
void editorDeleteRow(int at) {
    if (at < 0 || at >= E.numrows) return;

    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));
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
 * @param erow The row where the character will be inserted.
 * @param at The index at which to insert the character.
 * @param c The character to be inserted.
 */
void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size)
        at = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    editorUpdateRow(row);
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
void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);

    row->size += len;
    row->chars[row->size] = '\0';

    editorUpdateRow(row);
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
void editorRowDeleteChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void editorInsertChar(int c) {
    if(E.cursor_y == E.numrows)
        editorInsertRow(E.numrows, "", 0);

    editorRowInsertChar(&E.row[E.cursor_y], E.cursor_x, c);
    E.cursor_x++;
}

void editorInsertNewline() {
    if (E.cursor_x == 0) {
        editorInsertRow(E.cursor_y, "", 0);
    } else {
        erow *row = &E.row[E.cursor_y];
        editorInsertRow(E.cursor_y + 1, &row->chars[E.cursor_x], row->size - E.cursor_x);
        row = &E.row[E.cursor_y];
        row->size = E.cursor_x;
        row->chars = realloc(row->chars, row->size + 1);
        row->chars[row->size] = '\0';
        editorUpdateRow(row);
    }
    E.cursor_y++;
    E.cursor_x = 0;
}

/**
 * Deletes a character to the left of the cursor.
 *
 * If the cursor’s past the end of the file, then there is nothing to delete,
 * and we return immediately. Otherwise, we get the erow the cursor is on,
 * and if there is a character to the left of the cursor, we delete it and
 * move the cursor one to the left.
 */
void editorDeleteChar() {
    if (E.cursor_y == E.numrows) return;
    if(E.cursor_x == 0 && E.cursor_y == 0) return;

    erow *row = &E.row[E.cursor_y];
    if (E.cursor_x > 0) {
        editorRowDeleteChar(row, E.cursor_x - 1);
        E.cursor_x--;
    } else {
        E.cursor_x = E.row[E.cursor_y - 1].size;
        editorRowAppendString(&E.row[E.cursor_y - 1], row->chars, row->size);
        editorDeleteRow(E.cursor_y);
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
char *editorRowsToString(int *buflen) {
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

void editorOpen(char *filename) {
    free(E.filename);
    // copies a gives str, allocating required memory
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if(!fp) die("fopen");

    char *line = 0;
    size_t linecap = 0;
    ssize_t linelen;

    while((linelen = getline(&line, &linecap, fp)) != -1) {
        while(linelen > 0 && (line[linelen - 1] == '\n' ||
                              line[linelen - 1] == '\r'))
            linelen--;
        editorInsertRow(E.numrows, line, linelen);
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
void editorSave() {
    if (E.filename == NULL) {
        E.filename = editorPrompt("Save as: %s (ESC to cancel)", NULL);
        if (E.filename == NULL) {
            editorSetStatusMessage("Save aborted");
            return;
        }
    }

    int len;
    char *buf = editorRowsToString(&len);

    int file = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (file != -1) {
        if (ftruncate(file, len) != -1){
            if (write(file, buf, len) == len) {
                close(file);
                free(buf);
                E.dirty = 0;
                editorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(file);
    }

    free(buf);
    editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** Find ***/
void editorFindCallback(char *query, int key) {
    static int last_match = -1;
    static int direction = 1;

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

        erow *row = &E.row[current];
        char *match = strstr(row->render, query);
        if (match) {
            last_match = current;
            E.cursor_y = current;
            E.cursor_x = editorRowRxToCx(row, match - row->render);
            E.rowoff = E.numrows;

            break;
        }
    }
}

void editorFind() {
    int saved_cursor_x = E.cursor_x;
    int saved_cursor_y = E.cursor_y;
    int saved_colloff = E.coloff;
    int saved_rowoff = E.rowoff;

    char *query = editorPrompt("Search: %s (ESC/Arrows/Enter)", editorFindCallback);

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

void editorScroll() {
    E.rx = 0;
    if (E.cursor_y < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cursor_y], E.cursor_x);
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

void editorDrawRows(struct abuf *ab)
{
    int i;
    for (i = 0; i < E.screenrows; i++)
    {
        int filerow = i + E.rowoff;
        if(filerow >= E.numrows) {
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
                    abAppend(ab, "~", 1);
                    padding--;
                }
                while (padding--)
                    abAppend(ab, " ", 1);
                abAppend(ab, welcome, welcomelen);
            }
            else
            {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if(len > E.screencols) len = E.screencols;
            abAppend(ab, &E.row[filerow].render[E.coloff], len);
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
void editorDrawStatusBar(struct abuf *ab) {
    // Switch to inverted colors with: <esc>[7m
    abAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
        E.filename ? E.filename : "[No Name]", E.numrows,
        E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        E.cursor_y + 1, E.numrows);

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
void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;

    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen()
{
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cursor_y - E.rowoff) + 1,
                                              (E.rx - E.coloff) + 1);
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
void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);

    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);

    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** Input ***/
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
    size_t bufsize = 128;
    char *buf = malloc(bufsize);

    size_t buflen = 0;
    buf[0] = '\0';

    while (1) {
        editorSetStatusMessage(prompt, buf);
        editorRefreshScreen();

        int c = editorReadKey();
        if (c == DELETE_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
            if (buflen != 0) buf[--buflen] = '\0';
        } else if (c == '\x1b') {
            editorSetStatusMessage("");
            if (callback) callback(buf, c);
            free(buf);
            return NULL;
        } else if (c == '\r') {
            if (buflen != 0) {
                editorSetStatusMessage("");
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

void editorMoveCursor(int key)
{
    erow *row = (E.cursor_y > E.numrows) ? NULL : &E.row[E.cursor_y];

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

void editorProcessKeypress()
{
    static int quit_times = CCODE_QUIT_TIMES;

    int c = editorReadKey();

    switch (c) {
        case '\r': // Enter key
            editorInsertNewline();
            break;
        case CTRL_KEY('q'):
            if (E.dirty && quit_times > 0) {
                editorSetStatusMessage("Warning!!! File has unsaved changes. "
                    "Press Ctrl-Q %d more times to quit.", quit_times);
                quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2j]", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;

        case CTRL_KEY('s'):
            editorSave();
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
            editorFind();
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DELETE_KEY:
            if (c == DELETE_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDeleteChar();
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
                    editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        case CTRL_KEY('l'): // Tipically used to refresh screen
        case '\x1b': // Escape key F1-F12 included
            break;
        default:
            editorInsertChar(c);
            break;
    }
    quit_times = CCODE_QUIT_TIMES;
}

/*** Init ***/

void initEditor()
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
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
    E.screenrows -= 2;
}

int main(int argc, char *argv[])
{
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | CTRL-F = find");

    while (1)
    {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}
