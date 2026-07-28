/*
 * qi - A Lightweight Terminal Text Editor
 * Author: Christopher Camacho
 * Version: 1.1.55 (2026)
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
#include <errno.h>
#include "tracker.h"
#include "syntax.h"
#include "color.h"

#define MAX_LINE_LEN 512
#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_UNDO 500
#define UNDO_CAP MAX_UNDO
#define VERSION "1.1.55"

/* Define keycodes for ALT/OPT key combos */
#ifndef KEY_ALT_S
#define KEY_ALT_S 0x1fe
#endif
#ifndef KEY_ALT_G
#define KEY_ALT_G 0x1fd
#endif

/* BUTTON5_PRESSED is absent from macOS system ncurses 5.7; fall back to
 * BUTTON2_PRESSED so the symbol is always defined. The real registration
 * is handled by ALL_MOUSE_EVENTS in mousemask(). */
#ifndef BUTTON5_PRESSED
#define BUTTON5_PRESSED BUTTON2_PRESSED
#endif

/* Both macOS (Homebrew ncurses) and Linux follow the xterm standard:
 * BUTTON4 = scroll up, BUTTON5 = scroll down. */
#define SCROLL_UP_BTN   BUTTON4_PRESSED
#define SCROLL_DOWN_BTN BUTTON5_PRESSED

/* ---------- Undo/Redo Data Structures ---------- */
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
    char **bulk_lines;  /* heap-allocated array of heap strings   */
    int    bulk_start;
    int    bulk_count;
    int    bulk_total;  /* total line_count at snapshot time      */
    int    saved_line;  /* cursor state before operation          */
    int    saved_col;
} UndoOp;

/* ---------- Dynamic Line Storage ---------- */
static char **lines = NULL;   /* heap array of heap strings          */
static int line_cap = 0;      /* allocated slots in lines[]          */
int line_count = 0;           /* active lines                        */

/* ---------- Global State ---------- */
int current_line = 0;
int cursor_x = 0;
int scroll_y = 0;
char current_filename[256] = "untitled.txt";
char status_msg[1024] = "";
int is_modified = 0;
int mod_count = 0;
int overwrite_mode = 0;
clock_t last_char_time = 0;
int in_paste_stream = 0;
static int is_pasting = 0;
static int read_only_mode = 0;
int create_backup = 0;

static time_t file_mtime = 0;
static volatile sig_atomic_t got_fatal_signal = 0;

int match_line = -1;
int match_col  = -1;

static char *clipboard_line = NULL;
static int syntax_highlight_enabled = 1;
static int gutter_visible = 1;

/* ---------- Mouse Selection State ---------- */
/* Byte-index range of a double- or triple-click selection on a single line.
 * sel_start_col is inclusive, sel_end_col is exclusive. */
static int sel_active    = 0;
static int sel_line      = -1;
static int sel_start_col = 0;
static int sel_end_col   = 0;

static UndoOp *undo_buf = NULL;
static int undo_head = 0;
static int undo_count = 0;

static UndoOp *redo_buf = NULL;
static int redo_top = -1;

/* Function Declarations */
void save_undo_state_single(int line_idx);
void save_undo_state_batch(int start_line, int count);
void undo(void);
void redo_op(void);
void draw_screen(void);
void show_about_window(void);
void show_command_window(const char *prefill);
void save_file(void);
void save_as_file(void);

/* ---------- Signal Handler ---------- */
static void fatal_signal_handler(int sig) {
    (void)sig;
    got_fatal_signal = 1;
}

/* ---------- Line Swapping Feature ---------- */
static void swap_line_up(void) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
        return;
    }
    if (current_line <= 0) return;

    save_undo_state_batch(current_line - 1, 2);

    char *tmp = lines[current_line];
    lines[current_line] = lines[current_line - 1];
    lines[current_line - 1] = tmp;

    current_line--;
    is_modified = 1;

    int len = (int)strlen(lines[current_line]);
    if (cursor_x > len) cursor_x = len;

    if (current_line < scroll_y) scroll_y = current_line;
}

static void swap_line_down(void) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
        return;
    }
    if (current_line >= line_count - 1) return;

    save_undo_state_batch(current_line, 2);

    char *tmp = lines[current_line];
    lines[current_line] = lines[current_line + 1];
    lines[current_line + 1] = tmp;

    current_line++;
    is_modified = 1;

    int len = (int)strlen(lines[current_line]);
    if (cursor_x > len) cursor_x = len;

    int max_displayable_lines = LINES - 4;
    if (current_line >= scroll_y + max_displayable_lines) {
        scroll_y = current_line - max_displayable_lines + 1;
    }
}

/* ---------- UTF-8 Helpers ---------- */
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
            cols++; p++; continue;
        }
        int w = wcwidth(wc);
        cols += (w >= 0) ? w : 1;
        p += bytes;
    }
    return cols;
}

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

/* ---------- Memory & Line Management ---------- */
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

static char *xstrdup(const char *s) {
    char *p = strdup(s);
    return p ? p : strdup("");
}

static void set_line(int i, const char *s) {
    free(lines[i]);
    lines[i] = xstrdup(s);
}

static int insert_line_at(int i, const char *s) {
    if (!ensure_capacity(line_count + 1)) return 0;
    memmove(&lines[i + 1], &lines[i], (line_count - i) * sizeof(char *));
    lines[i] = NULL;
    set_line(i, s);
    line_count++;
    return 1;
}

static void remove_line_at(int i) {
    free(lines[i]);
    memmove(&lines[i], &lines[i + 1], (line_count - i - 1) * sizeof(char *));
    lines[line_count - 1] = NULL;
    line_count--;
}

/* ---------- Interactive Input Helper ---------- */
static int prompt_input(const char *prompt, char *buf, size_t buf_size, int digits_only) {
    int idx = 0;
    buf[0] = '\0';
    int prompt_len = (int)strlen(prompt);

    noecho();
    mvprintw(LINES - 1, 0, "%s", prompt);
    clrtoeol();
    refresh();

    while (idx < (int)buf_size - 1) {
        int ch = getch();
        if (ch == 27) {
            /* Intercept bracketed paste mode sequences */
            nodelay(stdscr, TRUE);
            int next1 = getch();
            if (next1 == '[') {
                int next2 = getch();
                if (next2 == '2') {
                    int seq_char;
                    while ((seq_char = getch()) != ERR && seq_char != '~');
                    nodelay(stdscr, FALSE);
                    continue;
                }
                if (next2 != ERR) ungetch(next2);
            }
            if (next1 != ERR) ungetch(next1);
            nodelay(stdscr, FALSE);

            status_msg[0] = '\0';
            return 0; /* ESC pressed */
        } else if (ch == 10 || ch == 13) {
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--;
                buf[idx] = '\0';
                mvprintw(LINES - 1, prompt_len + idx, " ");
                move(LINES - 1, prompt_len + idx);
                refresh();
            }
        } else if (digits_only ? isdigit((unsigned char)ch) : (ch >= 32 && ch <= 126)) {
            buf[idx++] = (char)ch;
            buf[idx] = '\0';
            mvprintw(LINES - 1, prompt_len + idx - 1, "%c", ch);
            refresh();
        }
    }
    return (strlen(buf) > 0);
}

/* ---------- Undo / Redo System ---------- */
static void free_op(UndoOp *op) {
    if ((op->type == OP_BULK || op->type == OP_LINE_JOIN) && op->bulk_lines) {
        for (int i = 0; i < op->bulk_count; i++) free(op->bulk_lines[i]);
        free(op->bulk_lines);
        op->bulk_lines = NULL;
    }
}

static void push_undo(UndoOp *op) {
    if (undo_count == UNDO_CAP) {
        free_op(&undo_buf[undo_head]);
        undo_head = (undo_head + 1) % UNDO_CAP;
        undo_count--;
    }
    int slot = (undo_head + undo_count) % UNDO_CAP;
    undo_buf[slot] = *op;
    undo_count++;
}

static void record_char_ins(int line, int col, char ch) {
    UndoOp op = {0};
    op.type = OP_CHAR_INS;
    op.line = line; op.col = col; op.ch = ch;
    op.saved_line = current_line; op.saved_col = cursor_x;
    push_undo(&op);
}

static void record_char_del(int line, int col, char ch) {
    UndoOp op = {0};
    op.type = OP_CHAR_DEL;
    op.line = line; op.col = col; op.ch = ch;
    op.saved_line = current_line; op.saved_col = cursor_x;
    push_undo(&op);
}

static void record_line_split(int line, int col) {
    UndoOp op = {0};
    op.type = OP_LINE_SPLIT;
    op.line = line; op.col = col;
    op.saved_line = current_line; op.saved_col = cursor_x;
    push_undo(&op);
}

static void record_line_join(int line, int col, const char *tail) {
    UndoOp op = {0};
    op.type = OP_LINE_JOIN;
    op.line = line; op.col = col;
    op.bulk_lines = malloc(sizeof(char *));
    if (op.bulk_lines) { op.bulk_lines[0] = xstrdup(tail); op.bulk_count = 1; }
    op.saved_line = current_line; op.saved_col = cursor_x;
    push_undo(&op);
}

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

void save_undo_state_single(int line_idx) { record_bulk(line_idx, 1); }
void save_undo_state_batch(int start_line, int count) { record_bulk(start_line, count); }

static void push_redo_snapshot(int saved_ln, int saved_cx) {
    if (redo_top >= UNDO_CAP - 1) {
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
    if (read_only_mode) { snprintf(status_msg, sizeof(status_msg), "Cannot undo in Read-Only mode!"); return; }
    if (undo_count == 0) { snprintf(status_msg, sizeof(status_msg), "Nothing to undo!"); return; }

    int slot = (undo_head + undo_count - 1) % UNDO_CAP;
    UndoOp *op = &undo_buf[slot];

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
    if (read_only_mode) { snprintf(status_msg, sizeof(status_msg), "Cannot redo in Read-Only mode!"); return; }
    if (redo_top < 0) { snprintf(status_msg, sizeof(status_msg), "Nothing to redo!"); return; }
    UndoOp *op = &redo_buf[redo_top];

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

/* ---------- File I/O ---------- */
void load_file(const char *filename) {
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
        if (!read_only_mode) {
            mvprintw(LINES - 1, 0, "Swap file detected for '%s'. Recover? (y/n): ", filename);
            clrtoeol();
            refresh();
            int ch = getch();
            if (ch == 'y' || ch == 'Y') {
                target_file = swp_filename;
                snprintf(status_msg, sizeof(status_msg), "Recovered from swap file.");
            }
        }
    }

    for (int i = 0; i < line_count; i++) { free(lines[i]); lines[i] = NULL; }
    line_count = 0;

    tracker_clear();

    FILE *fp = fopen(target_file, "r");
    if (fp) {
        char *buf = NULL;
        size_t buf_sz = 0;
        ssize_t n;
        while ((n = getline(&buf, &buf_sz, fp)) != -1) {
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

    struct stat st;
    file_mtime = (stat(current_filename, &st) == 0) ? st.st_mtime : 0;

    syntax_set_file(current_filename);
    syntax_scan(lines, line_count);
}

void interactive_open(void) {
    char filename[256];
    if (prompt_input("Enter filename to open (ESC to cancel): ", filename, sizeof(filename), 0)) {
        FILE *fp = fopen(filename, "r");
        if (fp) {
            fclose(fp);
            load_file(filename);
        } else {
            mvprintw(LINES - 1, 0, "File not found! Press any key...");
            clrtoeol(); refresh(); getch();
        }
    }
}

void save_file(void) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot save.");
        return;
    }
    if (strcmp(current_filename, "untitled.txt") == 0) {
        save_as_file();
        return;
    }

    /* ---------- Backup Logic (-b / --backup) ---------- */
    if (create_backup) {
        struct stat st;
        /* Only create a backup if the original file already exists on disk */
        if (stat(current_filename, &st) == 0) {
            char bak_filename[300];
            snprintf(bak_filename, sizeof(bak_filename), "%s.bak", current_filename);

            struct stat bak_st;

            /* Check if .bak file already exists */
            if (stat(bak_filename, &bak_st) == 0) {
                move(LINES - 1, 0);
                clrtoeol();
                attron(COLOR_PAIR(PAIR_RED) | A_BOLD);
                printw("Backup file '%s.bak' already exists. Overwrite backup? (y/n): ", current_filename);
                attroff(COLOR_PAIR(PAIR_RED) | A_BOLD);
                refresh();

                int confirm = getch();
                if (confirm != 'y' && confirm != 'Y') {
                    snprintf(status_msg, sizeof(status_msg), "Save cancelled (backup overwrite declined).");
                    return;
                }
            }

            FILE *src = fopen(current_filename, "rb");
            if (src) {
                FILE *dst = fopen(bak_filename, "wb");
                if (dst) {
                    char buf[4096];
                    size_t bytes;
                    while ((bytes = fread(buf, 1, sizeof(buf), src)) > 0) {
                        fwrite(buf, 1, bytes, dst);
                    }
                    fclose(dst);
                }
                fclose(src);
            }
        }
    }

    /* ---------- Main File Saving ---------- */
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

        struct stat st;
        file_mtime = (stat(current_filename, &st) == 0) ? st.st_mtime : 0;

        if (create_backup) {
            snprintf(status_msg, sizeof(status_msg), "Saved to '%s' (backup created: '%s.bak')", current_filename, current_filename);
        } else {
            snprintf(status_msg, sizeof(status_msg), "Saved successfully to '%s'!", current_filename);
        }
    } else {
        mvprintw(LINES - 1, 0, "Error: Could not save file! Press any key...");
        clrtoeol(); refresh(); getch();
    }
}

void save_as_file(void) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot save.");
        return;
    }

    char new_filename[256];
    if (!prompt_input("Save As: ", new_filename, sizeof(new_filename), 0)) {
        snprintf(status_msg, sizeof(status_msg), "Save As cancelled.");
        return;
    }

    /* Clean old swap file if saving over previous file path */
    if (strcmp(current_filename, "untitled.txt") != 0) {
        char old_swp[300];
        snprintf(old_swp, sizeof(old_swp), ".%s.swp", current_filename);
        unlink(old_swp);
    }

    strncpy(current_filename, new_filename, sizeof(current_filename) - 1);
    current_filename[sizeof(current_filename) - 1] = '\0';

    syntax_set_file(current_filename);
    syntax_scan(lines, line_count);

    save_file();
}

void auto_save(void) {
    if (read_only_mode) return;
    char swp_filename[300];
    snprintf(swp_filename, sizeof(swp_filename), ".%s.swp", current_filename);
    FILE *fp = fopen(swp_filename, "w");
    if (fp) {
        for (int i = 0; i < line_count; i++)
            fprintf(fp, "%s\n", lines[i]);
        fclose(fp);
    }
}

/* ---------- Bracket Matching ---------- */
static int find_matching_bracket(int start_line, int start_col) {
    const char *open  = "([{";
    const char *close = ")]}";
    char ch = lines[start_line][start_col];
    int dir = 0;
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

/* ---------- Screen Rendering ---------- */
void draw_screen(void) {
    update_bracket_match();

    move(0, 0); clrtoeol();
    attron(COLOR_PAIR(PAIR_YELLOW));
    if (read_only_mode)
        printw(" File: %s [Read-Only] (%d lines)", current_filename, line_count);
    else if (is_modified)
        printw(" File: %s * (unsaved) (%d lines)", current_filename, line_count);
    else
        printw(" File: %s (%d lines)", current_filename, line_count);
    attroff(COLOR_PAIR(PAIR_YELLOW));

    move(1, 0); clrtoeol();
    for (int x = 0; x < COLS; x++) mvaddch(1, x, ACS_HLINE);

    int max_displayable_lines = LINES - 4;
    int physical_row = 2;
    int wrap_col = COLS - 1;
    int file_line_index = scroll_y;

    int gutter_digits = 1;
    { int tmp = line_count; while (tmp >= 10) { tmp /= 10; gutter_digits++; } }
    int gutter_width = gutter_visible ? (gutter_digits + 3) : 0;

    while (physical_row < 2 + max_displayable_lines && file_line_index < line_count) {
        move(physical_row, 0); clrtoeol();

        if (gutter_visible) {
            if (file_line_index == current_line) {

                  /* Priority: Read-Only (Red), Overwrite (Yellow), Insert (Green) */
                  if (read_only_mode) {
                    attron(COLOR_PAIR(PAIR_RED));
                    mvprintw(physical_row, 0, "%*d ", gutter_digits, file_line_index + 1);
                    attroff(COLOR_PAIR(PAIR_RED));
                    attron(COLOR_PAIR(PAIR_RED) | A_BOLD);
                    mvaddch(physical_row, gutter_digits + 1, ACS_DIAMOND);
                    attroff(COLOR_PAIR(PAIR_RED) | A_BOLD);
                } else if (overwrite_mode) {
                    attron(COLOR_PAIR(PAIR_YELLOW));
                    mvprintw(physical_row, 0, "%*d ", gutter_digits, file_line_index + 1);
                    attroff(COLOR_PAIR(PAIR_YELLOW));
                    attron(COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
                    mvaddch(physical_row, gutter_digits +1, ACS_DIAMOND);
                    attroff(COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
                } else {
                    attron(COLOR_PAIR(PAIR_GREEN));
                    mvprintw(physical_row, 0, "%*d ", gutter_digits, file_line_index + 1);
                    attroff(COLOR_PAIR(PAIR_GREEN));
                    attron(COLOR_PAIR(PAIR_GREEN) | A_BOLD);
                    mvaddch(physical_row, gutter_digits + 1, ACS_DIAMOND);
                    attroff(COLOR_PAIR(PAIR_GREEN) | A_BOLD);
                }

            } else {
                mvprintw(physical_row, 0, "%*d ", gutter_digits, file_line_index + 1);
                mvaddch(physical_row, gutter_digits + 1, ACS_VLINE);
            }
        }

        char *line = lines[file_line_index];
        int len = strlen(line);
        int disp_len = utf8_display_width(line);
        int current_phys_row = physical_row;
        int current_phys_col = gutter_width;

        move(current_phys_row, current_phys_col);

        Span spans[MAX_SPANS];
        int nspans = syntax_highlight_enabled ? syntax_spans(file_line_index, line, spans) : 0;
        static const int tok_pair[] = { 0, 2, 4, 5, 3 };

        if (disp_len > wrap_col - gutter_width) {
            attron(A_DIM);
            mvaddch(physical_row, wrap_col - 1, '>');
            attroff(A_DIM);
            move(physical_row, gutter_width);
        }

        int span_idx = 0;
        int j = 0;
        while (j < len) {
            if (current_phys_col >= wrap_col) {
                current_phys_row++; current_phys_col = gutter_width;
                if (current_phys_row < 2 + max_displayable_lines) {
                    move(current_phys_row, current_phys_col);
                    clrtoeol();
                } else break;
            }
            unsigned char ub = (unsigned char)line[j];
            int clen = 1;
            if      ((ub & 0xF8) == 0xF0) clen = 4;
            else if ((ub & 0xF0) == 0xE0) clen = 3;
            else if ((ub & 0xE0) == 0xC0) clen = 2;
            if (j + clen > len) clen = len - j;

            int cw = utf8_display_width_n(line + j, clen);
            if (cw < 1) cw = 1;

            int is_bracket_cursor = (file_line_index == current_line && j == cursor_x && match_line >= 0);
            int is_bracket_match  = (file_line_index == match_line && j == match_col);

            if (is_bracket_cursor || is_bracket_match) {
                attron(COLOR_PAIR(PAIR_MATCH) | A_BOLD);
                for (int b = 0; b < clen; b++) printw("%c", line[j + b]);
                attroff(COLOR_PAIR(PAIR_MATCH) | A_BOLD);
                current_phys_col += cw;
                j += clen;
                continue;
            }

            /* Selection highlight (double- or triple-click) */
            int in_sel = (sel_active && file_line_index == sel_line
                          && j >= sel_start_col && j < sel_end_col);
            if (in_sel) {
                attron(COLOR_PAIR(PAIR_SELECT));
                for (int b = 0; b < clen; b++) printw("%c", line[j + b]);
                attroff(COLOR_PAIR(PAIR_SELECT));
                current_phys_col += cw;
                j += clen;
                continue;
            }

            while (span_idx < nspans && spans[span_idx].end <= j) span_idx++;

            int pair = 0;
            if (span_idx < nspans && j >= spans[span_idx].start && j < spans[span_idx].end)
                pair = tok_pair[spans[span_idx].type];

            int is_current = (file_line_index == current_line);

            if (is_current) attron(A_BOLD);
            if (pair) attron(COLOR_PAIR(pair));

            for (int b = 0; b < clen; b++) printw("%c", line[j + b]);

            if (pair) attroff(COLOR_PAIR(pair));
            if (is_current) attroff(A_BOLD);

            current_phys_col += cw;
            j += clen;
        }
        physical_row = current_phys_row + 1;
        file_line_index++;
    }

    while (physical_row < 2 + max_displayable_lines) {
        move(physical_row, 0); clrtoeol();
        physical_row++;
    }

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

    move(LINES - 2, 0); clrtoeol();
    for (int x = 0; x < COLS; x++) mvaddch(LINES - 2, x, ACS_HLINE);
    move(LINES - 1, 0); clrtoeol();

    if (strlen(status_msg) > 0) {
        attron(COLOR_PAIR(PAIR_YELLOW));
        mvprintw(LINES - 1, 0, "%.*s", COLS - 1, status_msg);
        attroff(COLOR_PAIR(PAIR_YELLOW));
    } else {
        int gd_s = 1; { int tmp = line_count; while (tmp >= 10) { tmp /= 10; gd_s++; } }
        int tw_s = COLS - 1 - (gd_s + 3);
        int vis_col = (tw_s > 0) ? (cursor_x % tw_s) + 1 : cursor_x + 1;

        move(LINES -1, 0);
        clrtoeol();

        /* Matches the priority in the gutter rendering block above */
        if (read_only_mode) {
            attron(COLOR_PAIR(PAIR_RED) | A_BOLD);
            printw("[RO Mode]  ");
            attroff(COLOR_PAIR(PAIR_RED) | A_BOLD);
        } else if (overwrite_mode) {
            attron(COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
            printw("[OW Mode]  ");
            attroff(COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
        } else {
            attron(COLOR_PAIR(PAIR_GREEN) | A_BOLD);
            printw("[RW Mode]  ");
            attroff(COLOR_PAIR(PAIR_GREEN) | A_BOLD);
        }

        if (!is_modified) {
            printw("Ln: %d Col: %d | (Ctrl+? for Help)", current_line + 1, vis_col);
        } else {
            int total_chars = 0, modified_lines = 0;
            for (int i = 0; i < line_count; i++) {
                total_chars += (int)strlen(lines[i]);
                if (tracker_is_modified(i, 0)) modified_lines++;
            }
            printw("Lines Mod: %d | Total Chars: %d | Ln: %d Col: %d | (Ctrl+? for Help)",
                modified_lines, total_chars, current_line + 1, vis_col);
        }
    }

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

/* ---------- Text Search & Replace ---------- */
void find_text(void) {
    char search_str[128];
    if (!prompt_input("Find: ", search_str, sizeof(search_str), 0)) return;

    struct { int line; int col; } matches[500];
    int match_count = 0, current_match_idx = 0;

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
        snprintf(status_msg, sizeof(status_msg), "No matches found for '%s'.", search_str);
        return;
    }

    int start_idx = 0;
    for (int k = 0; k < match_count; k++) {
        if (matches[k].line > current_line ||
           (matches[k].line == current_line && matches[k].col >= cursor_x)) {
            start_idx = k;
            break;
        }
    }
    current_match_idx = start_idx;

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
                 "Match %d of %d [Next: Right/Down | Prev: Left/Up | Enter: Done]",
                 current_match_idx + 1, match_count);
        draw_screen();

        int ch = getch();
        if (ch == 10 || ch == 13) {
            snprintf(status_msg, sizeof(status_msg), "Found match at line %d.", current_line + 1);
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

void replace_text(void) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
        return;
    }
    char search_str[128], replace_str[128];
    if (!prompt_input("Find: ", search_str, sizeof(search_str), 0)) return;
    if (!prompt_input("Replace with: ", replace_str, sizeof(replace_str), 0)) return;

    int replacements = 0;
    int search_len = strlen(search_str);
    int replace_len = strlen(replace_str);
    int replace_all = 0;

    save_undo_state_batch(0, line_count);

    for (int i = 0; i < line_count; i++) {
        if (!lines[i]) continue;

        int line_changed = 0;
        char current_buf[MAX_LINE_LEN];
        strncpy(current_buf, lines[i], sizeof(current_buf) - 1);
        current_buf[MAX_LINE_LEN - 1] = '\0';

        char lower_line[MAX_LINE_LEN], lower_search[128];
        int ll = strlen(current_buf);
        if (ll >= MAX_LINE_LEN) ll = MAX_LINE_LEN - 1;

        for (int j = 0; j < ll; j++)
            lower_line[j] = tolower((unsigned char)current_buf[j]);
        lower_line[ll] = '\0';

        for (int j = 0; j < search_len && j < 127; j++)
            lower_search[j] = tolower((unsigned char)search_str[j]);
        lower_search[search_len < 127 ? search_len : 127] = '\0';

        if (strstr(lower_line, lower_search) == NULL) continue;

        char buffer[MAX_LINE_LEN];
        int b_idx = 0;
        int orig_idx = 0;

        while (orig_idx < ll) {
            if (orig_idx + search_len <= ll &&
                strncmp(&lower_line[orig_idx], lower_search, search_len) == 0) {

                int do_replace = 0;

                if (replace_all) {
                    do_replace = 1;
                } else {
                    current_line = i;
                    cursor_x = orig_idx;
                    int max_displayable_lines = LINES - 4;
                    scroll_y = current_line - (max_displayable_lines / 2);
                    if (scroll_y < 0) scroll_y = 0;
                    if (scroll_y > line_count - max_displayable_lines)
                        scroll_y = line_count - max_displayable_lines;
                    if (scroll_y < 0) scroll_y = 0;

                    draw_screen();
                    snprintf(status_msg, sizeof(status_msg),
                             "Replace match at Ln %d, Col %d? [y]es / [n]o / [a]ll / [q]uit",
                             current_line + 1, cursor_x + 1);
                    draw_screen();

                    int confirm = getch();
                    if (confirm == 'y' || confirm == 'Y') {
                        do_replace = 1;
                    } else if (confirm == 'a' || confirm == 'A') {
                        do_replace = 1;
                        replace_all = 1;
                    } else if (confirm == 'q' || confirm == 'Q' || confirm == 27) {
                        snprintf(status_msg, sizeof(status_msg),
                                 "Replacement cancelled. Replaced %d occurrence(s).", replacements);
                        return;
                    } else {
                        do_replace = 0;
                    }
                }

                if (do_replace) {
                    for (int r = 0; r < replace_len && b_idx < MAX_LINE_LEN - 1; r++) {
                        buffer[b_idx++] = replace_str[r];
                    }
                    orig_idx += search_len;
                    replacements++;
                    line_changed = 1;
                } else {
                    if (b_idx < MAX_LINE_LEN - 1) {
                        buffer[b_idx++] = current_buf[orig_idx];
                    }
                    orig_idx++;
                }
            } else {
                if (b_idx < MAX_LINE_LEN - 1) {
                    buffer[b_idx++] = current_buf[orig_idx];
                }
                orig_idx++;
            }
        }
        buffer[b_idx] = '\0';

        if (line_changed && strcmp(lines[i], buffer) != 0) {
            free(lines[i]);
            lines[i] = strdup(buffer);
            is_modified = 1;
        }
    }

    snprintf(status_msg, sizeof(status_msg), "Replaced %d occurrence(s).", replacements);
}

/* ---------- Line Jump and Processing ---------- */
void goto_line(void) {
    char line_input[32];
    if (!prompt_input("Go to line: ", line_input, sizeof(line_input), 1)) return;

    int target = atoi(line_input);
    if (target < 1 || target > line_count) {
        snprintf(status_msg, sizeof(status_msg), "Line %d out of bounds! (Total lines: %d)", target, line_count);
        return;
    }
    current_line = target - 1; cursor_x = 0;
    int max_displayable_lines = LINES - 4;
    scroll_y = current_line - (max_displayable_lines / 2);
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
    if (scroll_y < 0) scroll_y = 0;
}

static void process_line_ranges_interactive(int is_cut_mode) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
        return;
    }
    char input[256];
    const char *prompt = is_cut_mode
        ? "Cut lines (e.g., 3, 5, 10-25 or !20-25): "
        : "Delete lines (e.g., 3, 5, 10-25 or !20-25): ";

    if (!prompt_input(prompt, input, sizeof(input), 0)) return;

    char saved_input_copy[256];
    strncpy(saved_input_copy, input, sizeof(saved_input_copy) - 1);
    saved_input_copy[sizeof(saved_input_copy) - 1] = '\0';

    char *to_process = calloc(line_count, sizeof(char));
    if (!to_process) return;

    save_undo_state_batch(0, line_count);

    char *p = input;
    while (*p == ' ' || *p == '\t') p++;

    int invert = 0;
    if (*p == '!') {
        invert = 1;
        p++;
    }

    char *token = strtok(p, ",");
    while (token != NULL) {
        while (*token == ' ' || *token == '\t') token++;
        int start = 0, end = 0;
        if (sscanf(token, "%d-%d", &start, &end) == 2) {
            if (start > 0 && end >= start)
                for (int i = start; i <= end && i <= line_count; i++) to_process[i - 1] = 1;
        } else if (sscanf(token, "%d", &start) == 1) {
            if (start > 0 && start <= line_count) to_process[start - 1] = 1;
        }
        token = strtok(NULL, ",");
    }

    if (invert) {
        for (int i = 0; i < line_count; i++) {
            to_process[i] = !to_process[i];
        }
    }

    if (is_cut_mode) {
        free(clipboard_line);
        clipboard_line = NULL;

        size_t total_bytes = 0;
        for (int i = 0; i < line_count; i++) {
            if (to_process[i]) {
                total_bytes += strlen(lines[i]) + 1;
            }
        }

        if (total_bytes > 0) {
            clipboard_line = malloc(total_bytes + 1);
            if (clipboard_line) {
                clipboard_line[0] = '\0';
                char *dst = clipboard_line;
                for (int i = 0; i < line_count; i++) {
                    if (to_process[i]) {
                        size_t len = strlen(lines[i]);
                        memcpy(dst, lines[i], len);
                        dst += len;
                        *dst++ = '\n';
                    }
                }
            }
        } else {
            clipboard_line = xstrdup("");
        }
    }

    int processed_count = 0;
    for (int i = line_count - 1; i >= 0; i--) {
        if (to_process[i]) {
            remove_line_at(i);
            processed_count++;
            if (i <= current_line && current_line > 0) current_line--;
        }
    }
    free(to_process);

    if (line_count == 0) {
        ensure_capacity(1); lines[0] = xstrdup(""); line_count = 1;
        current_line = 0; cursor_x = 0;
    }
    int len = (int)strlen(lines[current_line]);
    if (cursor_x > len) cursor_x = len;
    int max_displayable_lines = LINES - 4;
    if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
    if (scroll_y < 0) scroll_y = 0;

    if (processed_count > 0) {
        is_modified = 1;
        snprintf(status_msg, sizeof(status_msg), "%s lines %s",
                 is_cut_mode ? "Cut" : "Deleted", saved_input_copy);
    } else {
        snprintf(status_msg, sizeof(status_msg), "No lines %s.",
                 is_cut_mode ? "cut" : "deleted");
    }
}

/* ---------- Selection-based Copy / Cut / Paste ---------- */

/* Copy the active mouse selection (a byte range on a single line) into
 * clipboard_line.  Returns 1 on success, 0 if no selection is active. */
static int copy_selection(void) {
    if (!sel_active || sel_start_col >= sel_end_col) return 0;
    int len = sel_end_col - sel_start_col;
    free(clipboard_line);
    clipboard_line = malloc(len + 2); /* text + '\n' + '\0' */
    if (!clipboard_line) return 0;
    memcpy(clipboard_line, lines[sel_line] + sel_start_col, len);
    clipboard_line[len]     = '\n';
    clipboard_line[len + 1] = '\0';
    return 1;
}

/* Cut the active mouse selection: copy it then delete the bytes from the
 * line.  Returns 1 on success, 0 if no selection is active. */
static int cut_selection(void) {
    if (!copy_selection()) return 0;
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot cut.");
        return 0;
    }
    int len = (int)strlen(lines[sel_line]);
    int cut_len = sel_end_col - sel_start_col;
    save_undo_state_single(sel_line);
    memmove(lines[sel_line] + sel_start_col,
            lines[sel_line] + sel_end_col,
            len - sel_end_col + 1);
    /* Move cursor to start of the cut region */
    current_line = sel_line;
    cursor_x     = sel_start_col;
    sel_active   = 0;
    is_modified  = 1;
    snprintf(status_msg, sizeof(status_msg), "Cut %d character(s).", cut_len);
    return 1;
}

/* Paste clipboard_line inline at the current cursor position when the
 * clipboard holds a single-line selection (no embedded newlines beyond the
 * trailing one).  Falls back to the normal line-based paste otherwise. */
static int paste_inline(void) {
    if (!clipboard_line || clipboard_line[0] == '\0') return 0;
    /* Count newlines — inline paste only if there is exactly one trailing \n */
    int nl_count = 0;
    for (const char *p = clipboard_line; *p; p++) if (*p == '\n') nl_count++;
    if (nl_count != 1) return 0; /* multi-line: fall through to normal paste */
    int ins_len = (int)strlen(clipboard_line) - 1; /* exclude trailing \n */
    if (ins_len <= 0) return 0;
    int cur_len = (int)strlen(lines[current_line]);
    if (cur_len + ins_len >= MAX_LINE_LEN) {
        snprintf(status_msg, sizeof(status_msg), "Paste would exceed line length limit.");
        return 1; /* handled — don't fall through */
    }
    save_undo_state_single(current_line);
    /* Make room */
    memmove(lines[current_line] + cursor_x + ins_len,
            lines[current_line] + cursor_x,
            cur_len - cursor_x + 1);
    memcpy(lines[current_line] + cursor_x, clipboard_line, ins_len);
    cursor_x    += ins_len;
    is_modified  = 1;
    snprintf(status_msg, sizeof(status_msg), "Pasted %d character(s).", ins_len);
    return 1;
}

void copy_lines_interactive(void) {
    char input[256];
    if (!prompt_input("Copy lines (e.g., 3, 5, 10-25 or !20-25): ", input, sizeof(input), 0)) return;

    char saved_input_copy[256];
    strncpy(saved_input_copy, input, sizeof(saved_input_copy) - 1);
    saved_input_copy[sizeof(saved_input_copy) - 1] = '\0';

    char *to_process = calloc(line_count, sizeof(char));
    if (!to_process) return;

    char *p = input;
    while (*p == ' ' || *p == '\t') p++;

    int invert = 0;
    if (*p == '!') {
        invert = 1;
        p++;
    }

    char *token = strtok(p, ",");
    while (token != NULL) {
        while (*token == ' ' || *token == '\t') token++;
        int start = 0, end = 0;
        if (sscanf(token, "%d-%d", &start, &end) == 2) {
            if (start > 0 && end >= start)
                for (int i = start; i <= end && i <= line_count; i++) to_process[i - 1] = 1;
        } else if (sscanf(token, "%d", &start) == 1) {
            if (start > 0 && start <= line_count) to_process[start - 1] = 1;
        }
        token = strtok(NULL, ",");
    }

    if (invert) {
        for (int i = 0; i < line_count; i++) {
            to_process[i] = !to_process[i];
        }
    }

    free(clipboard_line);
    clipboard_line = NULL;

    size_t total_bytes = 0;
    int copied_count = 0;
    for (int i = 0; i < line_count; i++) {
        if (to_process[i]) {
            total_bytes += strlen(lines[i]) + 1;
            copied_count++;
        }
    }

    if (total_bytes > 0) {
        clipboard_line = malloc(total_bytes + 1);
        if (clipboard_line) {
            clipboard_line[0] = '\0';
            char *dst = clipboard_line;
            for (int i = 0; i < line_count; i++) {
                if (to_process[i]) {
                    size_t len = strlen(lines[i]);
                    memcpy(dst, lines[i], len);
                    dst += len;
                    *dst++ = '\n';
                }
            }
            *dst = '\0';
        }
    } else {
        clipboard_line = xstrdup("");
    }
    free(to_process);

    if (copied_count > 0) {
        snprintf(status_msg, sizeof(status_msg), "Copied lines %s (%d line%s)",
                 saved_input_copy, copied_count, copied_count > 1 ? "s" : "");
    } else {
        snprintf(status_msg, sizeof(status_msg), "No lines copied.");
    }
}

void delete_lines_interactive(void) { process_line_ranges_interactive(0); }
void cut_lines_interactive(void) { process_line_ranges_interactive(1); }

/* ---------- Command Execution Popup ---------- */

/* Run a shell command and return its stdout+stderr in a heap-allocated,
 * null-terminated buffer.  *out_len receives the byte count (excluding the
 * terminator).  Caller must free() the returned pointer.  Returns NULL on
 * allocation failure.  Output is capped at CMD_OUT_MAX bytes; a truncation
 * notice is appended when the cap is hit. */
#define CMD_OUT_MAX (256 * 1024)
static char *run_command(const char *cmd, size_t *out_len) {
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "popen failed: %s", strerror(errno));
        if (out_len) *out_len = err ? strlen(err) : 0;
        return err;
    }

    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    if (!buf) { pclose(fp); return NULL; }

    int truncated = 0;
    char tmp[512];
    while (fgets(tmp, sizeof(tmp), fp)) {
        size_t n = strlen(tmp);
        if (len + n + 1 > CMD_OUT_MAX) {
            truncated = 1;
            break;
        }
        if (len + n + 1 > cap) {
            cap = (cap * 2 > len + n + 1) ? cap * 2 : len + n + 1 + 512;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); pclose(fp); return NULL; }
            buf = nb;
        }
        memcpy(buf + len, tmp, n);
        len += n;
    }
    pclose(fp);

    if (truncated) {
        const char *notice = "\n[output truncated at 256 KB]";
        size_t nl = strlen(notice);
        if (len + nl + 1 <= cap || (buf = realloc(buf, len + nl + 1)) != NULL) {
            memcpy(buf + len, notice, nl);
            len += nl;
        }
    }

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

/* A variant of prompt_input() that draws the prompt inside a WINDOW * on the
 * given row, rather than on the stdscr status bar.  Supports backspace, ESC,
 * Enter, and the full UTF-8 input path.  Returns 1 if the user confirmed, 0
 * if they pressed ESC. */
static int prompt_input_win(WINDOW *w, int row, int col, int max_w,
                            const char *prompt, char *buf, size_t buf_size) {
    int idx = 0;   /* byte length of buf */
    buf[0] = '\0';
    int plen = (int)strlen(prompt);
    /* Visible field width — stays strictly inside the border.
     * col is the left margin (e.g. 2), so the field must end before
     * win_w - 1 (the right border column).  max_w is inner_w = win_w - 4,
     * so col + plen + field_w <= col + max_w - 1 is always safe. */
    int field_w = max_w - plen - 1;
    if (field_w < 4) field_w = 4;
    int view_off = 0;  /* scroll offset: first visible char index in buf */

    noecho();

    /* Helper lambda (inline) — redraw the prompt + scrolling field. */
#define PIWIN_REDRAW() do { \
        int blen = (int)strlen(buf); \
        /* Advance view_off so cursor stays in field */ \
        if (idx - view_off >= field_w) view_off = idx - field_w + 1; \
        if (view_off > idx) view_off = idx; \
        if (view_off < 0)  view_off = 0; \
        /* Draw prompt (bold) */ \
        wattron(w, A_BOLD); \
        mvwprintw(w, row, col, "%s", prompt); \
        wattroff(w, A_BOLD); \
        /* Draw visible slice of buf, padded to field_w with spaces */ \
        char _vis[512]; \
        int _vlen = blen - view_off; \
        if (_vlen < 0) _vlen = 0; \
        if (_vlen > field_w) _vlen = field_w; \
        memcpy(_vis, buf + view_off, (size_t)_vlen); \
        memset(_vis + _vlen, ' ', (size_t)(field_w - _vlen)); \
        _vis[field_w] = '\0'; \
        mvwaddnstr(w, row, col + plen, _vis, field_w); \
        /* Cursor position within the field */ \
        wmove(w, row, col + plen + (idx - view_off)); \
        wrefresh(w); \
    } while (0)

    PIWIN_REDRAW();

    while (idx < (int)buf_size - 1) {
        int ch = wgetch(w);
        if (ch == 27) {
            /* Drain any escape sequence */
            nodelay(w, TRUE);
            int n1 = wgetch(w);
            if (n1 == '[') {
                int n2 = wgetch(w);
                if (n2 == '2') {
                    int sc;
                    while ((sc = wgetch(w)) != ERR && sc != '~');
                }
            }
            nodelay(w, FALSE);
            buf[0] = '\0';
            return 0;
        } else if (ch == 10 || ch == 13) {
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--;
                while (idx > 0 && (buf[idx] & 0xC0) == 0x80) idx--;
                buf[idx] = '\0';
                PIWIN_REDRAW();
            }
        } else if (ch >= 0xC0 && ch <= 0xFF) {
            /* Multi-byte UTF-8 lead byte */
            unsigned char ub = (unsigned char)ch;
            int seq_len = (ub & 0xF8) == 0xF0 ? 4 :
                          (ub & 0xF0) == 0xE0 ? 3 : 2;
            if (idx + seq_len < (int)buf_size - 1) {
                buf[idx++] = (char)ch;
                nodelay(w, TRUE);
                for (int i = 1; i < seq_len; i++) {
                    int c = wgetch(w);
                    if (c == ERR || (c & 0xC0) != 0x80) { idx--; break; }
                    buf[idx++] = (char)c;
                }
                nodelay(w, FALSE);
                buf[idx] = '\0';
                PIWIN_REDRAW();
            }
        } else if (ch >= 32 && ch <= 126) {
            buf[idx++] = (char)ch;
            buf[idx] = '\0';
            PIWIN_REDRAW();
        }
    }
#undef PIWIN_REDRAW
    return (strlen(buf) > 0);
}

/* Split a null-terminated string into an array of line pointers.
 * The source string is modified in-place (newlines replaced with '\0').
 * Returns the number of lines.  The returned array must be free()d but
 * the individual pointers point into src and must NOT be freed. */
static char **split_lines(char *src, int *count) {
    int cap = 64, n = 0;
    char **lines = malloc(cap * sizeof(char *));
    if (!lines) { *count = 0; return NULL; }
    lines[n++] = src;
    for (char *p = src; *p; p++) {
        if (*p == '\n') {
            *p = '\0';
            if (n >= cap) {
                cap *= 2;
                char **nl = realloc(lines, cap * sizeof(char *));
                if (!nl) break;
                lines = nl;
            }
            lines[n++] = p + 1;
        }
    }
    /* Drop a trailing empty line that results from a trailing newline */
    if (n > 0 && lines[n - 1][0] == '\0') n--;
    *count = n;
    return lines;
}

void show_command_window(const char *prefill) {
    int git_mode = (strncmp(prefill, "git ", 4) == 0 || strcmp(prefill, "git") == 0
                    || prefill[0] == '\0' /* free-form — no git shortcuts */ ? 0 : 0);
    /* Determine git mode: prefill starts with "git " */
    git_mode = (strncmp(prefill, "git ", 4) == 0);

    /* Window dimensions: 80% wide, 70% tall, min 40x12 */
    int win_w = (COLS  * 4) / 5;  if (win_w < 40) win_w = 40;
    int win_h = (LINES * 7) / 10; if (win_h < 12) win_h = 12;
    if (win_w > COLS  - 2) win_w = COLS  - 2;
    if (win_h > LINES - 2) win_h = LINES - 2;
    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS  - win_w) / 2;

    WINDOW *cw = newwin(win_h, win_w, start_y, start_x);
    keypad(cw, TRUE);

    /* Output area: rows 1..(win_h-4), prompt on row (win_h-2) */
    int body_h   = win_h - 4;  /* rows available for output */
    int prompt_y = win_h - 2;
    int inner_w  = win_w - 4;  /* usable width inside the border */

    char cmd_buf[512] = "";
    /* Pre-fill the command buffer */
    if (prefill && *prefill)
        snprintf(cmd_buf, sizeof(cmd_buf), "%s", prefill);

    char  *out_buf   = NULL;   /* heap buffer from run_command() */
    char **out_lines = NULL;   /* array of line pointers into out_buf */
    int    out_count = 0;      /* number of output lines */
    int    scroll    = 0;      /* first visible output line */
    int    ran       = 0;      /* 1 after a command has been run */

    for (;;) {
        werase(cw);
        box(cw, 0, 0);

        /* Title */
        {
            const char *title = git_mode ? " GIT " : " COMMAND ";
            int tlen = (int)strlen(title);
            int tx   = (win_w - tlen) / 2;
            mvwaddch(cw, 0, tx - 1,    ACS_RTEE);
            mvwaddch(cw, 0, tx + tlen, ACS_LTEE);
            wattron(cw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
            mvwprintw(cw, 0, tx, "%s", title);
            wattroff(cw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
        }

        /* Output body */
        if (ran) {
            for (int i = 0; i < body_h; i++) {
                int li = scroll + i;
                if (li >= out_count) break;
                /* Truncate to inner_w display columns */
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%.*s", inner_w, out_lines[li]);
                mvwprintw(cw, 1 + i, 2, "%s", tmp);
            }
            /* Scroll indicators */
            if (scroll > 0)
                mvwprintw(cw, 1, win_w - 4, " ^ ");
            if (scroll + body_h < out_count)
                mvwprintw(cw, body_h, win_w - 4, " v ");
        } else if (!ran && git_mode) {
            /* Show git shortcut hints in the body before first run */
            int hint_y = body_h / 2 - 1;
            if (hint_y < 1) hint_y = 1;
            wattron(cw, A_DIM);
            mvwprintw(cw, hint_y,     2, "Quick shortcuts (press when prompt is empty):");
            mvwprintw(cw, hint_y + 1, 4, "s  status     l  log --oneline -20");
            mvwprintw(cw, hint_y + 2, 4, "d  diff       a  add <path>");
            mvwprintw(cw, hint_y + 3, 4, "c  commit     p  push");
            wattroff(cw, A_DIM);
        }

        /* Separator line above footer */
        for (int x = 1; x < win_w - 1; x++)
            mvwaddch(cw, win_h - 3, x, ACS_HLINE);
        mvwaddch(cw, win_h - 3, 0,         ACS_LTEE);
        mvwaddch(cw, win_h - 3, win_w - 1, ACS_RTEE);

        /* Footer: git shortcuts or generic hint */
        wattron(cw, A_DIM);
        if (git_mode)
            mvwprintw(cw, win_h - 3 + 1, 2,
                      "s status  l log  d diff  a add  c commit  p push");
        else
            mvwprintw(cw, win_h - 3 + 1, 2, "Enter to run  ESC to close");
        wattroff(cw, A_DIM);

        wrefresh(cw);

        /* Prompt — collect command */
        char new_cmd[512] = "";
        if (prefill && *prefill && !ran)
            snprintf(new_cmd, sizeof(new_cmd), "%s", prefill);

        int confirmed = prompt_input_win(cw, prompt_y, 2, inner_w,
                                         "> ", new_cmd, sizeof(new_cmd));
        if (!confirmed && new_cmd[0] == '\0') {
            /* ESC with empty buffer — close */
            break;
        }

        /* Git mode single-key shortcuts (only when buffer is a single char) */
        if (git_mode && strlen(new_cmd) == 1) {
            char sc = new_cmd[0];
            if      (sc == 's') snprintf(new_cmd, sizeof(new_cmd), "git status");
            else if (sc == 'l') snprintf(new_cmd, sizeof(new_cmd), "git log --oneline -20");
            else if (sc == 'd') snprintf(new_cmd, sizeof(new_cmd), "git diff");
            else if (sc == 'a') {
                /* Prompt for path(s) to stage, e.g. "." or "qi.c" */
                char path[256] = "";
                werase(cw); box(cw, 0, 0);
                wattron(cw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
                mvwprintw(cw, 0, (win_w - 5) / 2, " GIT ");
                wattroff(cw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
                wattron(cw, A_DIM);
                mvwprintw(cw, 2, 2, "Path to stage (e.g. .  qi.c  subdir/file.c):");
                wattroff(cw, A_DIM);
                wrefresh(cw);
                if (prompt_input_win(cw, 4, 2, inner_w, "add: ", path, sizeof(path)) && path[0]) {
                    snprintf(new_cmd, sizeof(new_cmd), "git add %s", path);
                } else {
                    prefill = "git ";
                    continue;
                }
            }
            else if (sc == 'p') snprintf(new_cmd, sizeof(new_cmd), "git push");
            else if (sc == 'c') {
                /* Inline commit: prompt for message then run git commit -m */
                char msg[256] = "";
                werase(cw); box(cw, 0, 0);
                wattron(cw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
                mvwprintw(cw, 0, (win_w - 5) / 2, " GIT ");
                wattroff(cw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
                wattron(cw, A_DIM);
                mvwprintw(cw, 2, 2, "Enter commit message (ESC to cancel):");
                wattroff(cw, A_DIM);
                wrefresh(cw);
                if (prompt_input_win(cw, 4, 2, inner_w, "msg: ", msg, sizeof(msg)) && msg[0]) {
                    snprintf(new_cmd, sizeof(new_cmd), "git commit -m \"%s\"", msg);
                } else {
                    /* Cancelled */
                    prefill = "git ";
                    continue;
                }
            }
        }

        if (new_cmd[0] == '\0') continue;

        /* Build shell command with 2>&1 */
        char shell_cmd[600];
        snprintf(shell_cmd, sizeof(shell_cmd), "%s 2>&1", new_cmd);

        /* Run and capture */
        free(out_buf);
        free(out_lines);
        out_buf   = NULL;
        out_lines = NULL;
        out_count = 0;
        scroll    = 0;
        ran       = 1;

        size_t out_len = 0;
        out_buf = run_command(shell_cmd, &out_len);
        if (out_buf) {
            out_lines = split_lines(out_buf, &out_count);
        }

        /* After running, stay in the loop to show output and allow
         * another command or ESC to close. */
        prefill = "";  /* clear prefill so next prompt starts empty */

        /* Show output; let user scroll or press any non-arrow key to
         * run another command or close. */
        for (;;) {
            werase(cw);
            box(cw, 0, 0);

            /* Title */
            {
                const char *title = git_mode ? " GIT " : " COMMAND ";
                int tlen = (int)strlen(title);
                int tx   = (win_w - tlen) / 2;
                mvwaddch(cw, 0, tx - 1,    ACS_RTEE);
                mvwaddch(cw, 0, tx + tlen, ACS_LTEE);
                wattron(cw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
                mvwprintw(cw, 0, tx, "%s", title);
                wattroff(cw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
            }

            /* Command echo */
            wattron(cw, A_DIM);
            mvwprintw(cw, 1, 2, "$ %.*s", inner_w - 2, new_cmd);
            wattroff(cw, A_DIM);

            /* Output lines */
            for (int i = 0; i < body_h - 1; i++) {
                int li = scroll + i;
                if (li >= out_count) break;
                char tmp[512];
                snprintf(tmp, sizeof(tmp), "%.*s", inner_w, out_lines[li]);
                mvwprintw(cw, 2 + i, 2, "%s", tmp);
            }
            if (scroll > 0)
                mvwprintw(cw, 2, win_w - 4, " ^ ");
            if (scroll + body_h - 1 < out_count)
                mvwprintw(cw, body_h, win_w - 4, " v ");

            /* Separator + footer */
            for (int x = 1; x < win_w - 1; x++)
                mvwaddch(cw, win_h - 3, x, ACS_HLINE);
            mvwaddch(cw, win_h - 3, 0,         ACS_LTEE);
            mvwaddch(cw, win_h - 3, win_w - 1, ACS_RTEE);
            wattron(cw, A_DIM);
            mvwprintw(cw, win_h - 2, 2,
                      "Arrow keys to scroll  |  Enter for new command  |  ESC to close");
            wattroff(cw, A_DIM);

            wrefresh(cw);

            int k = wgetch(cw);
            if (k == KEY_UP   || k == 'k') { if (scroll > 0) scroll--; }
            else if (k == KEY_DOWN || k == 'j') {
                if (scroll + body_h - 1 < out_count) scroll++;
            }
            else if (k == KEY_PPAGE) {
                scroll -= body_h - 1;
                if (scroll < 0) scroll = 0;
            }
            else if (k == KEY_NPAGE) {
                scroll += body_h - 1;
                if (scroll + body_h - 1 > out_count) scroll = out_count - body_h + 1;
                if (scroll < 0) scroll = 0;
            }
            else if (k == 27 || k == 'q') {
                /* ESC or q: close the popup entirely */
                goto cmd_done;
            }
            else if (k == 10 || k == 13) {
                /* Enter: break inner loop to show prompt again */
                break;
            }
            /* Any other key: treat as Enter (run another command) */
            else { break; }
        }
    }

cmd_done:
    free(out_buf);
    free(out_lines);
    delwin(cw);
    touchwin(stdscr);
    refresh();
}

void show_about_window(void) {
    int height = 15;
    int width = 60;
    int start_y = (LINES - height) / 2;
    int start_x = (COLS - width) / 2;

    WINDOW *about_win = newwin(height, width, start_y, start_x);
    box(about_win, 0, 0);

    wattron(about_win, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
    mvwprintw(about_win, 0, (width - 10) / 2, " About qi ");
    wattroff(about_win, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);

    mvwprintw(about_win, 2, 4, "qi - A Lightweight Text Editor");
    mvwprintw(about_win, 3, 4, "Version %s", VERSION);

    mvwprintw(about_win, 5, 4, "A lightweight, terminal-based text editor built for");
    mvwprintw(about_win, 6, 4, "speed, low footprint, and simple keyboard workflows.");

    mvwprintw(about_win, 8, 4, "License: GNU General Public License v3.0 (GPLv3)");
    mvwprintw(about_win, 9, 4, "Repository: https://github.com/quatscho/qi");

    wattron(about_win, A_DIM);
    mvwprintw(about_win, 11, 4, "(c) 2026, Christopher Camacho");
    wattroff(about_win, A_DIM);

    wattron(about_win, COLOR_PAIR(PAIR_YELLOW));
    mvwprintw(about_win, 13, (width - 29) / 2, "Press any key to close...");
    wattroff(about_win, COLOR_PAIR(PAIR_YELLOW));

    wrefresh(about_win);

    wgetch(about_win);

    delwin(about_win);
    touchwin(stdscr);
    refresh();
}

/* ---------- Help Dialog ---------- */
void show_help_window(void) {
    struct HelpEntry { const char *key; const char *desc; int is_header; };
    static const struct HelpEntry entries[] = {
        { "FILE",          NULL,                         1 },
        { "Ctrl+S",        "Save file",                  0 },
        { "Alt/Opt+S",     "Save As",                    0 },
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
        { "Ctrl+C",        "Copy line(s)",               0 },
        { "Ctrl+Shift+K",  "Cut line(s)",                0 },
        { "Ctrl+P",        "Paste line",                 0 },
        { "Ctrl+W",        "Delete word left",           0 },
        { "Ctrl+N",        "Duplicate line",             0 },
        { "Alt/Opt+K",     "Swap line up",               0 },
        { "Alt/Opt+J",     "Swap line down",             0 },
        { "Tab",           "Indent",                     0 },
        { "Shift+Tab",     "Dedent",                     0 },
        { "",              "",                            0 },
        { "NAVIGATION",    NULL,                         1 },
        { "Ctrl+T",        "Top of file",                0 },
        { "Ctrl+B",        "Bottom of file",             0 },
        { "Ctrl+A",        "Line start (smart)",         0 },
        { "Ctrl+E",        "Line end",                   0 },
        { "Alt/Opt+B",     "Jump to matching bracket",   0 },
        { "",              "",                            0 },
        { "VIEW",          NULL,                         1 },
        { "F3",            "Toggle Read-Only Mode",      0 },
        { "F4",            "Toggle gutter / col-81",     0 },
        { "F5",            "Toggle syntax highlight",    0 },
        { "Ctrl+X",        "Toggle Overwrite Mode",      0 },
        { "Ctrl+?",        "This help screen",           0 },
        { "Alt/Opt+A",     "Show About dialog",          0 },
        { "",              "",                            0 },
        { "COMMAND",       NULL,                         1 },
        { "Ctrl+Shift+\\", "Command popup",              0 },
        { "Alt/Opt+G",     "Git popup",                  0 },
    };
    int total = (int)(sizeof(entries) / sizeof(entries[0]));

    int win_h = 22, win_w = 46;
    if (win_h > LINES - 2) win_h = LINES - 2;
    int inner_h = win_h - 6;
    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS  - win_w) / 2;
    WINDOW *hw = newwin(win_h, win_w, start_y, start_x);
    keypad(hw, TRUE);

    int scroll = 0;
    for (;;) {
        werase(hw);
        box(hw, 0, 0);

        {
            char title[32];
            snprintf(title, sizeof(title), " HELP ");
            int tlen = (int)strlen(title);
            int tx = (win_w - tlen) / 2;
            mvwaddch(hw, 0, tx - 1,    ACS_RTEE);
            mvwaddch(hw, 0, tx + tlen, ACS_LTEE);
            wattron(hw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
            mvwprintw(hw, 0, tx, "%s", title);
            wattroff(hw, COLOR_PAIR(PAIR_YELLOW) | A_BOLD);
        }

        int row = 1;
        int rendered = 0;
        for (int i = 0; i < total && row < 1 + inner_h; i++) {
            if (rendered < scroll) { rendered++; continue; }
            if (entries[i].is_header) {
                wattron(hw, A_BOLD | A_UNDERLINE);
                mvwprintw(hw, row, 2, "%s", entries[i].key);
                wattroff(hw, A_BOLD | A_UNDERLINE);
            } else if (entries[i].key[0] == '\0') {
            } else {
                mvwprintw(hw, row, 2,  "%-14s", entries[i].key);
                mvwprintw(hw, row, 16, "%s", entries[i].desc);
            }
            row++;
            rendered++;
        }

        if (scroll > 0) mvwprintw(hw, 1, win_w - 4, " ^ ");
        if (scroll + inner_h < total) mvwprintw(hw, inner_h, win_w - 4, " v ");

        int footer_start = win_h - 4;
        for (int x = 1; x < win_w - 1; x++) mvwaddch(hw, footer_start, x, ACS_HLINE);
        wattron(hw, A_DIM);
        mvwprintw(hw, footer_start + 1, 2, "Arrow keys to scroll");
        wattroff(hw, A_DIM);
        wattron(hw, COLOR_PAIR(PAIR_YELLOW));
        mvwprintw(hw, footer_start + 2, 2, "Press any key to close...");
        wattroff(hw, COLOR_PAIR(PAIR_YELLOW));

        /*{
            char copy[48];
            snprintf(copy, sizeof(copy), " (c) 2026 Christopher Camacho ");
            int clen = (int)strlen(copy);
            int cx = (win_w - clen) / 2;
            mvwaddch(hw, win_h - 1, cx - 1,    ACS_RTEE);
            mvwaddch(hw, win_h - 1, cx + clen, ACS_LTEE);
            wattron(hw, A_DIM);
            mvwprintw(hw, win_h - 1, cx, "%s", copy);
            wattroff(hw, A_DIM);
        }*/

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

/* ---------- Editor Actions & Helpers ---------- */
static void handle_mouse_event(void) {
    MEVENT me;
    if (getmouse(&me) == OK) {
        int scroll_speed = 3;
        if (me.bstate & SCROLL_UP_BTN) {
            /* Scroll up */
            scroll_y -= scroll_speed;
            if (scroll_y < 0) scroll_y = 0;
            if (current_line < scroll_y) current_line = scroll_y;
        } else if (me.bstate & SCROLL_DOWN_BTN) {
            /* Scroll down */
            int mdl = LINES - 4;
            scroll_y += scroll_speed;
            if (scroll_y > line_count - 1) scroll_y = line_count - 1;
            if (scroll_y < 0) scroll_y = 0;
            if (current_line < scroll_y) current_line = scroll_y;
            else if (current_line >= scroll_y + mdl) current_line = scroll_y + mdl - 1;
        } else if (me.bstate & (BUTTON1_PRESSED | BUTTON1_DOUBLE_CLICKED | BUTTON1_TRIPLE_CLICKED)) {
            int click_row = (int)me.y;
            int click_col = (int)me.x;
            if (click_row >= 2 && click_row < LINES - 2) {
                int gd_m = 1; { int tmp = line_count; while (tmp >= 10) { tmp /= 10; gd_m++; } }
                int gw_m = gutter_visible ? (gd_m + 3) : 0;
                int text_width = COLS - 1 - gw_m;
                int phys = 2;
                int found = 0;
                for (int i = scroll_y; i < line_count && phys < LINES - 2; i++) {
                    int ll = utf8_display_width(lines[i]);
                    int vrows = (ll == 0) ? 1 : (ll / text_width) + 1;
                    if (click_row < phys + vrows) {
                        int row_within = click_row - phys;
                        int col_within = click_col - gw_m;
                        if (col_within < 0) col_within = 0;
                        int new_cx = row_within * text_width + col_within;
                        if (new_cx > ll) new_cx = ll;
                        current_line = i;
                        cursor_x = new_cx;
                        found = 1;

                        if (me.bstate & BUTTON1_TRIPLE_CLICKED) {
                            /* Triple-click: select entire line */
                            sel_active    = 1;
                            sel_line      = i;
                            sel_start_col = 0;
                            sel_end_col   = (int)strlen(lines[i]);
                            cursor_x      = sel_end_col;
                        } else if (me.bstate & BUTTON1_DOUBLE_CLICKED) {
                            /* Double-click: select word or single non-space char
                             * under the cursor.  cursor_x is a byte offset. */
                            char *ln = lines[i];
                            int blen = (int)strlen(ln);
                            int cx = new_cx;
                            if (cx > blen) cx = blen;
                            /* Clamp to a valid byte boundary */
                            if (cx == blen && blen > 0) cx = blen - 1;
                            int start = cx, end = cx;
                            if (blen > 0 && cx < blen) {
                                unsigned char c = (unsigned char)ln[cx];
                                int is_word = (c == '_' || (c < 0x80 ? isalnum(c) : 1));
                                if (is_word) {
                                    /* Expand left */
                                    while (start > 0) {
                                        unsigned char pc = (unsigned char)ln[start - 1];
                                        if (!(pc == '_' || (pc < 0x80 ? isalnum(pc) : 1))) break;
                                        start--;
                                    }
                                    /* Expand right */
                                    while (end < blen) {
                                        unsigned char nc = (unsigned char)ln[end];
                                        if (!(nc == '_' || (nc < 0x80 ? isalnum(nc) : 1))) break;
                                        end++;
                                    }
                                } else {
                                    /* Non-word character: select just that char */
                                    end = cx + 1;
                                }
                            }
                            sel_active    = 1;
                            sel_line      = i;
                            sel_start_col = start;
                            sel_end_col   = end;
                            cursor_x      = end;
                        } else {
                            /* Single click: clear any active selection */
                            sel_active = 0;
                        }
                        break;
                    }
                    phys += vrows;
                }
                if (!found && line_count > 0) {
                    current_line = line_count - 1;
                    cursor_x = (int)strlen(lines[current_line]);
                    sel_active = 0;
                }
            }
        }
    }
}

static void handle_backspace(void) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
    } else if (cursor_x > 0) {
        record_char_del(current_line, cursor_x - 1, lines[current_line][cursor_x - 1]);
        int len = strlen(lines[current_line]);
        memmove(lines[current_line] + cursor_x - 1,
                lines[current_line] + cursor_x,
                len - cursor_x + 1);
        cursor_x--; is_modified = 1;
    } else if (current_line > 0) {
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

static void handle_delete_key(void) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
    } else {
        int len = strlen(lines[current_line]);
        if (cursor_x < len) {
            record_char_del(current_line, cursor_x, lines[current_line][cursor_x]);
            memmove(lines[current_line] + cursor_x,
                    lines[current_line] + cursor_x + 1,
                    len - cursor_x);
            is_modified = 1;
        } else if (current_line < line_count - 1) {
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
}

static void handle_enter_key(int paste_batch_active) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
        return;
    }
    if (!paste_batch_active) record_line_split(current_line, cursor_x);

    int indent_len = 0;
    if (!is_pasting) {
        while (lines[current_line][indent_len] == ' ' ||
               lines[current_line][indent_len] == '\t')
            indent_len++;
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

    int max_displayable_lines = LINES - 4;
    if (current_line >= scroll_y + max_displayable_lines) {
        scroll_y = current_line - max_displayable_lines + 1;
        if (scroll_y < 0) scroll_y = 0;
    }
}

/* Insert a multi-byte UTF-8 sequence (2–4 bytes) at the cursor.
 * lead_byte is the first byte already read from getch().
 * Uses a bulk-line undo snapshot (same as paste) rather than per-byte undo. */
static void handle_utf8_char(int lead_byte, int paste_batch_active) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
        return;
    }

    /* Determine expected sequence length from the lead byte */
    unsigned char ub = (unsigned char)lead_byte;
    int seq_len;
    if      ((ub & 0xF8) == 0xF0) seq_len = 4;
    else if ((ub & 0xF0) == 0xE0) seq_len = 3;
    else if ((ub & 0xE0) == 0xC0) seq_len = 2;
    else return; /* not a valid UTF-8 lead byte */

    char seq[5] = {0};
    seq[0] = (char)lead_byte;

    /* Read the continuation bytes with a short timeout */
    nodelay(stdscr, TRUE);
    for (int i = 1; i < seq_len; i++) {
        int c = getch();
        if (c == ERR || (c & 0xC0) != 0x80) {
            /* Incomplete or invalid sequence — discard */
            nodelay(stdscr, FALSE);
            return;
        }
        seq[i] = (char)c;
    }
    nodelay(stdscr, FALSE);

    int len = (int)strlen(lines[current_line]);
    if (len + seq_len >= MAX_LINE_LEN) {
        snprintf(status_msg, sizeof(status_msg), "Line too long.");
        return;
    }

    if (!paste_batch_active) {
        save_undo_state_single(current_line);
    }

    if (overwrite_mode && cursor_x < len) {
        /* In overwrite mode, replace the UTF-8 character at cursor_x.
         * Determine the byte length of the character being overwritten. */
        unsigned char ob = (unsigned char)lines[current_line][cursor_x];
        int old_len = 1;
        if      ((ob & 0xF8) == 0xF0) old_len = 4;
        else if ((ob & 0xF0) == 0xE0) old_len = 3;
        else if ((ob & 0xE0) == 0xC0) old_len = 2;
        int delta = seq_len - old_len;
        if (len + delta >= MAX_LINE_LEN) {
            snprintf(status_msg, sizeof(status_msg), "Line too long.");
            return;
        }
        char *nl = malloc(len + delta + 1);
        if (!nl) return;
        memcpy(nl, lines[current_line], cursor_x);
        memcpy(nl + cursor_x, seq, seq_len);
        memcpy(nl + cursor_x + seq_len,
               lines[current_line] + cursor_x + old_len,
               len - cursor_x - old_len + 1);
        free(lines[current_line]); lines[current_line] = nl;
    } else {
        char *nl = malloc(len + seq_len + 1);
        if (!nl) return;
        memcpy(nl, lines[current_line], cursor_x);
        memcpy(nl + cursor_x, seq, seq_len);
        memcpy(nl + cursor_x + seq_len,
               lines[current_line] + cursor_x,
               len - cursor_x + 1);
        free(lines[current_line]); lines[current_line] = nl;
    }

    tracker_set_modified(current_line, cursor_x, 1);
    cursor_x += seq_len;
    is_modified = 1;
    mod_count++;
    if (mod_count >= 50) { auto_save(); mod_count = 0; }
}

static void handle_printable_char(int ch, int paste_batch_active) {
    if (read_only_mode) {
        snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
        return;
    }
    int len = strlen(lines[current_line]);
    if (!paste_batch_active) {
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

/* ---------- Main Loop ---------- */
int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");

    char *prog_name = strrchr(argv[0], '/');
    prog_name = prog_name ? prog_name + 1 : argv[0];
    if (strcmp(prog_name, "view") == 0 || strcmp(prog_name, "roqi") == 0) {
        read_only_mode = 1;
    }

    char *target_filename = NULL;
    char *target_line_arg = NULL;

    /* ---------- Command Line Flags ---------- */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: qi [options] [filename] [+line|:line]\n\n");
            printf("Options:\n");
            printf("  -h, --help             Display this help message\n");
            printf("  -v, --version          Display version information\n");
            printf("  -r, --read-only        Open file in read-only mode\n");
            printf("  -L, --no-line-numbers  Disable line numbers on startup\n");
            printf("  -b, --backup           Create a backup copy before saving\n");
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("qi text editor v%s\n", VERSION);
            return 0;
        } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--read-only") == 0) {
            read_only_mode = 1;
        } else if (strcmp(argv[i], "-L") == 0 || strcmp(argv[i], "--no-line-numbers") == 0) {
            gutter_visible = 0;
        } else if (strcmp(argv[i], "-b") == 0 || strcmp(argv[i], "--backup") == 0) {
            create_backup = 1;
        } else if (argv[i][0] == '+' || argv[i][0] == ':') {
            target_line_arg = argv[i];
        } else if (!target_filename) {
            target_filename = argv[i];
        }
    }

    struct termios tty;
    if (tcgetattr(STDIN_FILENO, &tty) == 0) {
        tty.c_iflag &= ~IXON;
        tcsetattr(STDIN_FILENO, TCSANOW, &tty);
    }
    printf("\e[?2004l"); fflush(stdout);

    initscr();
    colors_init();
    set_escdelay(25);
    raw();
    noecho();
    keypad(stdscr, TRUE);

    /* Bind terminal escape sequences for ALT/OPT combos */
    define_key("\033s", KEY_ALT_S);
    define_key("\033g", KEY_ALT_G);

    printf("\033[?2004h");
    fflush(stdout);

    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    curs_set(1);
    /* ALL_MOUSE_EVENTS ensures button 5 (scroll down) is registered even
     * on macOS system ncurses where BUTTON5_PRESSED may not be natively
     * defined and the terminal sends scroll-down as a separate button. */
    mousemask(ALL_MOUSE_EVENTS, NULL);

    signal(SIGTERM, fatal_signal_handler);
    signal(SIGHUP,  fatal_signal_handler);

    tracker_init(1024, 1);

    undo_buf = calloc(UNDO_CAP, sizeof(UndoOp));
    redo_buf = calloc(UNDO_CAP, sizeof(UndoOp));

    ensure_capacity(1);
    lines[0] = xstrdup("");
    line_count = 1;

    if (target_filename) {
        load_file(target_filename);
        if (target_line_arg) {
            int target_line = atoi(&target_line_arg[1]);
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

        if (ch == 27) {
            nodelay(stdscr, TRUE);
            int next1 = getch();

            if (next1 == ERR) {
                nodelay(stdscr, FALSE);
                continue;
            }

            /* Direct ESC + Up/Down key bindings (Mac Terminal / Meta Mode) */
            if (next1 == 'k') {
                nodelay(stdscr, FALSE);
                swap_line_up();
                continue;
            } else if (next1 == 'j') {
                nodelay(stdscr, FALSE);
                swap_line_down();
                continue;
            }

            if (next1 == 'a' || next1 == 'A') {
                nodelay(stdscr, FALSE);
                show_about_window();
                continue;
            }

            if (next1 == 's' || next1 == 'S') {
                nodelay(stdscr, FALSE);
                save_as_file();
                continue;
            }

            if (next1 == 'b' || next1 == 'B') {
                nodelay(stdscr, FALSE);
                if (find_matching_bracket(current_line, cursor_x)) {
                    current_line = match_line;
                    cursor_x    = match_col;
                    int mdl = LINES - 4;
                    if (current_line < scroll_y)
                        scroll_y = current_line;
                    else if (current_line >= scroll_y + mdl)
                        scroll_y = current_line - mdl + 1;
                    if (scroll_y < 0) scroll_y = 0;
                } else {
                    snprintf(status_msg, sizeof(status_msg), "No matching bracket.");
                }
                continue;
            }

            /* Extended CSI Escape Sequences parsing */
            if (next1 == '[') {
                int next2 = getch();

                if (next2 == '1') {
                    int semicolon = getch();
                    int modifier  = getch();
                    int direction = getch();

                    if (semicolon == ';' && (modifier == '3' || modifier == '9' || modifier == '5')) {
                        if (direction == 'A') {
                            nodelay(stdscr, FALSE);
                            swap_line_up();
                            continue;
                        } else if (direction == 'B') {
                            nodelay(stdscr, FALSE);
                            swap_line_down();
                            continue;
                        }
                    }
                } else if (next2 == '2') {
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
            }
            nodelay(stdscr, FALSE);
        }

        clock_t now = clock();
        double ms_since_last = ((double)(now - last_input_time) / CLOCKS_PER_SEC) * 1000.0;
        last_input_time = now;

        if (!read_only_mode && ms_since_last < 4.0 && ch != CTRL_KEY('u') && ch != CTRL_KEY('y') && last_input_time != 0) {
            if (!paste_batch_active) {
                save_undo_state_batch(current_line, line_count - current_line);
                paste_batch_active = 1;
            }
        } else {
            paste_batch_active = 0;
        }

        status_msg[0] = '\0';

        if (got_fatal_signal) {
            if (is_modified && !read_only_mode) save_file();
            break;
        }

        if (file_mtime != 0 && strcmp(current_filename, "untitled.txt") != 0) {
            struct stat st;
            if (stat(current_filename, &st) == 0 && st.st_mtime != file_mtime) {
                file_mtime = st.st_mtime;
                move(LINES - 1, 0); clrtoeol();
                attron(COLOR_PAIR(PAIR_RED));
                printw("File changed on disk! Reload? (y/n): ");
                attroff(COLOR_PAIR(PAIR_RED));
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
                attron(COLOR_PAIR(PAIR_RED));
                printw("Unsaved changes! Quit anyway? (y/n): ");
                attroff(COLOR_PAIR(PAIR_RED));
                refresh();
                int confirm = getch();
                if (confirm == 'y' || confirm == 'Y') break;
                else continue;
            } else break;
        }
        else if (ch == CTRL_KEY('o')) interactive_open();
        else if (ch == CTRL_KEY('s')) save_file();
        else if (ch == KEY_ALT_S) save_as_file();
        else if (ch == CTRL_KEY('w')) {
            if (read_only_mode) {
                snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
            } else if (cursor_x > 0) {
                int orig = cursor_x;
                while (cursor_x > 0 && isspace((unsigned char)lines[current_line][cursor_x-1])) cursor_x--;
                while (cursor_x > 0 && !isspace((unsigned char)lines[current_line][cursor_x-1])) cursor_x--;
                record_bulk(current_line, 1);
                int line_len = (int)strlen(lines[current_line]);
                memmove(lines[current_line] + cursor_x,
                        lines[current_line] + orig,
                        line_len - orig + 1);
                is_modified = 1;
            }
        }
        else if (ch == KEY_F(3)) {
            read_only_mode = !read_only_mode;
            snprintf(status_msg, sizeof(status_msg), "Read-Only mode %s", read_only_mode ? "ON" : "OFF");
        }
        else if (ch == KEY_F(4)) {
            gutter_visible = !gutter_visible;
            snprintf(status_msg, sizeof(status_msg), "Gutter %s.", gutter_visible ? "on" : "off");
        }
        else if (ch == KEY_F(5)) {
            syntax_highlight_enabled = !syntax_highlight_enabled;
            snprintf(status_msg, sizeof(status_msg), "Syntax highlighting %s.", syntax_highlight_enabled ? "on" : "off");
        }
        else if (ch == CTRL_KEY('n')) {
            if (read_only_mode) {
                snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
            } else {
                record_bulk(current_line, 1);
                insert_line_at(current_line + 1, lines[current_line]);
                current_line++;
                is_modified = 1;
                snprintf(status_msg, sizeof(status_msg), "Line duplicated.");
            }
        }
        else if (ch == CTRL_KEY('f')) find_text();
        else if (ch == CTRL_KEY('?')) show_help_window();
        else if (ch == 28)                show_command_window("");   /* Ctrl+\ */
        else if (ch == KEY_ALT_G)         show_command_window("git ");
        else if (ch == CTRL_KEY('r')) replace_text();
        else if (ch == CTRL_KEY('g')) goto_line();
        else if (ch == CTRL_KEY('u')) undo();
        else if (ch == CTRL_KEY('y')) redo_op();
        else if (ch == CTRL_KEY('d')) delete_lines_interactive();
        else if (ch == CTRL_KEY('x')) {
            if (read_only_mode) {
                snprintf(status_msg, sizeof(status_msg), "File is Read-Only!");
            } else {
                overwrite_mode = !overwrite_mode;
                snprintf(status_msg, sizeof(status_msg), "Mode: %s", overwrite_mode ? "OVERWRITE" : "INSERT");
            }
        }
        else if (ch == CTRL_KEY('t')) { current_line = 0; cursor_x = 0; scroll_y = 0; }
        else if (ch == CTRL_KEY('b')) {
            current_line = line_count - 1; cursor_x = 0;
            int max_displayable_lines = LINES - 4;
            scroll_y = current_line - max_displayable_lines + 1;
            if (scroll_y < 0) scroll_y = 0;
        }
        else if (ch == CTRL_KEY('k') || ch == CTRL_KEY('K') || ch == 11) {
            /* If a mouse selection is active, cut just the selected text;
             * otherwise fall back to the interactive line-range cut. */
            if (sel_active) {
                cut_selection();
            } else {
                cut_lines_interactive();
            }
        }
        else if (ch == CTRL_KEY('c') || ch == CTRL_KEY('C')) {
            /* If a mouse selection is active, copy just the selected text;
             * otherwise fall back to the interactive line-range copy. */
            if (sel_active) {
                if (copy_selection()) {
                    int copied_len = sel_end_col - sel_start_col;
                    sel_active = 0;
                    snprintf(status_msg, sizeof(status_msg), "Copied %d character(s).", copied_len);
                }
            } else {
                copy_lines_interactive();
            }
        }
        else if (ch == CTRL_KEY('p')) {
            if (read_only_mode) {
                snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot paste.");
            } else if (clipboard_line && strlen(clipboard_line) > 0) {
                /* Try inline paste first (single-line selection clipboard);
                 * fall back to line-based paste for multi-line clipboard. */
                if (!paste_inline()) {
                    record_bulk(current_line, line_count - current_line);

                    const char *p = clipboard_line;
                    int inserted = 0;

                    while (*p != '\0') {
                        const char *next_nl = strchr(p, '\n');
                        size_t len = next_nl ? (size_t)(next_nl - p) : strlen(p);

                        char *line_buf = malloc(len + 1);
                        if (line_buf) {
                            memcpy(line_buf, p, len);
                            line_buf[len] = '\0';
                            insert_line_at(current_line + inserted, line_buf);
                            free(line_buf);
                            inserted++;
                        }

                        if (!next_nl) break;
                        p = next_nl + 1;
                    }

                    cursor_x = 0;
                    is_modified = 1;
                    snprintf(status_msg, sizeof(status_msg), "Pasted %d line(s).", inserted);
                }
            } else {
                snprintf(status_msg, sizeof(status_msg), "Clipboard is empty.");
            }
        }
        else if (ch == KEY_MOUSE) {
            handle_mouse_event();
        }
        else if (ch == KEY_UP) {
            if (current_line > 0) {
                current_line--;
                {
                    int gd_u = 1; int tmp = line_count; while (tmp >= 10) { tmp /= 10; gd_u++; }
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
                    }
                }
                int len = strlen(lines[current_line]);
                if (cursor_x > len) cursor_x = len;
            }
        }
        else if (ch == KEY_DOWN) {
            if (current_line < line_count - 1) {
                current_line++;
                int max_displayable_lines = LINES - 4;
                {
                    int gd_d = 1; int tmp = line_count; while (tmp >= 10) { tmp /= 10; gd_d++; }
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
                    }
                }
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
        else if (ch == KEY_HOME || ch == 1) {
            cursor_x = 0;
        }
        else if (ch == KEY_END || ch == CTRL_KEY('e')) {
            cursor_x = (int)strlen(lines[current_line]);
        }
        else if (ch == CTRL_KEY('a')) {
            int first_nws = 0;
            while (first_nws < (int)strlen(lines[current_line]) &&
                   isspace((unsigned char)lines[current_line][first_nws]))
                first_nws++;
            cursor_x = (cursor_x == first_nws) ? 0 : first_nws;
        }
        else if (ch == 9) {
            if (read_only_mode) {
                snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
            } else {
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
        }
        else if (ch == 353) {
            if (read_only_mode) {
                snprintf(status_msg, sizeof(status_msg), "File is Read-Only! Cannot modify.");
            } else {
                int is_makefile = (strstr(current_filename, "Makefile") != NULL);
                int tab_size = is_makefile ? 1 : 4;
                int removed = 0;
                record_bulk(current_line, 1);
                while (removed < tab_size && lines[current_line][0] == ' ') {
                    int ll = (int)strlen(lines[current_line]);
                    memmove(lines[current_line], lines[current_line] + 1, ll);
                    removed++;
                }
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
        }
        else if (ch == 10 || ch == 13) {
            handle_enter_key(paste_batch_active);
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            handle_backspace();
        }
        else if (ch == KEY_DC || ch == 330) {
            handle_delete_key();
        }
        else if (ch >= 32 && ch <= 126) {
            handle_printable_char(ch, paste_batch_active);
        }
        /* Multi-byte UTF-8: lead byte 0xC0–0xFF arrives as a positive int
         * from getch() when ncurses is in raw/noecho mode with setlocale set.
         * Collect the continuation bytes and insert the full sequence. */
        else if (ch >= 0xC0 && ch <= 0xFF) {
            handle_utf8_char(ch, paste_batch_active);
        }
    }

    for (int i = 0; i < undo_count; i++) free_op(&undo_buf[(undo_head + i) % UNDO_CAP]);
    free(undo_buf);
    for (int i = 0; i <= redo_top; i++) free_op(&redo_buf[i]);
    free(redo_buf);
    free(clipboard_line);
    tracker_free();
    for (int i = 0; i < line_count; i++) free(lines[i]);
    free(lines);

    printf("\033[?2004l");
    fflush(stdout);

    endwin();
    return 0;
}
