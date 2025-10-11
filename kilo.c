/* kilo.c - patched original Kilo with Undo/Redo and Markdown formatting toggles
 *
 * Features added:
 *  - Undo (Ctrl-Z) and Redo (Ctrl-Y) supporting inserts, deletes, cursor moves, and formatting inserts
 *  - Formatting toggles: Ctrl-B (bold -> **text**), Ctrl-U (underline -> _text_), Ctrl-K (strikethrough -> ~~text~~)
 *  - Formatting is stored as Markdown markers in the file
 *
 * Compile:
 *   gcc -o kilo kilo.c -Wall -Wextra
 *
 * Based on the original Kilo structure but simplified/modified as needed.
 */

#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define KILO_VERSION "0.9-undofmt"
#define TAB_STOP 8
#define CTRL_KEY(k) ((k) & 0x1f)
#define KILO_QUIT_TIMES 3

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

typedef struct erow {
    int idx;       /* row index in file */
    int size;      /* characters */
    char *chars;   /* row content */
    int rsize;     /* rendered size */
    char *render;  /* rendered content (tabs->spaces) */
} erow;

struct editorConfig {
    int cx, cy;    /* cursor x and y (in chars) */
    int rx;        /* render x */
    int rowoff;    /* row offset */
    int coloff;    /* col offset */
    int screenrows;
    int screencols;
    int numrows;
    erow *row;
    int dirty;
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
    struct termios orig_termios;
    int quit_times;
};

struct editorConfig E;

/*** Undo/Redo system ***/

typedef enum {
    ACT_INSERT,
    ACT_DELETE,
    ACT_MOVE
} ActionType;

typedef struct {
    ActionType type;
    int ay, ax;     /* position where action applies (row, col) - before action for delete/move, start for insert*/
    char *data;     /* text inserted or deleted (for insert/delete). NULL for move */
    int data_len;
    /* For moves, store previous coordinates in data as "y: x" integers encoded */
    int prev_cy;
    int prev_cx;
} EditorAction;

typedef struct {
    EditorAction *items;
    int len;
    int cap;
} ActionStack;

void actionstack_init(ActionStack *s) {
    s->items = NULL;
    s->len = 0;
    s->cap = 0;
}
void actionstack_push(ActionStack *s, EditorAction a) {
    if (s->len + 1 > s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->items = realloc(s->items, s->cap * sizeof(EditorAction));
    }
    s->items[s->len++] = a;
}
int actionstack_empty(ActionStack *s) { return s->len == 0; }
EditorAction actionstack_pop(ActionStack *s) {
    EditorAction a = s->items[--s->len];
    return a;
}
void actionstack_clear(ActionStack *s) {
    for (int i = 0; i < s->len; i++) {
        free(s->items[i].data);
    }
    free(s->items);
    s->items = NULL;
    s->len = s->cap = 0;
}

/* global stacks */
ActionStack undoStack;
ActionStack redoStack;

/*** prototypes ***/
void editorSetStatusMessage(const char *fmt, ...);

/*** terminal ***/

void die(const char *s) {
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(s);
    exit(1);
}

void disableRawMode() {
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
        ; /* ignore errors on exit */
}

void enableRawMode() {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; /* 0.1 seconds */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/*** input ***/

int editorReadKey() {
    int nread;
    char c;
    while ((nread = (int)read(STDIN_FILENO, &c, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN) die("read");
    }

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                /* extended escape */
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
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
        }
        return '\x1b';
    } else {
        return c;
    }
}

/*** row operations ***/

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
            rx += (TAB_STOP - 1) - (rx % TAB_STOP) + 1;
        else
            rx++;
    }
    return rx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    for (int j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->rsize = row->size + tabs*(TAB_STOP - 1);
    row->render = malloc(row->rsize + 1);

    int idx = 0;
    for (int j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
            row->render[idx++] = ' ';
            while (idx % TAB_STOP) row->render[idx++] = ' ';
        } else {
            row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
}

void editorInsertRow(int at, char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    memmove(&E.row[at+1], &E.row[at], sizeof(erow) * (E.numrows - at));
    for (int j = at + 1; j <= E.numrows; j++) E.row[j].idx++;

    E.row[at].idx = at;
    E.row[at].size = (int)len;
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

void editorDelRow(int at) {
    if (at < 0 || at >= E.numrows) return;
    editorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.numrows - at - 1));
    for (int j = at; j < E.numrows - 1; j++) E.row[j].idx = j;
    E.numrows--;
    E.dirty++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;
    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at+1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = (char)c;
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowAppendString(erow *row, char *s, size_t len) {
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], s, len);
    row->size += (int)len;
    row->chars[row->size] = '\0';
    editorUpdateRow(row);
    E.dirty++;
}

void editorRowDelChar(erow *row, int at) {
    if (at < 0 || at >= row->size) return;
    memmove(&row->chars[at], &row->chars[at+1], row->size - at);
    row->size--;
    editorUpdateRow(row);
    E.dirty++;
}

/*** editor operations ***/

void push_undo_insert(int ay, int ax, const char *s, int len) {
    EditorAction a;
    a.type = ACT_INSERT;
    a.ay = ay; a.ax = ax;
    a.data = malloc(len);
    memcpy(a.data, s, len);
    a.data_len = len;
    a.prev_cy = E.cy;
    a.prev_cx = E.cx;
    actionstack_push(&undoStack, a);
    /* clear redo */
    actionstack_clear(&redoStack);
}

void push_undo_delete(int ay, int ax, const char *s, int len) {
    EditorAction a;
    a.type = ACT_DELETE;
    a.ay = ay; a.ax = ax;
    a.data = malloc(len);
    memcpy(a.data, s, len);
    a.data_len = len;
    a.prev_cy = E.cy;
    a.prev_cx = E.cx;
    actionstack_push(&undoStack, a);
    actionstack_clear(&redoStack);
}

void push_undo_move(int prev_y, int prev_x) {
    EditorAction a;
    a.type = ACT_MOVE;
    a.ay = E.cy; a.ax = E.cx;
    a.data = NULL;
    a.data_len = 0;
    a.prev_cy = prev_y;
    a.prev_cx = prev_x;
    actionstack_push(&undoStack, a);
    actionstack_clear(&redoStack);
}

/* helpers to insert/delete a sequence at current position */
void editorInsertCharsAt(int y, int x, const char *s, int len) {
    if (y < 0 || y > E.numrows) return;
    if (y == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    erow *row = &E.row[y];
    for (int i = 0; i < len; i++) {
        editorRowInsertChar(row, x + i, s[i]);
    }
}

/* delete len chars starting at position (y,x) and return deleted text as malloc'ed buffer */
char *editorDelCharsAt(int y, int x, int len) {
    if (y < 0 || y >= E.numrows) return NULL;
    erow *row = &E.row[y];
    if (x < 0 || x >= row->size) return NULL;
    if (len <= 0) return NULL;
    if (x + len > row->size) len = row->size - x;
    char *buf = malloc(len);
    memcpy(buf, &row->chars[x], len);
    for (int i = 0; i < len; i++) editorRowDelChar(row, x);
    return buf;
}

void editorInsertChar(int c) {
    if (E.cy == E.numrows) {
        editorInsertRow(E.numrows, "", 0);
    }
    erow *row = &E.row[E.cy];
    editorRowInsertChar(row, E.cx, c);
    /* push undo for single character insertion */
    char ch = (char)c;
    push_undo_insert(E.cy, E.cx, &ch, 1);
    E.cx++;
}

void editorInsertNewline() {
    if (E.cx == 0) {
        editorInsertRow(E.cy, "", 0);
        push_undo_insert(E.cy, 0, "\n", 0); /* marker for new row insertion (not used heavily) */
    } else {
        erow *row = &E.row[E.cy];
        editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
        /* record delete of moved tail as a delete action then an insert for new row? Simpler: record as delete of tail */
        int tail_len = row->size - E.cx;
        char *tail = malloc(tail_len);
        memcpy(tail, &row->chars[E.cx], tail_len);
        /* we performed a split: so deletion from current row of tail */
        for (int i = 0; i < tail_len; i++) editorRowDelChar(row, E.cx);
        push_undo_delete(E.cy, E.cx, tail, tail_len);
        free(tail);
    }
    E.cy++;
    E.cx = 0;
}

void editorDelChar() {
    if (E.cy == E.numrows) return;
    if (E.cx == 0 && E.cy == 0) return;

    erow *row = &E.row[E.cy];
    if (E.cx > 0) {
        /* delete char at cx-1 */
        char *d = editorDelCharsAt(E.cy, E.cx - 1, 1);
        if (d) {
            push_undo_delete(E.cy, E.cx - 1, d, 1);
            free(d);
        }
        E.cx--;
    } else {
        /* join with previous line */
        int prev_len = E.row[E.cy - 1].size;
        editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);
        push_undo_delete(E.cy, 0, "", 0); /* simple marker: deletion of newline */
        editorDelRow(E.cy);
        E.cy--;
        E.cx = prev_len;
    }
}

/*** file i/o ***/

char *editorRowsToString(int *buflen) {
    int totlen = 0;
    for (int j = 0; j < E.numrows; j++)
        totlen += E.row[j].size + 1;
    *buflen = totlen;

    char *buf = malloc(totlen);
    char *p = buf;
    for (int j = 0; j < E.numrows; j++) {
        memcpy(p, E.row[j].chars, E.row[j].size);
        p += E.row[j].size;
        *p = '\n';
        p++;
    }
    return buf;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);

    FILE *fp = fopen(filename, "r");
    if (!fp) return;

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen-1] == '\n' || line[linelen-1] == '\r')) linelen--;
        editorInsertRow(E.numrows, line, linelen);
    }
    free(line);
    fclose(fp);
    E.dirty = 0;
}

int editorSave() {
    if (E.filename == NULL) return 0;
    int len;
    char *buf = editorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
    if (fd != -1) {
        if (ftruncate(fd, len) != -1) {
            if (write(fd, buf, len) == len) {
                close(fd);
                free(buf);
                E.dirty = 0;
                return 1;
            }
        }
        close(fd);
    }
    free(buf);
    return 0;
}

/*** append buffer ***/

struct abuf {
    char *b;
    int len;
};

#define ABUF_INIT {NULL,0}

void abAppend(struct abuf *ab, const char *s, int len) {
    char *new = realloc(ab->b, ab->len + len);
    if (!new) return;
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abFree(struct abuf *ab) {
    free(ab->b);
}

/*** output ***/

void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);

    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.rx < E.coloff) E.coloff = E.rx;
    if (E.rx >= E.coloff + E.screencols) E.coloff = E.rx - E.screencols + 1;
}

/* render with basic detection of markdown markers and apply terminal attributes via escape codes */
int is_marker_at(erow *row, int idx, const char *m, int mlen) {
    if (idx + mlen > row->rsize) return 0;
    return (strncmp(&row->render[idx], m, mlen) == 0);
}

/* we'll render markdown markers literally (they're stored in chars), but we'll apply attributes around text between markers.
   Simpler approach: when encountering markers in render, do not print markers and instead enable attribute until closing marker found.
   This is a heuristic and not a full markdown parser. */

void editorDrawRows(struct abuf *ab) {
    for (int y = 0; y < E.screenrows; y++) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int wlen = snprintf(welcome, sizeof(welcome), "kilo -- version %s", KILO_VERSION);
                if (wlen > E.screencols) wlen = E.screencols;
                int padding = (E.screencols - wlen) / 2;
                if (padding) abAppend(ab, "~", 1);
                while (padding--) abAppend(ab, " ", 1);
                abAppend(ab, welcome, wlen);
            } else {
                abAppend(ab, "~", 1);
            }
        } else {
            erow *row = &E.row[filerow];
            int len = row->rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;

            /* naive markdown markers: **, _, ~~ */
            int i = E.coloff;
            int printed = 0;
            int attr_on = 0;
            int attr_type = 0; /* 1=bold,2=underline,3=strike */
            while (printed < len && i < row->rsize) {
                /* check for markers at this render index by mapping back to chars roughly */
                /* Because render is expanded tabs, the markers are plain ascii and appear unchanged. */
                if (!attr_on && i + 2 <= row->rsize && row->render[i] == '*' && row->render[i+1] == '*') {
                    /* start or end bold */
                    attr_on = !attr_on;
                    /* toggle attribute */
                    if (attr_on) {
                        attr_type = 1;
                        abAppend(ab, "\x1b[1m", 4); /* bold on */
                    } else {
                        abAppend(ab, "\x1b[0m", 4);
                        attr_type = 0;
                    }
                    i += 2;
                    printed += 2;
                    continue;
                } else if (!attr_on && row->render[i] == '_' ) {
                    attr_on = !attr_on;
                    if (attr_on) {
                        attr_type = 2;
                        abAppend(ab, "\x1b[4m", 4); /* underline on */
                    } else {
                        abAppend(ab, "\x1b[0m", 4);
                        attr_type = 0;
                    }
                    i += 1;
                    printed += 1;
                    continue;
                } else if (!attr_on && i + 2 <= row->rsize && row->render[i] == '~' && row->render[i+1] == '~') {
                    attr_on = !attr_on;
                    if (attr_on) {
                        attr_type = 3;
                        abAppend(ab, "\x1b[9m", 4); /* strikethrough on */
                    } else {
                        abAppend(ab, "\x1b[0m", 4);
                        attr_type = 0;
                    }
                    i += 2;
                    printed += 2;
                    continue;
                } else {
                    /* print single char */
                    char buf[5] = {0};
                    buf[0] = row->render[i];
                    abAppend(ab, buf, 1);
                    i++; printed++;
                }
            }
            /* reset attributes if on */
            if (attr_on) {
                abAppend(ab, "\x1b[0m", 4);
            }
        }
        abAppend(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1) abAppend(ab, "\r\n", 2);
    }
}

void editorDrawStatusBar(struct abuf *ab) {
    abAppend(ab, "\x1b[7m", 4); /* invert colors */
    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numrows, E.dirty ? "(modified)" : "");
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d", E.cy + 1, E.numrows);
    if (len > E.screencols) len = E.screencols;
    abAppend(ab, status, len);
    while (len < E.screencols - (int)rlen) {
        abAppend(ab, " ", 1);
        len++;
    }
    abAppend(ab, rstatus, rlen);
    abAppend(ab, "\x1b[m", 4);
    abAppend(ab, "\r\n", 2);
}

void editorDrawMessageBar(struct abuf *ab) {
    abAppend(ab, "\x1b[K", 3);
    int msglen = (int)strlen(E.statusmsg);
    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abAppend(ab, E.statusmsg, msglen);
}

void editorRefreshScreen() {
    editorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    /* position cursor */
    char buf[32];
    int cx = E.rx - E.coloff + 1;
    int cy = E.cy - E.rowoff + 1;
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cy, cx);
    abAppend(&ab, buf, (int)strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abFree(&ab);
}

/*** input handling & editor movement ***/

void editorMoveCursor(int key) {
    int prev_y = E.cy, prev_x = E.cx;
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) {
                E.cy--;
            }
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) {
                E.cy++;
            }
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) E.cx = rowlen;

    /* push move action */
    push_undo_move(prev_y, prev_x);
}

/* formatting helper: try to wrap a word at cursor with markers, else insert pair markers and place cursor between them */
void editorToggleFormat(const char *open_mark, int mlen) {
    /* heuristic: find word boundaries around cursor in chars space */
    if (E.cy >= E.numrows) {
        /* just insert markers and place cursor between them */
        editorInsertCharsAt(E.cy, E.cx, open_mark, mlen);
        char close[8];
        if (mlen == 2 && open_mark[0] == '*' ) { close[0] = '*'; close[1] = '*'; }
        else if (mlen == 2 && open_mark[0] == '~') { close[0] = '~'; close[1] = '~'; }
        else { close[0] = open_mark[0]; }
        editorInsertCharsAt(E.cy, E.cx + mlen, close, mlen);
        push_undo_insert(E.cy, E.cx, open_mark, mlen);
        push_undo_insert(E.cy, E.cx + mlen, close, mlen);
        E.cx += mlen;
        return;
    }

    erow *row = &E.row[E.cy];
    int L = row->size;
    int left = E.cx - 1;
    while (left >= 0 && !isspace((unsigned char)row->chars[left])) left--;
    left++;
    int right = E.cx;
    while (right < L && !isspace((unsigned char)row->chars[right])) right++;

    if (left < right) {
        /* check if already wrapped by markers */
        int has_open = 0, has_close = 0;
        if (left - mlen >= 0) {
            has_open = (strncmp(&row->chars[left - mlen], open_mark, mlen) == 0);
        }
        if (right + mlen <= L) {
            has_close = (strncmp(&row->chars[right], open_mark, mlen) == 0);
        }
        if (has_open && has_close) {
            /* remove markers */
            char *del1 = editorDelCharsAt(E.cy, left - mlen, mlen);
            char *del2 = editorDelCharsAt(E.cy, right - mlen - mlen, mlen); /* adjust because first deletion shifted */
            if (del1) { push_undo_delete(E.cy, left - mlen, del1, mlen); free(del1); }
            if (del2) { push_undo_delete(E.cy, right - mlen - mlen, del2, mlen); free(del2); }
            E.cx = left - mlen;
            return;
        } else {
            /* insert markers around [left,right) */
            editorInsertCharsAt(E.cy, right, open_mark, mlen); /* insert close after word */
            editorInsertCharsAt(E.cy, left, open_mark, mlen);  /* insert open before word */
            push_undo_insert(E.cy, left, open_mark, mlen);
            push_undo_insert(E.cy, right + mlen, open_mark, mlen);
            E.cx = right + mlen; /* move cursor after inserted open */
            return;
        }
    } else {
        /* no word - insert pair and place cursor between */
        editorInsertCharsAt(E.cy, E.cx, open_mark, mlen);
        char close[8];
        memcpy(close, open_mark, mlen);
        editorInsertCharsAt(E.cy, E.cx + mlen, close, mlen);
        push_undo_insert(E.cy, E.cx, open_mark, mlen);
        push_undo_insert(E.cy, E.cx + mlen, close, mlen);
        E.cx += mlen;
        return;
    }
}

/* perform undo */
void editorUndo() {
    if (actionstack_empty(&undoStack)) {
        editorSetStatusMessage("Nothing to undo");
        return;
    }
    EditorAction a = actionstack_pop(&undoStack);

    /* to redo, we need inverse action */
    EditorAction inverse;
    inverse.data = NULL;
    inverse.data_len = 0;

    if (a.type == ACT_INSERT) {
        /* we inserted data at (ay,ax) -> so undo by deleting it */
        int y = a.ay, x = a.ax;
        /* delete a.data_len chars starting at y,x */
        char *deleted = editorDelCharsAt(y, x, a.data_len);
        /* prepare inverse: delete -> to redo we need to insert back */
        inverse.type = ACT_DELETE;
        inverse.ay = y;
        inverse.ax = x;
        inverse.prev_cy = a.prev_cy;
        inverse.prev_cx = a.prev_cx;
        if (deleted) {
            inverse.data = malloc(a.data_len);
            memcpy(inverse.data, deleted, a.data_len);
            inverse.data_len = a.data_len;
            free(deleted);
        } else {
            inverse.data = NULL;
            inverse.data_len = 0;
        }
    } else if (a.type == ACT_DELETE) {
        /* we deleted data at (ay,ax) -> so undo by inserting it back */
        int y = a.ay, x = a.ax;
        if (a.data_len > 0)
            editorInsertCharsAt(y, x, a.data, a.data_len);
        inverse.type = ACT_INSERT;
        inverse.ay = y;
        inverse.ax = x;
        inverse.prev_cy = a.prev_cy;
        inverse.prev_cx = a.prev_cx;
        inverse.data_len = a.data_len;
        if (a.data_len > 0) {
            inverse.data = malloc(a.data_len);
            memcpy(inverse.data, a.data, a.data_len);
        } else inverse.data = NULL;
    } else if (a.type == ACT_MOVE) {
        /* move: we moved from prev to current (stored in a.prev_cy/prev_cx), undo by restoring prev */
        int oldy = a.prev_cy;
        int oldx = a.prev_cx;
        inverse.type = ACT_MOVE;
        inverse.ay = E.cy;
        inverse.ax = E.cx;
        inverse.prev_cy = oldy;
        inverse.prev_cx = oldx;
        inverse.data = NULL;
        inverse.data_len = 0;
        /* perform move */
        E.cy = oldy;
        E.cx = oldx;
    }

    /* push inverse onto redo stack */
    actionstack_push(&redoStack, inverse);

    /* free popped action's data */
    free(a.data);
}

/* perform redo */
void editorRedo() {
    if (actionstack_empty(&redoStack)) {
        editorSetStatusMessage("Nothing to redo");
        return;
    }
    EditorAction a = actionstack_pop(&redoStack);
    EditorAction inverse;
    inverse.data = NULL;
    inverse.data_len = 0;

    if (a.type == ACT_INSERT) {
        /* insert back */
        int y = a.ay, x = a.ax;
        if (a.data_len > 0)
            editorInsertCharsAt(y, x, a.data, a.data_len);
        inverse.type = ACT_DELETE;
        inverse.ay = y; inverse.ax = x;
        inverse.prev_cy = a.prev_cy;
        inverse.prev_cx = a.prev_cx;
        inverse.data_len = a.data_len;
        if (a.data_len > 0) {
            inverse.data = malloc(a.data_len);
            memcpy(inverse.data, a.data, a.data_len);
        } else inverse.data = NULL;
    } else if (a.type == ACT_DELETE) {
        int y = a.ay, x = a.ax;
        char *deleted = editorDelCharsAt(y, x, a.data_len);
        inverse.type = ACT_INSERT;
        inverse.ay = y; inverse.ax = x;
        inverse.prev_cy = a.prev_cy;
        inverse.prev_cx = a.prev_cx;
        inverse.data_len = a.data_len;
        if (deleted) {
            inverse.data = malloc(a.data_len);
            memcpy(inverse.data, deleted, a.data_len);
            free(deleted);
        } else inverse.data = NULL;
    } else if (a.type == ACT_MOVE) {
        inverse.type = ACT_MOVE;
        inverse.ay = E.cy;
        inverse.ax = E.cx;
        inverse.prev_cy = a.prev_cy;
        inverse.prev_cx = a.prev_cx;
        /* perform move */
        E.cy = a.prev_cy;
        E.cx = a.prev_cx;
    }

    actionstack_push(&undoStack, inverse);
    free(a.data);
}

/*** status message ***/

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

/*** init ***/

void initEditor() {
    E.cx = 0; E.cy = 0; E.rx = 0;
    E.rowoff = 0; E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.dirty = 0;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;
    E.quit_times = KILO_QUIT_TIMES;

    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        /* fallback */
        E.screenrows = 24;
        E.screencols = 80;
    } else {
        E.screenrows = ws.ws_row - 2; /* leave room for status/message */
        E.screencols = ws.ws_col;
    }

    actionstack_init(&undoStack);
    actionstack_init(&redoStack);
}

/*** input processing ***/

void editorProcessKeypress() {
    int c = editorReadKey();

    switch (c) {
        case '\r':
            editorInsertNewline();
            break;
        case CTRL_KEY('q'):
            if (E.dirty && E.quit_times > 0) {
                editorSetStatusMessage("WARNING: File has unsaved changes. Press Ctrl-Q %d more times to quit.", E.quit_times);
                E.quit_times--;
                return;
            }
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case CTRL_KEY('s'):
            if (editorSave()) {
                editorSetStatusMessage("File saved.");
            } else {
                editorSetStatusMessage("Error saving: %s", strerror(errno));
            }
            break;
        case CTRL_KEY('z'): /* undo */
            editorUndo();
            break;
        case CTRL_KEY('y'): /* redo */
            editorRedo();
            break;
        case CTRL_KEY('b'): /* bold -> ** */
            editorToggleFormat("**", 2);
            break;
        case CTRL_KEY('u'): /* underline -> _ */
            editorToggleFormat("_", 1);
            break;
        case CTRL_KEY('k'): /* strikethrough -> ~~ */
            editorToggleFormat("~~", 2);
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
            break;
        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            if (c == DEL_KEY) editorMoveCursor(ARROW_RIGHT);
            editorDelChar();
            break;
        case PAGE_UP:
        case PAGE_DOWN: {
            int times = E.screenrows;
            while (times--) editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
        } break;
        case ARROW_UP:
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
            editorMoveCursor(c);
            break;
        default:
            if (!iscntrl(c)) {
                editorInsertChar(c);
            }
            break;
    }
    E.quit_times = KILO_QUIT_TIMES;
}

/*** main ***/

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();

    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-S = save | Ctrl-Q = quit | Ctrl-Z = undo | Ctrl-Y = redo | Ctrl-B/U/K = bold/underline/strike");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }
    return 0;
}

