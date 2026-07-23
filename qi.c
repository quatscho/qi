/*
 * qi - A Lightweight Terminal Text Editor
 * Author: Christopher Camacho
 * Version: 1.1.29 (2026)
 * License: GPL version 3
 *
 * A minimalist, ncurses-based text editor featuring dynamic line counting,
 * interactive search and replace, multi-line deletion tools, visual state
 * change tracking, and multi-language syntax highlighting.
 */
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <ncurses.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <sys/stat.h>
#include <locale.h>
#include <wchar.h>
#include "tracker.h"
#include "syntax.h"

/* ---------- UTF-8 display-width helper ----------
 * Returns the number of terminal columns needed to display the string s.
 * Multi-byte UTF-8 sequences are decoded; each codepoint contributes
 * wcwidth() columns (1 for most, 2 for wide CJK, 0 for combining).
 * Undecodable bytes are counted as 1 column each.
 * NOTE: cursor_x and all editing paths remain byte-indexed; only
 * wrap/row-count calculations use this function.  Convert individual
 * features to column-awareness as they are touched in future changes.
 */
static int utf8_display_width(const char *s) {
    if (!s) return 0;
    int cols = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        wchar_t wc;
        int bytes;
        if (*p < 0x80) {
            wc = *p; bytes = 1;
        } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
            wc = ((*p & 0x1F) << 6) | (p[1] & 0x3F); bytes = 2;
        } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
            wc = ((*p & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); bytes = 3;
        } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
            wc = ((*p & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); bytes = 4;
        } else {
            /* Invalid/partial byte — count as 1 column */
            cols++; p++; continue;
        }
        int w = wcwidth(wc);
        cols += (w >= 0) ? w : 1;
        p += bytes;
    }
    return cols;
}

/* utf8_display_width for a prefix of n bytes */
static int utf8_display_width_n(const char *s, int n) {
    if (!s || n <= 0) return 0;
    char *tmp = malloc(n + 1);
    if (!tmp) return n;
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    int w = utf8_display_width(tmp);
    free(tmp);
    return w;
}

#define MAX_LINE_LEN 512
#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_UNDO 500
#define VERSION "1.1.29"

/* ---------- dynamic line storage ---------- */
static char **lines = NULL;   /* heap array of heap strings          */
static int line_cap = 0;      /* allocated slots in lines[]          */
int line_count = 0;           /* active lines                        */

/* Grow lines[] to hold at least need slots. Returns 0 on OOM. */
static int ensure_capacity(int need) {
    if (need <= line_cap) return 1;
    int new_cap = line_cap ? line_cap * 2 : 256;
    while (new_cap < need) new_cap *= 2;
    char **tmp = realloc(lines, new_cap * sizeof(char *));
    if (!tmp) return 0;
    lines = tmp;
    for (int i = line_cap; i < new_cap; i++) lines[i] = NULL;
    line_cap = new_cap;
    return 1;
}

/* Return a heap-allocated copy of s, or empty string on OOM. */
static char *xstrdup(const char *s) {
    char *p = strdup(s);
    return p ? p : strdup("");
}

/* Replace lines[i] with a copy of s. */
static void set_line(int i, const char *s) {
    free(lines[i]);
    lines[i] = xstrdup(s);
}

/* Insert a blank line at position i, shifting everything down. */
static int insert_line_at(int i, const char *s) {
    if (!ensure_capacity(line_count + 1)) return 0;
    memmove(&lines[i + 1], &lines[i], (line_count - i) * sizeof(char *));
    lines[i] = NULL;
    set_line(i, s);
    line_count++;
    return 1;
}

/* Remove line i, shifting everything up. */
static void remove_line_at(int i) {
    free(lines[i]);
    memmove(&lines[i], &lines[i + 1], (line_count - i - 1) * sizeof(char *));
    lines[line_count - 1] = NULL;
    line_count--;
}

/* ---------- global state ---------- */
int current_line = 0;
int cursor_x = 0;
int scroll_y = 0;
char current_filename[256] = "untitled.txt";
char status_msg[512] = "";
int is_modified = 0;
int mod_count = 0;
int overwrite_mode = 0;
clock_t last_char_time = 0;
int in_paste_stream = 0;
static int is_pasting = 0;

/* mtime of the file as last loaded/saved; 0 = untitled or unknown */
static time_t file_mtime = 0;

/* Set by SIGTERM/SIGHUP handler — checked in main loop */
static volatile sig_atomic_t got_fatal_signal = 0;

static void fatal_signal_handler(int sig) {
    (void)sig;
    got_fatal_signal = 1;
}

/* Bracket match highlight: -1 means no active match */
int match_line = -1;
int match_col  = -1;

/* Line clipboard for Ctrl+K / Ctrl+P */
static char *clipboard_line = NULL;

/* Syntax highlight toggle */
static int syntax_highlight_enabled = 1;

/* Gutter + col-81 guide visibility toggle (F4) */
static int gutter_visible = 1;

/* ---------- undo / redo ----------
 *
 * Operations recorded:
 *   OP_CHAR_INS  — one character inserted at (line, col)
 *   OP_CHAR_DEL  — one character deleted from (line, col)
 *   OP_LINE_SPLIT — line split at (line, col): lines[line] truncated, new line inserted after
 *   OP_LINE_JOIN  — lines[line] and lines[line+1] joined; join_col is where the join happened
 *   OP_BULK       — arbitrary multi-line snapshot (replace-all, delete-lines, paste)
 *
 * The ring buffer holds up to MAX_UNDO records.  Undo pops from the undo
 * head; the inverse is pushed onto the redo stack.  Any new edit clears redo.
 */
typedef enum {
    OP_CHAR_INS,
    OP_CHAR_DEL,
    OP_LINE_SPLIT,
    OP_LINE_JOIN,
    OP_BULK,
} OpType;

typedef struct {
    OpType type;
    int    line;        /* line index at time of operation        */
    int    col;         /* column index (char ops and split/join) */
    char   ch;          /* character (OP_CHAR_INS / OP_CHAR_DEL) */
    /* OP_LINE_JOIN: text of the line that was removed (lines[line+1]) */
    /* OP_BULK: snapshot of all lines from bulk_start..bulk_start+bulk_count-1 */
    char **bulk_lines;  /* heap-allocated array of heap strings   */
    int    bulk_start;
    int    bulk_count;
    int    bulk_total;  /* total line_count at snapshot time      */
    /* cursor state before the operation */
    int    saved_line;
    int    saved_col;
} UndoOp;

#define UNDO_CAP MAX_UNDO

static UndoOp *undo_buf = NULL;   /* ring buffer, allocated in main()  */
static int undo_head = 0;         /* index of oldest entry             */
static int undo_count = 0;        /* number of valid entries           */

static UndoOp *redo_buf = NULL;   /* linear stack (not ring)           */
static int redo_top = -1;

void save_undo_state_single(int line_idx);  /* kept for call-site compat */
void save_undo_state_batch(int start_line, int count);
void undo(void);
void redo_op(void);

/* Free heap strings inside an op (bulk only). */
static void free_op(UndoOp *op) {
    if ((op->type == OP_BULK || op->type == OP_LINE_JOIN) && op->bulk_lines) {
        for (int i = 0; i < op->bulk_count; i++) free(op->bulk_lines[i]);
        free(op->bulk_lines);
        op->bulk_lines = NULL;
    }
}

/* Push op onto the undo ring, evicting the oldest entry if full. */
static void push_undo(UndoOp *op) {
    if (undo_count == UNDO_CAP) {
        /* Evict oldest */
        free_op(&undo_buf[undo_head]);
        undo_head = (undo_head + 1) % UNDO_CAP;
        undo_count--;
    }
    int slot = (undo_head + undo_count) % UNDO_CAP;
    undo_buf[slot] = *op;
    undo_count++;
}

/* Record a single-character insertion. */
static void record_char_ins(int line, int col, char ch) {
    UndoOp op = {0};
    op.type = OP_CHAR_INS;
    op.line = line; op.col = col; op.ch = ch;
    op.saved_line = current_line; op.saved_col = cursor_x;
    push_undo(&op);
}

/* Record a single-character deletion. */
static void record_char_del(int line, int col, char ch) {
    UndoOp op = {0};
    op.type = OP_CHAR_DEL;
    op.line = line; op.col = col; op.ch = ch;
    op.saved_line = current_line; op.saved_col = cursor_x;
    push_undo(&op);
}

/* Record a line split: lines[line] was truncated at col, new line inserted after. */
static void record_line_split(int line, int col) {
    UndoOp op = {0};
    op.type = OP_LINE_SPLIT;
    op.line = line; op.col = col;
    op.saved_line = current_line; op.saved_col = cursor_x;
    push_undo(&op);
}

/* Record a line join: lines[line] and lines[line+1] were merged.
 * tail is the text that was in lines[line+1] before the join. */
static void record_line_join(int line, int col, const char *tail) {
    UndoOp op = {0};
    op.type = OP_LINE_JOIN;
    op.line = line; op.col = col;
    op.bulk_lines = malloc(sizeof(char *));
    if (op.bulk_lines) { op.bulk_lines[0] = xstrdup(tail); op.bulk_count = 1; }
    op.saved_line = current_line; op.saved_col = cursor_x;
    push_undo(&op);
}

/* Record a bulk snapshot of lines[start..start+count-1]. */
static void record_bulk(int start, int count) {
    if (count <= 0) return;
    UndoOp op = {0};
    op.type = OP_BULK;
    op.bulk_start = start;
    op.bulk_count = count;
    op.bulk_total = line_count;
    op.bulk_lines = malloc(count * sizeof(char *));
    if (!op.bulk_lines) return;
    for (int i = 0; i < count; i++)
        op.bulk_lines[i] = xstrdup(lines[start + i]);
    op.saved_line = current_line; op.saved_col = cursor_x;
    push_undo(&op);
}

/* Compatibility wrappers used by replace_text and delete_lines_interactive. */
void save_undo_state_single(int line_idx) { record_bulk(line_idx, 1); }
void save_undo_state_batch(int start_line, int count) { record_bulk(start_line, count); }

/* Push a full-file snapshot onto the redo stack so redo_op can restore it exactly. */
static void push_redo_snapshot(int saved_ln, int saved_cx) {
    if (redo_top >= UNDO_CAP - 1) {
        /* Drop the oldest redo entry (bottom of stack) to make room */
        free_op(&redo_buf[0]);
        memmove(&redo_buf[0], &redo_buf[1], redo_top * sizeof(UndoOp));
        redo_top--;
    }
    redo_top++;
    UndoOp *r = &redo_buf[redo_top];
    memset(r, 0, sizeof(UndoOp));
    r->type       = OP_BULK;
    r->bulk_start = 0;
    r->bulk_count = line_count;
    r->bulk_total = line_count;
    r->saved_line = saved_ln;
    r->saved_col  = saved_cx;
    r->bulk_lines = malloc(line_count * sizeof(char *));
    if (r->bulk_lines)
        for (int i = 0; i < line_count; i++)
            r->bulk_lines[i] = xstrdup(lines[i]);
}

void undo(void) {
    if (undo_count == 0) { snprintf(status_msg, sizeof(status_msg), "Nothing to undo!"); return; }

    int slot = (undo_head + undo_count - 1) % UNDO_CAP;
    UndoOp *op = &undo_buf[slot];

    /* Snapshot the current state onto the redo stack BEFORE applying undo.
     * redo_op() will restore this snapshot exactly, reverting the undo. */
    push_redo_snapshot(current_line, cursor_x);

    switch (op->type) {
    case OP_CHAR_INS:
        if (op->line < line_count) {
            int len = (int)strlen(lines[op->line]);
            if (op->col < len)
                memmove(lines[op->line] + op->col,
                        lines[op->line] + op->col + 1,
                        len - op->col);
        }
        break;
    case OP_CHAR_DEL:
        if (op->line < line_count) {
            int len = (int)strlen(lines[op->line]);
            char *nl = malloc(len + 2);
            if (nl) {
                memcpy(nl, lines[op->line], op->col);
                nl[op->col] = op->ch;
                memcpy(nl + op->col + 1, lines[op->line] + op->col, len - op->col + 1);
                free(lines[op->line]); lines[op->line] = nl;
            }
        }
        break;
    case OP_LINE_SPLIT:
        if (op->line + 1 < line_count) {
            int a_len = (int)strlen(lines[op->line]);
            int b_len = (int)strlen(lines[op->line + 1]);
            char *merged = malloc(a_len + b_len + 1);
            if (merged) {
                memcpy(merged, lines[op->line], a_len);
                memcpy(merged + a_len, lines[op->line + 1], b_len + 1);
                free(lines[op->line]); lines[op->line] = merged;
                remove_line_at(op->line + 1);
            }
        }
        break;
    case OP_LINE_JOIN:
        if (op->line < line_count && op->bulk_lines) {
            char *head = xstrdup(lines[op->line]);
            head[op->col] = '\0';
            free(lines[op->line]); lines[op->line] = head;
            insert_line_at(op->line + 1, op->bulk_lines[0]);
        }
        break;
    case OP_BULK: {
        int bs = op->bulk_start, bc = op->bulk_count;
        while (line_count > bs) remove_line_at(line_count - 1);
        for (int i = 0; i < bc; i++) {
            ensure_capacity(line_count + 1);
            lines[line_count] = xstrdup(op->bulk_lines[i]);
            line_count++;
        }
        if (line_count == 0) { ensure_capacity(1); lines[0] = xstrdup(""); line_count = 1; }
        break;
    }
    }

    current_line = op->saved_line;
    cursor_x    = op->saved_col;
    if (current_line >= line_count) current_line = line_count - 1;
    if (current_line < 0) current_line = 0;
    int clen = (int)strlen(lines[current_line]);
    if (cursor_x > clen) cursor_x = clen;

    free_op(op);
    undo_count--;

    is_modified = 1;
    snprintf(status_msg, sizeof(status_msg), "Undo performed.");
}

void redo_op(void) {
    if (redo_top < 0) { snprintf(status_msg, sizeof(status_msg), "Nothing to redo!"); return; }
    UndoOp *op = &redo_buf[redo_top];

    /* Restore the snapshot that was taken just before undo ran. */
    int bs = op->bulk_start, bc = op->bulk_count;
    while (line_count > bs) remove_line_at(line_count - 1);
    if (op->bulk_lines) {
        for (int i = 0; i < bc; i++) {
            ensure_capacity(line_count + 1);
            lines[line_count] = xstrdup(op->bulk_lines[i]);
            line_count++;
        }
    }
    if (line_count == 0) { ensure_capacity(1); lines[0] = xstrdup(""); line_count = 1; }

    current_line = op->saved_line;
    cursor_x    = op->saved_col;
    if (current_line >= line_count) current_line = line_count - 1;
    if (current_line < 0) current_line = 0;
    int clen2 = (int)strlen(lines[current_line]);
    if (cursor_x > clen2) cursor_x = clen2;

    free_op(op);
    redo_top--;
    is_modified = 1;
    snprintf(status_msg, sizeof(status_msg), "Redo performed.");
}

/* ---------- file I/O ---------- */
void load_file(const char *filename) {
    /* Clear undo and redo stacks on file load */
    for (int i = 0; i < undo_count; i++) free_op(&undo_buf[(undo_head + i) % UNDO_CAP]);
    undo_head = 0; undo_count = 0;
    for (int i = 0; i <= redo_top; i++) free_op(&redo_buf[i]);
    redo_top = -1;

    char swp_filename[256];
    snprintf(swp_filename, sizeof(swp_filename), ".%s.swp", filename);
    const char *target_file = filename;
    FILE *swp_fp = fopen(swp_filename, "r");
    if (swp_fp) {
        fclose(swp_fp);
        mvprintw(LINES - 1, 0, "Swap file detected for '%s'. Recover? (y/n): ", filename);
        clrtoeol();
        refresh();
        int ch = getch();
        if (ch == 'y' || ch == 'Y') {
            target_file = swp_filename;
            snprintf(status_msg, sizeof(status_msg), "Recovered from swap file.");
        }
    }

    /* Free existing lines */
    for (int i = 0; i < line_count; i++) { free(lines[i]); lines[i] = NULL; }
    line_count = 0;

    tracker_clear();

    FILE *fp = fopen(target_file, "r");
    if (fp) {
        char *buf = NULL;
        size_t buf_sz = 0;
        ssize_t n;
        while ((n = getline(&buf, &buf_sz, fp)) != -1) {
            /* Strip trailing newline */
            while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
                buf[--n] = '\0';
            if (!ensure_capacity(line_count + 1)) break;
            lines[line_count] = xstrdup(buf);
            line_count++;
        }
        free(buf);
        fclose(fp);
        if (line_count == 0) {
            ensure_capacity(1);
            lines[0] = xstrdup("");
            line_count = 1;
        }
        strncpy(current_filename, filename, sizeof(current_filename) - 1);
        scroll_y = 0; current_line = 0; cursor_x = 0; is_modified = 0;
    } else {
        strncpy(current_filename, filename, sizeof(current_filename) - 1);
        ensure_capacity(1);
        lines[0] = xstrdup("");
        line_count = 1;
        is_modified = 0;
    }
    /* Record mtime so we can detect external changes */
    {
        struct stat st;
        file_mtime = (stat(current_filename, &st) == 0) ? st.st_mtime : 0;
    }
    syntax_set_file(current_filename);
    syntax_scan(lines, line_count);
}

void interactive_open() {
    char filename[256];
    int idx = 0;
    filename[0] = '\0';
    const char *prompt = "Enter filename to open (ESC to cancel): ";
    int prompt_len = strlen(prompt);
    noecho();
    mvprintw(LINES - 1, 0, "%s", prompt);
    clrtoeol();
    refresh();
    while (idx < (int)sizeof(filename) - 1) {
        int ch = getch();
        if (ch == 27) { status_msg[0] = '\0'; return; }
        else if (ch == 10 || ch == 13) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--; filename[idx] = '\0';
                mvprintw(LINES - 1, prompt_len + idx, " ");
                move(LINES - 1, prompt_len + idx); refresh();
            }
        } else if (ch >= 32 && ch <= 126) {
            filename[idx++] = (char)ch; filename[idx] = '\0';
            mvprintw(LINES - 1, prompt_len + idx - 1, "%c", ch); refresh();
        }
    }
    if (strlen(filename) > 0) {
        FILE *fp = fopen(filename, "r");
        if (fp) { fclose(fp); load_file(filename); }
        else {
            mvprintw(LINES - 1, 0, "File not found! Press any key...");
            clrtoeol(); refresh(); getch();
        }
    }
}

void save_file() {
    if (strcmp(current_filename, "untitled.txt") == 0) {
        char filename[256];
        int fidx = 0;
        filename[0] = '\0';
        const char *prompt = "Enter filename to save: ";
        int prompt_len = (int)strlen(prompt);
        noecho();
        mvprintw(LINES - 1, 0, "%s", prompt); clrtoeol(); refresh();
        while (fidx < (int)sizeof(filename) - 1) {
            int c = getch();
            if (c == 27) { status_msg[0] = '\0'; return; }
            else if (c == 10 || c == 13) break;
            else if (c == KEY_BACKSPACE || c == 127 || c == 8) {
                if (fidx > 0) {
                    fidx--; filename[fidx] = '\0';
                    mvprintw(LINES - 1, prompt_len + fidx, " ");
                    move(LINES - 1, prompt_len + fidx); refresh();
                }
            } else if (c >= 32 && c <= 126) {
                filename[fidx++] = (char)c; filename[fidx] = '\0';
                mvprintw(LINES - 1, prompt_len + fidx - 1, "%c", c); refresh();
            }
        }
        if (fidx == 0) return;
        strncpy(current_filename, filename, sizeof(current_filename) - 1);
        current_filename[sizeof(current_filename) - 1] = '\0';
    }
    FILE *fp = fopen(current_filename, "w");
    if (fp) {
        for (int i = 0; i < line_count; i++) {
            int len = strlen(lines[i]);
            while (len > 0 && (lines[i][len-1] == ' ' || lines[i][len-1] == '\t'))
                lines[i][--len] = '\0';
            fprintf(fp, "%s\n", lines[i]);
        }
        fclose(fp);
        char swp_filename[300];
        snprintf(swp_filename, sizeof(swp_filename), ".%s.swp", current_filename);
        unlink(swp_filename);
        is_modified = 0;
        tracker_clear();
        /* Update mtime after save so we don't falsely warn about our own write */
        {
            struct stat st;
            file_mtime = (stat(current_filename, &st) == 0) ? st.st_mtime : 0;
        }
        snprintf(status_msg, sizeof(status_msg), "Saved successfully to '%s'!", current_filename);
    } else {
        mvprintw(LINES - 1, 0, "Error: Could not save file! Press any key...");
        clrtoeol(); refresh(); getch();
    }
}

void auto_save() {
    char swp_filename[300];
    snprintf(swp_filename, sizeof(swp_filename), ".%s.swp", current_filename);
    FILE *fp = fopen(swp_filename, "w");
    if (fp) {
        for (int i = 0; i < line_count; i++)
            fprintf(fp, "%s\n", lines[i]);
        fclose(fp);
    }
}

/* ---------- bracket matching ---------- */

/* Search forward or backward through lines[] for the bracket that closes/opens
 * the one at (start_line, start_col). Writes the result into match_line/match_col.
 * Returns 1 on success, 0 if no match found. */
static int find_matching_bracket(int start_line, int start_col) {
    const char *open  = "([{";
    const char *close = ")]}";
    char ch = lines[start_line][start_col];
    int dir = 0;   /* +1 = forward, -1 = backward */
    char target = 0;

    for (int i = 0; i < 3; i++) {
        if (ch == open[i])  { dir =  1; target = close[i]; break; }
        if (ch == close[i]) { dir = -1; target = open[i];  break; }
    }
    if (dir == 0) return 0;

    int depth = 1;
    int l = start_line, c = start_col + dir;

    while (l >= 0 && l < line_count) {
        int len = (int)strlen(lines[l]);
        while (c >= 0 && c < len) {
            char cur = lines[l][c];
            if (cur == ch)     depth++;
            else if (cur == target) {
                depth--;
                if (depth == 0) { match_line = l; match_col = c; return 1; }
            }
            c += dir;
        }
        l += dir;
        if (l >= 0 && l < line_count)
            c = (dir == 1) ? 0 : (int)strlen(lines[l]) - 1;
    }
    return 0;
}

/* Called each frame: update match_line/match_col based on cursor position. */
static void update_bracket_match(void) {
    match_line = -1; match_col = -1;
    if (current_line < 0 || current_line >= line_count) return;
    int len = (int)strlen(lines[current_line]);
    if (cursor_x < 0 || cursor_x >= len) return;
    char ch = lines[current_line][cursor_x];
    if (ch == '(' || ch == ')' || ch == '[' ||
        ch == ']' || ch == '{' || ch == '}')
        find_matching_bracket(current_line, cursor_x);
}

/* ---------- screen rendering ---------- */
void draw_screen() {
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_CYAN, COLOR_BLACK);
    init_pair(4, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(5, COLOR_GREEN, COLOR_BLACK);
    init_pair(6, COLOR_YELLOW, COLOR_BLACK);
    init_pair(7, COLOR_BLACK, COLOR_YELLOW);  /* bracket highlight: black on yellow */

    update_bracket_match();

    move(0, 0); clrtoeol();
    attron(COLOR_PAIR(1));
    if (is_modified)
        printw(" File: %s * (unsaved) (%d lines)", current_filename, line_count);
    else
        printw(" File: %s (%d lines)", current_filename, line_count);
    attroff(COLOR_PAIR(1));

    move(1, 0); clrtoeol();
    for (int x = 0; x < COLS; x++) mvaddch(1, x, ACS_HLINE);

    int max_displayable_lines = LINES - 4;
    int physical_row = 2;
    /* wrap one column before the terminal edge to prevent ncurses auto-scroll */
    int wrap_col = COLS - 1;
    int file_line_index = scroll_y;

    /* Gutter width scales with the number of digits in line_count */
    int gutter_digits = 1;
    { int tmp = line_count; while (tmp >= 10) { tmp /= 10; gutter_digits++; } }
    /* gutter layout: <digits> + 1 space + 1 marker + 1 space = digits+3 cols; text starts at digits+3 */
    int gutter_width = gutter_visible ? (gutter_digits + 3) : 0;

    while (physical_row < 2 + max_displayable_lines && file_line_index < line_count) {
        move(physical_row, 0); clrtoeol();

        if (gutter_visible) {
            if (file_line_index == current_line) {
                attron(COLOR_PAIR(1));
                mvprintw(physical_row, 0, "%*d ", gutter_digits, file_line_index + 1);
                attroff(COLOR_PAIR(1));
                attron(COLOR_PAIR(1) | A_BOLD);
                mvaddch(physical_row, gutter_digits + 1, ACS_DIAMOND);
                attroff(COLOR_PAIR(1) | A_BOLD);
            } else {
                mvprintw(physical_row, 0, "%*d ", gutter_digits, file_line_index + 1);
                mvaddch(physical_row, gutter_digits + 1, ACS_VLINE);
            }
        }

        char *line = lines[file_line_index];
        int len = strlen(line);
        int disp_len = utf8_display_width(line);  /* display columns, not bytes */
        int current_phys_row = physical_row;
        int current_phys_col = gutter_width;

        move(current_phys_row, current_phys_col);

        /* Get syntax spans for this line */
        Span spans[MAX_SPANS];
        int nspans = syntax_highlight_enabled ? syntax_spans(file_line_index, line, spans) : 0;

        /* Colour-pair lookup: TOK -> ncurses pair */
        static const int tok_pair[] = { 0, 2, 4, 5, 3 };

        /* Render character by character, applying span colours */
        /* Show '>' at right edge of first visual row if line extends beyond terminal */
        if (disp_len > wrap_col - gutter_width) {
            attron(A_DIM);
            mvaddch(physical_row, wrap_col - 1, '>');
            attroff(A_DIM);
            move(physical_row, gutter_width);
        }

        int span_idx = 0;
        int j = 0;  /* byte index */
        while (j < len) {
            if (current_phys_col >= wrap_col) {
                current_phys_row++; current_phys_col = gutter_width;
                if (current_phys_row < 2 + max_displayable_lines) {
                    move(current_phys_row, current_phys_col);
                    clrtoeol();
                } else break;
            }
            /* Determine byte length of this UTF-8 codepoint */
            unsigned char ub = (unsigned char)line[j];
            int clen = 1;
            if      ((ub & 0xF8) == 0xF0) clen = 4;
            else if ((ub & 0xF0) == 0xE0) clen = 3;
            else if ((ub & 0xE0) == 0xC0) clen = 2;
            /* Clamp to remaining bytes */
            if (j + clen > len) clen = len - j;
            /* Display width of this codepoint */
            int cw = utf8_display_width_n(line + j, clen);
            if (cw < 1) cw = 1;
            /* Bracket highlight takes priority */
            int is_bracket_cursor = (file_line_index == current_line && j == cursor_x &&
                                     match_line >= 0);
            int is_bracket_match  = (file_line_index == match_line && j == match_col);
            if (is_bracket_cursor || is_bracket_match) {
                attron(COLOR_PAIR(7) | A_BOLD);
                for (int b = 0; b < clen; b++) printw("%c", line[j + b]);
                attroff(COLOR_PAIR(7) | A_BOLD);
                current_phys_col += cw;
                j += clen;
                continue;
            }
            /* Advance past expired spans */
            while (span_idx < nspans && spans[span_idx].end <= j)
                span_idx++;
            /* Apply colour if inside an active span */
            int pair = 0;
            if (span_idx < nspans && j >= spans[span_idx].start && j < spans[span_idx].end)
                pair = tok_pair[spans[span_idx].type];
            if (pair) attron(COLOR_PAIR(pair));
            for (int b = 0; b < clen; b++) printw("%c", line[j + b]);
            if (pair) attroff(COLOR_PAIR(pair));
            current_phys_col += cw;
            j += clen;
        }
        physical_row = current_phys_row + 1;
        file_line_index++;
    }

    /* clear any remaining rows below the last rendered line */
    while (physical_row < 2 + max_displayable_lines) {
        move(physical_row, 0); clrtoeol();
        physical_row++;
    }

    /* Column-81 margin guide (only when gutter is visible) */
    if (gutter_visible && COLS > 81) {
        for (int r = 2; r < LINES - 2; r++) {
            chtype ch_at = mvinch(r, 81);
            if ((ch_at & A_CHARTEXT) == ' ') {
                attron(A_DIM);
                mvaddch(r, 81, ACS_VLINE);
                attroff(A_DIM);
            }
        }
    }

    /* Status bar */
    move(LINES - 2, 0); clrtoeol();
    for (int x = 0; x < COLS; x++) mvaddch(LINES - 2, x, ACS_HLINE);
    move(LINES - 1, 0); clrtoeol();
    if (strlen(status_msg) > 0) {
        attron(COLOR_PAIR(1));
        mvprintw(LINES - 1, 0, "%.*s", COLS - 1, status_msg);
        attroff(COLOR_PAIR(1));
    } else {
        int gd_s = 1; { int tmp = line_count; while (tmp >= 10) { tmp /= 10; gd_s++; } }
        int tw_s = COLS - 1 - (gd_s + 3);
        int vis_col = (tw_s > 0) ? (cursor_x % tw_s) + 1 : cursor_x + 1;
        if (!is_modified) {
            mvprintw(LINES - 1, 0,
                "qi v%s  |  Ln: %d  Col: %d  |  ^? for Help",
                VERSION, current_line + 1, vis_col);
        } else {
            int total_chars = 0, modified_lines = 0;
            for (int i = 0; i < line_count; i++) {
                total_chars += (int)strlen(lines[i]);
                if (tracker_is_modified(i, 0)) modified_lines++;
            }
            mvprintw(LINES - 1, 0,
                "Lines Mod: %d | Total Chars: %d | Ln: %d Col: %d | (^? for Help)",
                modified_lines, total_chars, current_line + 1, vis_col);
        }
    }

    /* Cursor placement — gutter_width matches the dynamic gutter computed above */
    int gd2 = 1; { int tmp = line_count; while (tmp >= 10) { tmp /= 10; gd2++; } }
    int gw2 = gutter_visible ? (gd2 + 3) : 0;
    int text_width2 = COLS - 1 - gw2;
    int cursor_physical_row = 2;
    for (int i = scroll_y; i < current_line; i++) {
        int l_dw = utf8_display_width(lines[i]);
        int l_rows = (l_dw == 0) ? 1 : (l_dw / text_width2) + 1;
        cursor_physical_row += l_rows;
    }
    cursor_physical_row += (cursor_x / text_width2);
    int cursor_physical_col = gw2 + (cursor_x % text_width2);

    if (cursor_physical_row < LINES - 2) move(cursor_physical_row, cursor_physical_col);
    else move(LINES - 3, COLS - 1);

    refresh();
}

/* ---------- find / replace ---------- */
void find_text() {
    char search_str[128];
    int idx = 0;
    search_str[0] = '\0';
    const char *prompt = "Find: ";
    int prompt_len = strlen(prompt);

    noecho();
    mvprintw(LINES - 1, 0, "\045s", prompt);
    clrtoeol();
    refresh();

    while (idx < (int)sizeof(search_str) - 1) {
        int ch = getch();
        if (ch == 27) { status_msg[0] = '\0'; return; }
        else if (ch == 10 || ch == 13) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--; search_str[idx] = '\0';
                mvprintw(LINES - 1, prompt_len + idx, " ");
                move(LINES - 1, prompt_len + idx); refresh();
            }
        } else if (ch >= 32 && ch <= 126) {
            search_str[idx++] = (char)ch; search_str[idx] = '\0';
            mvprintw(LINES - 1, prompt_len + idx - 1, "\045c", ch); refresh();
        }
    }
    if (strlen(search_str) == 0) return;

    struct { int line; int col; } matches[500];
    int match_count = 0, current_match_idx = 0;

    /* Build match list across the entire file */
    for (int i = 0; i < line_count; i++) {
        char lower_line[MAX_LINE_LEN], lower_search[128];
        int ll = strlen(lines[i]);
        if (ll >= MAX_LINE_LEN) ll = MAX_LINE_LEN - 1;
        for (int j = 0; j < ll; j++)
            lower_line[j] = tolower((unsigned char)lines[i][j]);
        lower_line[ll] = '\0';
        int sl = strlen(search_str);
        for (int j = 0; j < sl && j < 127; j++)
            lower_search[j] = tolower((unsigned char)search_str[j]);
        lower_search[sl] = '\0';
        char *ptr = lower_line;
        while ((ptr = strstr(ptr, lower_search)) != NULL) {
            if (match_count < 500) {
                matches[match_count].line = i;
                matches[match_count].col = (int)(ptr - lower_line);
                match_count++;
            }
            ptr++;
        }
    }

    if (match_count == 0) {
        snprintf(status_msg, sizeof(status_msg), "No matches found for '\045s'.", search_str);
        return;
    }

    /* Start navigation from the first match at or after the current cursor position */
    int start_idx = 0;
    for (int k = 0; k < match_count; k++) {
        if (matches[k].line > current_line ||
           (matches[k].line == current_line && matches[k].col >= cursor_x)) {
            start_idx = k;
            break;
        }
    }
    current_match_idx = start_idx;

    /* Interactive match navigation loop */
    while (1) {
        current_line = matches[current_match_idx].line;
        cursor_x = matches[current_match_idx].col;
        int max_displayable_lines = LINES - 4;
        scroll_y = current_line - (max_displayable_lines / 2);
        if (scroll_y < 0) scroll_y = 0;
        if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
        if (scroll_y < 0) scroll_y = 0;

        draw_screen();
        snprintf(status_msg, sizeof(status_msg),
                 "Match \045d of \045d [Next: Right/Down | Prev: Left/Up | Enter: Done]",
                 current_match_idx + 1, match_count);
        draw_screen();

        int ch = getch();
        if (ch == 10 || ch == 13) {
            snprintf(status_msg, sizeof(status_msg), "Found match at line \045d.", current_line + 1);
            break;
        } else if (ch == 27) {
            status_msg[0] = '\0';
            break;
        } else if (ch == KEY_RIGHT || ch == KEY_DOWN) {
            current_match_idx = (current_match_idx + 1) % match_count;
        } else if (ch == KEY_LEFT || ch == KEY_UP) {
            current_match_idx = (current_match_idx - 1 + match_count) % match_count;
        }
    }
}

void replace_text() {
    char search_str[128], replace_str[128];
    int idx = 0;
    search_str[0] = replace_str[0] = '\0';
    noecho();

    const char *prompt1 = "Find text to replace: ";
    int p1_len = strlen(prompt1);
    mvprintw(LINES - 1, 0, "%s", prompt1); clrtoeol(); refresh();
    while (idx < (int)sizeof(search_str) - 1) {
        int ch = getch();
        if (ch == 27) { status_msg[0] = '\0'; return; }
        else if (ch == 10 || ch == 13) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) { idx--; search_str[idx] = '\0'; mvprintw(LINES-1,p1_len+idx," "); move(LINES-1,p1_len+idx); refresh(); }
        } else if (ch >= 32 && ch <= 126) { search_str[idx++]=(char)ch; search_str[idx]='\0'; mvprintw(LINES-1,p1_len+idx-1,"%c",ch); refresh(); }
    }
    if (strlen(search_str) == 0) return;

    idx = 0;
    const char *prompt2 = "Replace with: ";
    int p2_len = strlen(prompt2);
    mvprintw(LINES - 1, 0, "%s", prompt2); clrtoeol(); refresh();
    while (idx < (int)sizeof(replace_str) - 1) {
        int ch = getch();
        if (ch == 27) { status_msg[0] = '\0'; return; }
        else if (ch == 10 || ch == 13) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) { idx--; replace_str[idx]='\0'; mvprintw(LINES-1,p2_len+idx," "); move(LINES-1,p2_len+idx); refresh(); }
        } else if (ch >= 32 && ch <= 126) { replace_str[idx++]=(char)ch; replace_str[idx]='\0'; mvprintw(LINES-1,p2_len+idx-1,"%c",ch); refresh(); }
    }

    struct { int line; int col; } matches[500];
    int match_count = 0;
    for (int i = 0; i < line_count; i++) {
        char *ptr = lines[i];
        while ((ptr = strstr(ptr, search_str)) != NULL) {
            if (match_count < 500) { matches[match_count].line = i; matches[match_count].col = (int)(ptr - lines[i]); match_count++; }
            ptr += strlen(search_str);
        }
    }
    if (match_count == 0) { snprintf(status_msg, sizeof(status_msg), "No matches found for '%s'.", search_str); return; }

    save_undo_state_batch(0, line_count);
    int current_idx = 0, replaced_count = 0, force_all = 0;
    while (current_idx < match_count) {
        int ln = matches[current_idx].line;
        int col = matches[current_idx].col;
        current_line = ln; cursor_x = col;
        int max_displayable_lines = LINES - 4;
        scroll_y = current_line - (max_displayable_lines / 2);
        if (scroll_y < 0) scroll_y = 0;

        int choice = 'n';
        if (!force_all) {
            snprintf(status_msg, sizeof(status_msg),
                     "Match %d of %d: Replace? (y: Yes | n: No | a: All | q: Quit/ESC)",
                     current_idx + 1, match_count);
            draw_screen(); choice = getch();
        } else { choice = 'y'; }

        if (choice == 'q' || choice == 27) break;
        if (choice == 'a') { force_all = 1; choice = 'y'; }
        if (choice == 'y') {
            int search_len = strlen(search_str);
            int replace_len = strlen(replace_str);
            int old_len = strlen(lines[ln]);
            int new_len = old_len - search_len + replace_len;
            char *tmp = malloc(new_len + 1);
            if (tmp) {
                memcpy(tmp, lines[ln], col);
                memcpy(tmp + col, replace_str, replace_len);
                memcpy(tmp + col + replace_len, lines[ln] + col + search_len,
                       old_len - col - search_len + 1);
                free(lines[ln]); lines[ln] = tmp;
                replaced_count++;
                int delta = replace_len - search_len;
                for (int j = current_idx + 1; j < match_count && matches[j].line == ln; j++)
                    matches[j].col += delta;
            }
        }
        current_idx++;
    }
    if (replaced_count > 0)
        snprintf(status_msg, sizeof(status_msg), "Replaced %s with %s (%d instance%s)",
                 search_str, replace_str, replaced_count, replaced_count == 1 ? "" : "s");
    else
        snprintf(status_msg, sizeof(status_msg), "No replacements made.");
}

/* ---------- goto line ---------- */
void goto_line() {
    char line_input[32]; int idx = 0; line_input[0] = '\0';
    const char *prompt = "Go to line: "; int prompt_len = strlen(prompt);
    echo(); mvprintw(LINES-1,0,"%s",prompt); clrtoeol(); refresh();
    while (idx < (int)sizeof(line_input) - 1) {
        int ch = getch();
        if (ch == 27) { noecho(); status_msg[0]='\0'; return; }
        else if (ch == 10 || ch == 13) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) { idx--; line_input[idx]='\0'; mvprintw(LINES-1,prompt_len+idx," "); move(LINES-1,prompt_len+idx); refresh(); }
        } else if (isdigit((unsigned char)ch)) { line_input[idx++]=(char)ch; line_input[idx]='\0'; }
    }
    noecho();
    if (strlen(line_input) == 0) return;
    int target = atoi(line_input);
    if (target < 1 || target > line_count) {
        snprintf(status_msg, sizeof(status_msg), "Line %d out of bounds! (Total lines: %d)", target, line_count); return;
    }
    current_line = target - 1; cursor_x = 0;
    int max_displayable_lines = LINES - 4;
    scroll_y = current_line - (max_displayable_lines / 2);
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
    if (scroll_y < 0) scroll_y = 0;
}

/* ---------- delete lines ---------- */
void delete_lines_interactive() {
    char input[256]; int idx = 0; input[0] = '\0';
    const char *prompt = "Delete lines (e.g., 3, 5, 10-25 or !20-25): "; int prompt_len = strlen(prompt);

    /* Keep noecho active so ncurses never prints ^D or control characters */
    noecho();
    mvprintw(LINES - 1, 0, "\045s", prompt);
    clrtoeol();
    refresh();

    while (idx < (int)sizeof(input) - 1) {
        int ch = getch();
        if (ch == 27) { status_msg[0] = '\0'; return; }
        else if (ch == 10 || ch == 13) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--;
                input[idx] = '\0';
                mvprintw(LINES - 1, prompt_len + idx, " ");
                move(LINES - 1, prompt_len + idx);
                refresh();
            }
        } else if (ch >= 32 && ch <= 126) {
            input[idx++] = (char)ch;
            input[idx] = '\0';
            mvprintw(LINES - 1, prompt_len + idx - 1, "\045c", ch);
            refresh();
        }
    }

    if (strlen(input) == 0) return;

    char saved_input_copy[256];
    strncpy(saved_input_copy, input, sizeof(saved_input_copy) - 1);
    saved_input_copy[sizeof(saved_input_copy) - 1] = '\0';

    /* Build deletion bitmap  use heap to avoid VLA issues on large files */
    char *to_delete = calloc(line_count, sizeof(char));
    if (!to_delete) return;

    save_undo_state_batch(0, line_count);

    /* Check for leading '!' inversion operator */
    char *p = input;
    while (*p == ' ' || *p == '\t') p++; /* Skip leading whitespace */

    int invert = 0;
    if (*p == '!') {
        invert = 1;
        p++; /* Skip the '!' character */
    }

    char *token = strtok(p, ",");
    while (token != NULL) {
        while (*token == ' ' || *token == '\t') token++;
        int start = 0, end = 0;
        if (sscanf(token, "\045d-\045d", &start, &end) == 2) {
            if (start > 0 && end >= start)
            for (int i = start; i <= end && i <= line_count; i++) to_delete[i - 1] = 1;
        } else if (sscanf(token, "\045d", &start) == 1) {
            if (start > 0 && start <= line_count) to_delete[start - 1] = 1;
        }
        token = strtok(NULL, ",");
    }

    /* Apply '!' inversion if specified */
    if (invert) {
        for (int i = 0; i < line_count; i++) {
            to_delete[i] = !to_delete[i];
        }
    }

    int deleted_count = 0;
    for (int i = line_count - 1; i >= 0; i--) {
        if (to_delete[i]) {
            remove_line_at(i);
            deleted_count++;
            if (i <= current_line && current_line > 0) current_line--;
        }
    }
    free(to_delete);

    if (line_count == 0) {
        ensure_capacity(1); lines[0] = xstrdup(""); line_count = 1;
        current_line = 0; cursor_x = 0;
    }
    int len = strlen(lines[current_line]);
    if (cursor_x > len) cursor_x = len;
    int max_displayable_lines = LINES - 4;
    if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
    if (scroll_y < 0) scroll_y = 0;

    if (deleted_count > 0) {
        is_modified = 1;
        snprintf(status_msg, sizeof(status_msg), "Deleted lines \045s", saved_input_copy);
    } else {
        snprintf(status_msg, sizeof(status_msg), "No lines deleted.");
    }
}

/* ---------- help window ---------- */
void show_help_window() {
    /* Categorised, scrollable help entries.
     * NULL entries are section headers; empty string "" is a blank spacer. */
    struct HelpEntry { const char *key; const char *desc; int is_header; };
    static const struct HelpEntry entries[] = {
        { "FILE",          NULL,                         1 },
        { "Ctrl+S",        "Save file",                  0 },
        { "Ctrl+O",        "Open file",                  0 },
        { "Ctrl+Q",        "Quit",                       0 },
        { "",              "",                            0 },
        { "SEARCH",        NULL,                         1 },
        { "Ctrl+F",        "Find text",                  0 },
        { "Ctrl+R",        "Find & Replace",             0 },
        { "Ctrl+G",        "Go to line",                 0 },
        { "",              "",                            0 },
        { "UNDO / REDO",   NULL,                         1 },
        { "Ctrl+U",        "Undo",                       0 },
        { "Ctrl+Y",        "Redo",                       0 },
        { "",              "",                            0 },
        { "EDITING",       NULL,                         1 },
        { "Ctrl+D",        "Delete line(s)",             0 },
        { "Ctrl+K",        "Cut line",                   0 },
        { "Ctrl+P",        "Paste line",                 0 },
        { "Ctrl+W",        "Delete word left",           0 },
        { "Ctrl+N",        "Duplicate line",             0 },
        { "Tab",           "Indent",                     0 },
        { "Shift+Tab",     "Dedent",                     0 },
        { "",              "",                            0 },
        { "NAVIGATION",    NULL,                         1 },
        { "Ctrl+T",        "Top of file",                0 },
        { "Ctrl+B",        "Bottom of file",             0 },
        { "Ctrl+A",        "Line start (smart)",         0 },
        { "Ctrl+E",        "Line end",                   0 },
        { "%",             "Jump to matching bracket",   0 },
        { "",              "",                            0 },
        { "VIEW",          NULL,                         1 },
        { "F4",            "Toggle gutter / col-81",     0 },
        { "F5",            "Toggle syntax highlight",    0 },
        { "Ctrl+X",        "Toggle Insert/Overwrite",    0 },
        { "Ctrl+?",        "This help screen",           0 },
    };
    int total = (int)(sizeof(entries) / sizeof(entries[0]));

    int win_h = 22, win_w = 46;
    if (win_h > LINES - 2) win_h = LINES - 2;
    int inner_h = win_h - 6; /* rows available for scrolling content (2 borders + 4 footer rows) */
    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS  - win_w) / 2;
    WINDOW *hw = newwin(win_h, win_w, start_y, start_x);
    keypad(hw, TRUE);

    int scroll = 0;
    for (;;) {
        werase(hw);
        box(hw, 0, 0);

        /* Title: ─────┤ qi vX.X.X ├───── on top border row */
        {
            char title[32];
            snprintf(title, sizeof(title), " qi v%s ", VERSION);
            int tlen = (int)strlen(title);
            int tx = (win_w - tlen) / 2;
            mvwaddch(hw, 0, tx - 1,    ACS_RTEE);
            mvwaddch(hw, 0, tx + tlen, ACS_LTEE);
            wattron(hw, COLOR_PAIR(1) | A_BOLD);
            mvwprintw(hw, 0, tx, "%s", title);
            wattroff(hw, COLOR_PAIR(1) | A_BOLD);
        }

        /* Render visible entries */
        int row = 1;
        int rendered = 0;
        for (int i = 0; i < total && row < 1 + inner_h; i++) {
            if (rendered < scroll) { rendered++; continue; }
            if (entries[i].is_header) {
                wattron(hw, A_BOLD | A_UNDERLINE);
                mvwprintw(hw, row, 2, "%s", entries[i].key);
                wattroff(hw, A_BOLD | A_UNDERLINE);
            } else if (entries[i].key[0] == '\0') {
                /* blank spacer */
            } else {
                mvwprintw(hw, row, 2,  "%-14s", entries[i].key);
                mvwprintw(hw, row, 16, "%s", entries[i].desc);
            }
            row++;
            rendered++;
        }

        /* Scroll indicator */
        if (scroll > 0)
            mvwprintw(hw, 1, win_w - 4, " ^ ");
        if (scroll + inner_h < total)
            mvwprintw(hw, inner_h, win_w - 4, " v ");

        /* Footer: separator, scroll hint, close hint — all above the bottom bump row */
        for (int x = 1; x < win_w - 1; x++) mvwaddch(hw, win_h - 5, x, ACS_HLINE);
        wattron(hw, A_DIM);
        mvwprintw(hw, win_h - 4, 2, "Arrow keys to scroll");
        wattroff(hw, A_DIM);
        wattron(hw, COLOR_PAIR(1));
        mvwprintw(hw, win_h - 3, 2, "Press any key to close...");
        wattroff(hw, COLOR_PAIR(1));
        /* Copyright: ─────┤ (c) 2026 ... ├───── on bottom border row */
        {
            char copy[48];
            snprintf(copy, sizeof(copy), " (c) 2026 Christopher Camacho ");
            int clen = (int)strlen(copy);
            int cx = (win_w - clen) / 2;
            mvwaddch(hw, win_h - 1, cx - 1,    ACS_RTEE);
            mvwaddch(hw, win_h - 1, cx + clen, ACS_LTEE);
            wattron(hw, A_DIM);
            mvwprintw(hw, win_h - 1, cx, "%s", copy);
            wattroff(hw, A_DIM);
        }

        wrefresh(hw);

        int ch = wgetch(hw);
        if (ch == KEY_UP   || ch == 'k') { if (scroll > 0) scroll--; }
        else if (ch == KEY_DOWN || ch == 'j') { if (scroll + inner_h < total) scroll++; }
        else if (ch == KEY_PPAGE) { scroll -= inner_h; if (scroll < 0) scroll = 0; }
        else if (ch == KEY_NPAGE) { scroll += inner_h; if (scroll + inner_h > total) scroll = total - inner_h; if (scroll < 0) scroll = 0; }
        else break;
    }
    delwin(hw);
    touchwin(stdscr);
    refresh();
}

/* ---------- main ---------- */
int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");  /* enable UTF-8 locale for wcwidth() */
    struct termios tty;
    if (tcgetattr(STDIN_FILENO, &tty) == 0) {
        tty.c_iflag &= ~IXON;
        tcsetattr(STDIN_FILENO, TCSANOW, &tty);
    }
    printf("\e[?2004l"); fflush(stdout);

    initscr();
    set_escdelay(25);
    raw();
    noecho();
    keypad(stdscr, TRUE);

    /* enable terminal bracketed paste mode */
    printf("\033[?2004h");
    fflush(stdout);

    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    curs_set(1);
#ifndef BUTTON5_PRESSED
#define BUTTON5_PRESSED BUTTON2_PRESSED
#endif
    mousemask(BUTTON1_PRESSED | BUTTON4_PRESSED | BUTTON5_PRESSED, NULL);

    /* Install signal handlers for graceful shutdown */
    signal(SIGTERM, fatal_signal_handler);
    signal(SIGHUP,  fatal_signal_handler);

    /* Initialise tracker with a modest hint; it only uses 1 byte per line */
    tracker_init(1024, 1);

    undo_buf = calloc(UNDO_CAP, sizeof(UndoOp));
    redo_buf = calloc(UNDO_CAP, sizeof(UndoOp));

    /* Bootstrap with one empty line */
    ensure_capacity(1);
    lines[0] = xstrdup("");
    line_count = 1;

    if (argc > 1) {
        load_file(argv[1]);
        if (argc > 2 && (argv[2][0] == '+' || argv[2][0] == ':')) {
            int target_line = atoi(&argv[2][1]);
            if (target_line > 0 && target_line <= line_count) {
                current_line = target_line - 1; cursor_x = 0;
                int max_displayable_lines = LINES - 4;
                scroll_y = current_line - (max_displayable_lines / 2);
                if (scroll_y < 0) scroll_y = 0;
                if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
                if (scroll_y < 0) scroll_y = 0;
            }
        }
    } else {
        strcpy(current_filename, "untitled.txt");
    }

    clock_t last_input_time = 0;
    int paste_batch_active = 0;

    while (1) {
        draw_screen();
        int ch = getch();

        if (ch == 27) { /* ESC key */
            nodelay(stdscr, TRUE);
            int next1 = getch();
            int next2 = getch();

            if (next1 == '[' && next2 == '2') {
                char seq[16] = {0};
                int idx = 0;
                int c;
                while ((c = getch()) != ERR && idx < 10) {
                    seq[idx++] = c;
                    if (c == '~') break;
                }
                if (strcmp(seq, "00~") == 0) {
                    is_pasting = 1;
                    nodelay(stdscr, FALSE);
                    continue;
                } else if (strcmp(seq, "01~") == 0) {
                    is_pasting = 0;
                    nodelay(stdscr, FALSE);
                    continue;
                }
            }
            nodelay(stdscr, FALSE);
        }

        clock_t now = clock();
        double ms_since_last = ((double)(now - last_input_time) / CLOCKS_PER_SEC) * 1000.0;
        last_input_time = now;

        if (ms_since_last < 4.0 && ch != CTRL_KEY('u') && ch != CTRL_KEY('y') && last_input_time != 0) {
            if (!paste_batch_active) {
                save_undo_state_batch(current_line, line_count - current_line);
                paste_batch_active = 1;
            }
        } else {
            paste_batch_active = 0;
        }

        status_msg[0] = '\0';

        /* Handle SIGTERM / SIGHUP: save if modified, then exit */
        if (got_fatal_signal) {
            if (is_modified) save_file();
            break;
        }

        /* External file modification check (every draw cycle, lightweight) */
        if (file_mtime != 0 && strcmp(current_filename, "untitled.txt") != 0) {
            struct stat st;
            if (stat(current_filename, &st) == 0 && st.st_mtime != file_mtime) {
                file_mtime = st.st_mtime;  /* update so we only warn once */
                move(LINES - 1, 0); clrtoeol();
                attron(COLOR_PAIR(2));
                printw("File changed on disk! Reload? (y/n): ");
                attroff(COLOR_PAIR(2));
                refresh();
                int ans = getch();
                if (ans == 'y' || ans == 'Y') {
                    load_file(current_filename);
                    snprintf(status_msg, sizeof(status_msg), "Reloaded '%s' from disk.", current_filename);
                }
                continue;
            }
        }

        if (ch == CTRL_KEY('q')) {
            if (is_modified) {
                move(LINES-1,0); clrtoeol();
                attron(COLOR_PAIR(2));
                printw("Unsaved changes! Quit anyway? (y/n): ");
                attroff(COLOR_PAIR(2));
                refresh();
                int confirm = getch();
                if (confirm == 'y' || confirm == 'Y') break;
                else continue;
            } else break;
        }
        else if (ch == CTRL_KEY('o')) interactive_open();
        else if (ch == CTRL_KEY('s')) save_file();
        else if (ch == CTRL_KEY('w')) {
            /* Delete word to the left of the cursor */
            if (cursor_x > 0) {
                int orig = cursor_x;
                /* skip trailing spaces */
                while (cursor_x > 0 && isspace((unsigned char)lines[current_line][cursor_x-1])) cursor_x--;
                /* skip word chars */
                while (cursor_x > 0 && !isspace((unsigned char)lines[current_line][cursor_x-1])) cursor_x--;
                record_bulk(current_line, 1);
                int line_len = (int)strlen(lines[current_line]);
                memmove(lines[current_line] + cursor_x,
                        lines[current_line] + orig,
                        line_len - orig + 1);
                is_modified = 1;
            }
        }
        else if (ch == KEY_F(4)) {
            gutter_visible = !gutter_visible;
            snprintf(status_msg, sizeof(status_msg), "Gutter %s.",
                     gutter_visible ? "on" : "off");
        }
        else if (ch == KEY_F(5)) {
            syntax_highlight_enabled = !syntax_highlight_enabled;
            snprintf(status_msg, sizeof(status_msg), "Syntax highlighting %s.",
                     syntax_highlight_enabled ? "on" : "off");
        }
        else if (ch == CTRL_KEY('n')) {
            /* Duplicate current line */
            record_bulk(current_line, 1);
            insert_line_at(current_line + 1, lines[current_line]);
            current_line++;
            is_modified = 1;
            snprintf(status_msg, sizeof(status_msg), "Line duplicated.");
        }
        else if (ch == CTRL_KEY('f')) find_text();
        else if (ch == CTRL_KEY('?')) show_help_window();
        else if (ch == CTRL_KEY('r')) replace_text();
        else if (ch == CTRL_KEY('g')) goto_line();
        else if (ch == CTRL_KEY('u')) undo();
        else if (ch == CTRL_KEY('y')) redo_op();
        else if (ch == CTRL_KEY('d')) delete_lines_interactive();
        else if (ch == CTRL_KEY('x')) {
            overwrite_mode = !overwrite_mode;
            snprintf(status_msg, sizeof(status_msg), "Mode: %s", overwrite_mode ? "OVERWRITE" : "INSERT");
        }
        else if (ch == CTRL_KEY('t')) { current_line = 0; cursor_x = 0; scroll_y = 0; }
        else if (ch == CTRL_KEY('b')) {
            current_line = line_count - 1; cursor_x = 0;
            int max_displayable_lines = LINES - 4;
            scroll_y = current_line - max_displayable_lines + 1;
            if (scroll_y < 0) scroll_y = 0;
        }
        else if (ch == CTRL_KEY('k')) {
        /* Cut current line into clipboard */
            record_bulk(current_line, 1);

            // Fix 1: Ensure we only copy if the line actually has content
            if (lines[current_line] != NULL) {
                 free(clipboard_line);
                 clipboard_line = xstrdup(lines[current_line]);

                 // Safety check for memory allocation/empty results
                 if (clipboard_line == NULL || strlen(clipboard_line) == 0) {
                      clipboard_line = strdup(""); // Fallback to empty string if null
                 }
            }

            if (line_count > 1) {
                 remove_line_at(current_line);
                 // Ensure current_line stays within bounds after the shift
                 if (current_line >= line_count) {
                      current_line = line_count - 1;
                 }
            } else {
                 // For single-line files, clear content but maintain count
                 free(lines[0]);
                 lines[0] = xstrdup("");
            }

            cursor_x = 0;
            if (current_line < scroll_y) scroll_y = current_line;
            is_modified = 1;
            snprintf(status_msg, sizeof(status_msg), "Line cut.");
        }

        else if (ch == CTRL_KEY('p')) {
            /* Paste clipboard line above current line */
            if (clipboard_line) {
                record_bulk(current_line, line_count - current_line);
                insert_line_at(current_line, clipboard_line);
                cursor_x = 0;
                is_modified = 1;
                snprintf(status_msg, sizeof(status_msg), "Line pasted.");
            } else {
                snprintf(status_msg, sizeof(status_msg), "Clipboard is empty.");
            }
        }
        else if (ch == KEY_MOUSE) {
            MEVENT me;
            if (getmouse(&me) == OK) {
                int scroll_speed = 3;
                if (me.bstate & BUTTON4_PRESSED) {
                    scroll_y -= scroll_speed;
                    if (scroll_y < 0) scroll_y = 0;
                    if (current_line < scroll_y) current_line = scroll_y;
                } else if (me.bstate & BUTTON5_PRESSED) {
                    int mdl = LINES - 4;
                    scroll_y += scroll_speed;
                    if (scroll_y > line_count - 1) scroll_y = line_count - 1;
                    if (scroll_y < 0) scroll_y = 0;
                    if (current_line < scroll_y) current_line = scroll_y;
                    else if (current_line >= scroll_y + mdl) current_line = scroll_y + mdl - 1;
                } else if (me.bstate & BUTTON1_PRESSED) {
                int click_row = (int)me.y;
                int click_col = (int)me.x;
                /* rows 0 and 1 are header; rows LINES-2 and LINES-1 are status */
                if (click_row >= 2 && click_row < LINES - 2) {
                    int gd_m=1; { int tmp=line_count; while(tmp>=10){tmp/=10;gd_m++;} }
                    int gw_m = gutter_visible ? (gd_m + 3) : 0;
                    int text_width = COLS - 1 - gw_m;
                    /* Walk file lines from scroll_y, accumulating visual rows,
                     * to find which file line and column the click lands on. */
                    int phys = 2;
                    int found = 0;
                    for (int i = scroll_y; i < line_count && phys < LINES - 2; i++) {
                        int ll = utf8_display_width(lines[i]);
                        int vrows = (ll == 0) ? 1 : (ll / text_width) + 1;
                        if (click_row < phys + vrows) {
                            /* click is within this file line */
                            int row_within = click_row - phys;
                            int col_within = click_col - gw_m;
                            if (col_within < 0) col_within = 0;
                            int new_cx = row_within * text_width + col_within;
                            if (new_cx > ll) new_cx = ll;
                            current_line = i;
                            cursor_x = new_cx;
                            found = 1;
                            break;
                        }
                        phys += vrows;
                    }
                    /* click below last line: move to end of file */
                    if (!found && line_count > 0) {
                        current_line = line_count - 1;
                        cursor_x = (int)strlen(lines[current_line]);
                    }
                }
                } /* end BUTTON1_PRESSED */
            }
        }
        else if (ch == '%') {
            /* Jump to matching bracket */
            if (find_matching_bracket(current_line, cursor_x)) {
                current_line = match_line;
                cursor_x    = match_col;
                /* Scroll so the destination is visible */
                int mdl = LINES - 4;
                if (current_line < scroll_y)
                    scroll_y = current_line;
                else if (current_line >= scroll_y + mdl)
                    scroll_y = current_line - mdl + 1;
                if (scroll_y < 0) scroll_y = 0;
            } else {
                snprintf(status_msg, sizeof(status_msg), "No matching bracket.");
            }
        }
        else if (ch == KEY_UP) {
            if (current_line > 0) {
                current_line--;
                { int gd_u=1; int tmp=line_count; while(tmp>=10){tmp/=10;gd_u++;}
                int available_width = COLS - 1 - (gutter_visible ? (gd_u + 3) : 0);
                int visual_rows_above = 0;
                for (int i = scroll_y; i < current_line; i++) {
                    int l_dw = utf8_display_width(lines[i]);
                    visual_rows_above += (l_dw == 0) ? 1 : (l_dw / available_width) + 1;
                }
                if (visual_rows_above < 3 && scroll_y > 0) {
                    while (scroll_y > 0 && visual_rows_above < 3) {
                        scroll_y--;
                        visual_rows_above = 0;
                        for (int i = scroll_y; i < current_line; i++) {
                            int l_dw = utf8_display_width(lines[i]);
                            visual_rows_above += (l_dw == 0) ? 1 : (l_dw / available_width) + 1;
                        }
                    }
                } }
                int len = strlen(lines[current_line]);
                if (cursor_x > len) cursor_x = len;
            }
        }
        else if (ch == KEY_DOWN) {
            if (current_line < line_count - 1) {
                current_line++;
                int max_displayable_lines = LINES - 4;
                { int gd_d=1; int tmp=line_count; while(tmp>=10){tmp/=10;gd_d++;}
                int available_width = COLS - 1 - (gutter_visible ? (gd_d + 3) : 0);
                int visual_row_index = 0;
                for (int i = scroll_y; i <= current_line; i++) {
                    int l_dw = utf8_display_width(lines[i]);
                    int l_rows = (l_dw == 0) ? 1 : (l_dw / available_width) + 1;
                    if (i < current_line) visual_row_index += l_rows;
                    else visual_row_index += (utf8_display_width_n(lines[i], cursor_x) / available_width);
                }
                if (max_displayable_lines - visual_row_index <= 3) {
                    scroll_y++;
                    if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
                    if (scroll_y < 0) scroll_y = 0;
                } }
                int len = strlen(lines[current_line]);
                if (cursor_x > len) cursor_x = len;
            }
        }
        else if (ch == KEY_LEFT) { if (cursor_x > 0) cursor_x--; }
        else if (ch == KEY_RIGHT) { if (cursor_x < (int)strlen(lines[current_line])) cursor_x++; }
        else if (ch == 393 || ch == 545 || ch == 260 || ch == 543) {
            if (cursor_x > 0) {
                while (cursor_x > 0 && isspace((unsigned char)lines[current_line][cursor_x-1])) cursor_x--;
                while (cursor_x > 0 && !isspace((unsigned char)lines[current_line][cursor_x-1])) cursor_x--;
            }
            draw_screen(); continue;
        }
        else if (ch == 402 || ch == 560 || ch == 261 || ch == 544) {
            int len = strlen(lines[current_line]);
            if (cursor_x < len) {
                while (cursor_x < len && !isspace((unsigned char)lines[current_line][cursor_x])) cursor_x++;
                while (cursor_x < len && isspace((unsigned char)lines[current_line][cursor_x])) cursor_x++;
            }
            draw_screen(); continue;
        }
        else if (ch == KEY_PPAGE) {
            int max_displayable_lines = LINES - 4;
            current_line -= max_displayable_lines; if (current_line < 0) current_line = 0;
            scroll_y -= max_displayable_lines; if (scroll_y < 0) scroll_y = 0;
            int len = strlen(lines[current_line]); if (cursor_x > len) cursor_x = len;
        }
        else if (ch == KEY_NPAGE) {
            int max_displayable_lines = LINES - 4;
            current_line += max_displayable_lines; if (current_line >= line_count) current_line = line_count - 1;
            scroll_y += max_displayable_lines;
            if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
            if (scroll_y < 0) scroll_y = 0;
            int len = strlen(lines[current_line]); if (cursor_x > len) cursor_x = len;
        }
        else if (ch == KEY_HOME || ch == 1 || ch == 27) {
            if (ch == KEY_HOME || ch == 1) {
                cursor_x = 0;
            } else {
                int next1 = getch();
                if (next1 == 'b') {
                    if (cursor_x > 0) {
                        while (cursor_x > 0 && isspace((unsigned char)lines[current_line][cursor_x-1])) cursor_x--;
                        while (cursor_x > 0 && !isspace((unsigned char)lines[current_line][cursor_x-1])) cursor_x--;
                    }
                    continue;
                } else if (next1 == 'f') {
                    int len = strlen(lines[current_line]);
                    if (cursor_x < len) {
                        while (cursor_x < len && !isspace((unsigned char)lines[current_line][cursor_x])) cursor_x++;
                        while (cursor_x < len && isspace((unsigned char)lines[current_line][cursor_x])) cursor_x++;
                    }
                    continue;
                }
                int next2 = getch();
                if (next1 == '[' && (next2 == '1' || next2 == 'H')) {
                    if (next2 == '1') getch();
                    cursor_x = 0;
                } else if (next1 == '[' && (next2 == '4' || next2 == 'F')) {
                    if (next2 == '4') getch();
                    cursor_x = (int)strlen(lines[current_line]);
                } else { ungetch(next2); ungetch(next1); }
            }
        }
        else if (ch == KEY_END || ch == CTRL_KEY('e')) {
            cursor_x = (int)strlen(lines[current_line]);
        }
        else if (ch == KEY_HOME || ch == CTRL_KEY('a')) {
            /* First press: jump to first non-whitespace; second press: column 0 */
            int first_nws = 0;
            while (first_nws < (int)strlen(lines[current_line]) &&
                   isspace((unsigned char)lines[current_line][first_nws]))
                first_nws++;
            cursor_x = (cursor_x == first_nws) ? 0 : first_nws;
        }
        else if (ch == 9) {
            /* Tab — record as bulk (multi-char insert) */
            int len = strlen(lines[current_line]);
            int is_makefile = (strstr(current_filename, "Makefile") != NULL);
            int tab_size = is_makefile ? 1 : 4;
            char *new_line = malloc(len + tab_size + 1);
            if (new_line) {
                record_bulk(current_line, 1);
                memcpy(new_line, lines[current_line], cursor_x);
                if (is_makefile) new_line[cursor_x] = '\t';
                else memset(new_line + cursor_x, ' ', tab_size);
                memcpy(new_line + cursor_x + tab_size, lines[current_line] + cursor_x, len - cursor_x + 1);
                free(lines[current_line]); lines[current_line] = new_line;
                for (int i = 0; i < tab_size; i++)
                    tracker_set_modified(current_line, cursor_x + i, 1);
                cursor_x += tab_size;
                is_modified = 1; mod_count++;
            }
        }
        else if (ch == 353) {
            /* Shift+Tab — dedent: remove up to one tab-width of leading spaces */
            int is_makefile = (strstr(current_filename, "Makefile") != NULL);
            int tab_size = is_makefile ? 1 : 4;
            int removed = 0;
            record_bulk(current_line, 1);
            while (removed < tab_size && lines[current_line][0] == ' ') {
                int ll = (int)strlen(lines[current_line]);
                memmove(lines[current_line], lines[current_line] + 1, ll);
                removed++;
            }
            /* also handle a leading hard tab */
            if (removed == 0 && lines[current_line][0] == '\t') {
                int ll = (int)strlen(lines[current_line]);
                memmove(lines[current_line], lines[current_line] + 1, ll);
                removed = 1;
            }
            if (removed > 0) {
                if (cursor_x >= removed) cursor_x -= removed;
                else cursor_x = 0;
                is_modified = 1;
            }
        }
         else if (ch == 10 || ch == 13) {
            /* Enter — record line split */
            if (!paste_batch_active) record_line_split(current_line, cursor_x);

            /* Measure leading whitespace of the current line for auto-indent */
            int indent_len = 0;

            /* STEP 2B: Only calculate auto-indent if NOT currently pasting */
            if (!is_pasting) {
                while (lines[current_line][indent_len] == ' ' ||
                       lines[current_line][indent_len] == '\t')
                    indent_len++;
                /* Only auto-indent if cursor is at or past the indented region */
                if (cursor_x < indent_len) indent_len = 0;
            }

            char *tail_text = lines[current_line] + cursor_x;
            int tail_len = (int)strlen(tail_text);
            char *new_line = malloc(indent_len + tail_len + 1);
            if (new_line) {
                memcpy(new_line, lines[current_line], indent_len);
                memcpy(new_line + indent_len, tail_text, tail_len + 1);
            } else {
                new_line = xstrdup(tail_text);
                indent_len = 0;
            }
            lines[current_line][cursor_x] = '\0';
            insert_line_at(current_line + 1, new_line);
            free(new_line);
            current_line++; cursor_x = indent_len;
            is_modified = 1;

            /* Ensure the new line is always visible after Enter */
            int max_displayable_lines = LINES - 4;
            if (current_line >= scroll_y + max_displayable_lines) {
                scroll_y = current_line - max_displayable_lines + 1;
                if (scroll_y < 0) scroll_y = 0;
            }
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (cursor_x > 0) {
                /* Backspace within a line — record char deletion */
                record_char_del(current_line, cursor_x - 1, lines[current_line][cursor_x - 1]);
                int len = strlen(lines[current_line]);
                memmove(lines[current_line] + cursor_x - 1,
                        lines[current_line] + cursor_x,
                        len - cursor_x + 1);
                cursor_x--; is_modified = 1;
            } else if (current_line > 0) {
                /* Backspace at start of line — record line join */
                int target = current_line - 1;
                int target_len = strlen(lines[target]);
                int cur_len = strlen(lines[current_line]);
                char *merged = malloc(target_len + cur_len + 1);
                if (merged) {
                    record_line_join(target, target_len, lines[current_line]);
                    memcpy(merged, lines[target], target_len);
                    memcpy(merged + target_len, lines[current_line], cur_len + 1);
                    free(lines[target]); lines[target] = merged;
                    remove_line_at(current_line);
                    current_line--; cursor_x = target_len;
                    is_modified = 1;
                    if (current_line < scroll_y) scroll_y = current_line;
                }
            }
        }
        else if (ch == KEY_DC || ch == 330) {
            int len = strlen(lines[current_line]);
            if (cursor_x < len) {
                /* Delete key within a line — record char deletion */
                record_char_del(current_line, cursor_x, lines[current_line][cursor_x]);
                memmove(lines[current_line] + cursor_x,
                        lines[current_line] + cursor_x + 1,
                        len - cursor_x);
                is_modified = 1;
            } else if (current_line < line_count - 1) {
                /* Delete at end of line — record line join */
                int next = current_line + 1;
                int next_len = strlen(lines[next]);
                char *merged = malloc(len + next_len + 1);
                if (merged) {
                    record_line_join(current_line, len, lines[next]);
                    memcpy(merged, lines[current_line], len);
                    memcpy(merged + len, lines[next], next_len + 1);
                    free(lines[current_line]); lines[current_line] = merged;
                    remove_line_at(next);
                    is_modified = 1;
                }
            }
        }
        else if (ch >= 32 && ch <= 126) {
            int len = strlen(lines[current_line]);
            if (!paste_batch_active) {
                /* Record char insert; group consecutive chars on same line */
                record_char_ins(current_line, cursor_x, (char)ch);
            }
            if (overwrite_mode && cursor_x < len) {
                lines[current_line][cursor_x] = (char)ch;
            } else {
                char *new_line = malloc(len + 2);
                if (new_line) {
                    memcpy(new_line, lines[current_line], cursor_x);
                    new_line[cursor_x] = (char)ch;
                    memcpy(new_line + cursor_x + 1, lines[current_line] + cursor_x, len - cursor_x + 1);
                    free(lines[current_line]); lines[current_line] = new_line;
                }
            }
            tracker_set_modified(current_line, cursor_x, 1);
            cursor_x++; is_modified = 1;
            mod_count++;
            if (mod_count >= 50) { auto_save(); mod_count = 0; }
        }
    } /* end while(1) */

    for (int i = 0; i < undo_count; i++) free_op(&undo_buf[(undo_head + i) % UNDO_CAP]);
    free(undo_buf);
    for (int i = 0; i <= redo_top; i++) free_op(&redo_buf[i]);
    free(redo_buf);
    free(clipboard_line);
    tracker_free();
    for (int i = 0; i < line_count; i++) free(lines[i]);
    free(lines);

    /* disable bracketed paste mode before exiting */
    printf("\033[?20041");
    fflush(stdout);

    endwin();
    return 0;
}
