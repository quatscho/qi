/*
 * qi - A Lightweight Terminal Text Editor
 * Author: Christopher Camacho
 * Version: 1.1.2 (2026)
 *
 * A minimalist, ncurses-based text editor featuring dynamic line counting,
 * interactive search and replace, multi-line deletion tools, visual state
 * change tracking, and multi-language syntax highlighting.
 */
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <ncurses.h>
#include <termios.h>
#include <unistd.h>
#include <time.h>
#include "tracker.h"
#include "syntax.h"

#define MAX_LINE_LEN 512
#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_UNDO 500
#define VERSION "1.1.2"

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

/* ---------- undo ---------- */
typedef struct {
    int line_index;
    char original_text[MAX_LINE_LEN];
} LineDelta;

typedef struct {
    int is_batch;
    int num_lines;
    LineDelta deltas[150];
    int cursor_x;
    int current_line;
} UndoBatch;

UndoBatch undo_stack[MAX_UNDO];
int undo_stack_top = -1;
void save_undo_state_single(int line_idx);
void save_undo_state_batch(int start_line, int count);
void undo(void);

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

void save_undo_state_single(int line_idx) {
    if (undo_stack_top >= MAX_UNDO - 1) {
        for (int i = 0; i < MAX_UNDO - 1; i++)
            undo_stack[i] = undo_stack[i + 1];
        undo_stack_top--;
    }
    undo_stack_top++;
    undo_stack[undo_stack_top].is_batch = 0;
    undo_stack[undo_stack_top].num_lines = 1;
    undo_stack[undo_stack_top].deltas[0].line_index = line_idx;
    strncpy(undo_stack[undo_stack_top].deltas[0].original_text,
            lines[line_idx], MAX_LINE_LEN - 1);
    undo_stack[undo_stack_top].deltas[0].original_text[MAX_LINE_LEN - 1] = '\0';
    undo_stack[undo_stack_top].cursor_x = cursor_x;
    undo_stack[undo_stack_top].current_line = current_line;
}

void save_undo_state_batch(int start_line, int count) {
    int actual_count = (count > 150) ? 150 : count;
    if (undo_stack_top >= MAX_UNDO - 1) {
        for (int i = 0; i < MAX_UNDO - 1; i++)
            undo_stack[i] = undo_stack[i + 1];
        undo_stack_top--;
    }
    undo_stack_top++;
    undo_stack[undo_stack_top].is_batch = 1;
    undo_stack[undo_stack_top].num_lines = actual_count;
    for (int i = 0; i < actual_count; i++) {
        undo_stack[undo_stack_top].deltas[i].line_index = start_line + i;
        strncpy(undo_stack[undo_stack_top].deltas[i].original_text,
                lines[start_line + i], MAX_LINE_LEN - 1);
        undo_stack[undo_stack_top].deltas[i].original_text[MAX_LINE_LEN - 1] = '\0';
    }
    undo_stack[undo_stack_top].cursor_x = cursor_x;
    undo_stack[undo_stack_top].current_line = current_line;
}

void undo(void) {
    if (undo_stack_top < 0) {
        snprintf(status_msg, sizeof(status_msg), "Nothing to undo!");
        return;
    }
    UndoBatch *b = &undo_stack[undo_stack_top];
    if (b->is_batch) {
        /* Restore saved lines; truncate file to the first saved index */
        int first = b->deltas[0].line_index;
        while (line_count > first) remove_line_at(line_count - 1);
        for (int i = 0; i < b->num_lines; i++) {
            if (!ensure_capacity(line_count + 1)) break;
            lines[line_count] = xstrdup(b->deltas[i].original_text);
            line_count++;
        }
    } else {
        for (int i = 0; i < b->num_lines; i++)
            set_line(b->deltas[i].line_index, b->deltas[i].original_text);
    }
    current_line = b->current_line;
    cursor_x = b->cursor_x;
    undo_stack_top--;
    snprintf(status_msg, sizeof(status_msg), "Undo performed.");
}

/* ---------- file I/O ---------- */
void load_file(const char *filename) {
    undo_stack_top = -1;

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
        echo(); noraw();
        mvprintw(LINES - 1, 0, "Enter filename to save: ");
        clrtoeol(); refresh();
        getstr(filename);
        noecho(); raw();
        if (strlen(filename) > 0)
            strncpy(current_filename, filename, 256);
        else return;
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

/* ---------- screen rendering ---------- */
void draw_screen() {
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    init_pair(2, COLOR_RED, COLOR_BLACK);
    init_pair(3, COLOR_CYAN, COLOR_BLACK);
    init_pair(4, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(5, COLOR_GREEN, COLOR_BLACK);
    init_pair(6, COLOR_YELLOW, COLOR_BLACK);

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

    while (physical_row < 2 + max_displayable_lines && file_line_index < line_count) {
        move(physical_row, 0); clrtoeol();

        if (file_line_index == current_line) {
            attron(COLOR_PAIR(1));
            mvprintw(physical_row, 0, "%3d ", file_line_index + 1);
            attroff(COLOR_PAIR(1));
            attron(COLOR_PAIR(1) | A_BOLD);
            mvaddch(physical_row, 4, ACS_DIAMOND);
            attroff(COLOR_PAIR(1) | A_BOLD);
        } else {
            mvprintw(physical_row, 0, "%3d ", file_line_index + 1);
            mvaddch(physical_row, 4, ACS_VLINE);
        }

        char *line = lines[file_line_index];
        int len = strlen(line);
        int current_phys_row = physical_row;
        int current_phys_col = 6;

        move(current_phys_row, current_phys_col);

        /* Get syntax spans for this line */
        Span spans[MAX_SPANS];
        int nspans = syntax_spans(file_line_index, line, spans);

        /* Colour-pair lookup: TOK -> ncurses pair */
        static const int tok_pair[] = { 0, 2, 4, 5, 3 };

        /* Render character by character, applying span colours */
        int span_idx = 0;
        for (int j = 0; j < len; j++) {
            if (current_phys_col >= wrap_col) {
                current_phys_row++; current_phys_col = 6;
                if (current_phys_row < 2 + max_displayable_lines) {
                    move(current_phys_row, current_phys_col);
                    clrtoeol();
                } else break;
            }
            /* Advance past expired spans */
            while (span_idx < nspans && spans[span_idx].end <= j)
                span_idx++;
            /* Apply colour if inside an active span */
            int pair = 0;
            if (span_idx < nspans && j >= spans[span_idx].start && j < spans[span_idx].end)
                pair = tok_pair[spans[span_idx].type];
            if (pair) attron(COLOR_PAIR(pair));
            printw("%c", line[j]);
            if (pair) attroff(COLOR_PAIR(pair));
            current_phys_col++;
        }
        physical_row = current_phys_row + 1;
        file_line_index++;
    }

    /* clear any remaining rows below the last rendered line */
    while (physical_row < 2 + max_displayable_lines) {
        move(physical_row, 0); clrtoeol();
        physical_row++;
    }

    /* Column-81 margin guide */
    if (COLS > 81) {
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
    attron(COLOR_PAIR(3));
    printw(" %s", status_msg);
    attroff(COLOR_PAIR(3));

    /* Cursor placement — use the same wrap width as draw_screen (COLS - 7) */
    int text_width = COLS - 7;
    int cursor_physical_row = 2;
    for (int i = scroll_y; i < current_line; i++) {
        int l_len = strlen(lines[i]);
        int l_rows = (l_len == 0) ? 1 : (l_len / text_width) + 1;
        cursor_physical_row += l_rows;
    }
    cursor_physical_row += (cursor_x / text_width);
    int cursor_physical_col = 6 + (cursor_x % text_width);

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
    mvprintw(LINES - 1, 0, "%s", prompt); clrtoeol(); refresh();
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
            mvprintw(LINES - 1, prompt_len + idx - 1, "%c", ch); refresh();
        }
    }
    if (strlen(search_str) == 0) return;

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
            snprintf(status_msg, sizeof(status_msg), "Found match at line %d.", current_line + 1); break;
        } else if (ch == 27) { status_msg[0] = '\0'; break; }
        else if (ch == KEY_RIGHT || ch == KEY_DOWN) current_match_idx = (current_match_idx + 1) % match_count;
        else if (ch == KEY_LEFT || ch == KEY_UP) current_match_idx = (current_match_idx - 1 + match_count) % match_count;
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

    save_undo_state_single(current_line);
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
    const char *prompt = "Delete lines (e.g., 3, 5, 10-25): "; int prompt_len = strlen(prompt);
    echo(); mvprintw(LINES-1,0,"%s",prompt); clrtoeol(); refresh();
    while (idx < (int)sizeof(input) - 1) {
        int ch = getch();
        if (ch == 27) { noecho(); status_msg[0]='\0'; return; }
        else if (ch == 10 || ch == 13) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) { idx--; input[idx]='\0'; mvprintw(LINES-1,prompt_len+idx," "); move(LINES-1,prompt_len+idx); refresh(); }
        } else if (ch >= 32 && ch <= 126) { input[idx++]=(char)ch; input[idx]='\0'; }
    }
    noecho();
    if (strlen(input) == 0) return;

    char saved_input_copy[256];
    strncpy(saved_input_copy, input, sizeof(saved_input_copy) - 1);
    saved_input_copy[sizeof(saved_input_copy)-1] = '\0';

    /* Build deletion bitmap — use heap to avoid VLA issues on large files */
    char *to_delete = calloc(line_count, sizeof(char));
    if (!to_delete) return;

    save_undo_state_batch(0, line_count);

    char *token = strtok(input, ",");
    while (token != NULL) {
        while (*token == ' ') token++;
        char *dash = strchr(token, '-');
        if (dash) {
            int start = atoi(token), end = atoi(dash + 1);
            if (start > 0 && end >= start)
                for (int i = start; i <= end && i <= line_count; i++) to_delete[i-1] = 1;
        } else {
            int ln = atoi(token);
            if (ln > 0 && ln <= line_count) to_delete[ln-1] = 1;
        }
        token = strtok(NULL, ",");
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

    if (deleted_count > 0)
        snprintf(status_msg, sizeof(status_msg), "Deleted lines %s", saved_input_copy);
    else
        snprintf(status_msg, sizeof(status_msg), "No lines deleted.");
}

/* ---------- help window ---------- */
void show_help_window() {
    int height = 18, width = 50;
    int start_y = (LINES - height) / 2;
    int start_x = (COLS - width) / 2;
    WINDOW *help_win = newwin(height, width, start_y, start_x);
    keypad(help_win, TRUE);
    box(help_win, 0, 0);
    wattron(help_win, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(help_win, 0, (width - 10) / 2, " qi v%s ", VERSION);
    wattroff(help_win, COLOR_PAIR(1) | A_BOLD);
    const char *help[] = {
        "Ctrl+W  Save file",
        "Ctrl+O  Open file",
        "Ctrl+Q  Quit",
        "Ctrl+F  Find text",
        "Ctrl+R  Find & Replace",
        "Ctrl+G  Go to line",
        "Ctrl+U  Undo",
        "Ctrl+D  Delete line(s)",
        "Ctrl+X  Toggle Insert/Overwrite",
        "Ctrl+T  Top of file",
        "Ctrl+B  Bottom of file",
        "Ctrl+?  This help screen",
        "",
        "Press any key to close..."
    };
    for (int i = 0; i < 14; i++)
        mvwprintw(help_win, 2 + i, 2, "%s", help[i]);
    wrefresh(help_win);
    wgetch(help_win);
    delwin(help_win);
    touchwin(stdscr);
    refresh();
}

/* ---------- main ---------- */
int main(int argc, char *argv[]) {
    struct termios tty;
    if (tcgetattr(STDIN_FILENO, &tty) == 0) {
        tty.c_iflag &= ~IXON;
        tcsetattr(STDIN_FILENO, TCSANOW, &tty);
    }
    printf("\e[?2004l"); fflush(stdout);

    initscr();
    set_escdelay(25);
    raw();
    keypad(stdscr, TRUE);
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    noecho();
    curs_set(1);

    /* Initialise tracker with a modest hint; it only uses 1 byte per line */
    tracker_init(1024, 1);

    memset(undo_stack, 0, sizeof(undo_stack));

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

        clock_t now = clock();
        double ms_since_last = ((double)(now - last_input_time) / CLOCKS_PER_SEC) * 1000.0;
        last_input_time = now;

        if (ms_since_last < 4.0 && ch != CTRL_KEY('u') && last_input_time != 0) {
            if (!paste_batch_active) {
                save_undo_state_batch(current_line, line_count - current_line);
                paste_batch_active = 1;
            }
        } else {
            paste_batch_active = 0;
        }

        status_msg[0] = '\0';

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
        else if (ch == CTRL_KEY('w')) save_file();
        else if (ch == CTRL_KEY('f')) find_text();
        else if (ch == CTRL_KEY('?')) show_help_window();
        else if (ch == CTRL_KEY('r')) replace_text();
        else if (ch == CTRL_KEY('g')) goto_line();
        else if (ch == CTRL_KEY('u')) undo();
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
        else if (ch == KEY_UP) {
            if (current_line > 0) {
                current_line--;
                int available_width = COLS - 7;
                int visual_rows_above = 0;
                for (int i = scroll_y; i < current_line; i++) {
                    int l_len = strlen(lines[i]);
                    visual_rows_above += (l_len == 0) ? 1 : (l_len / available_width) + 1;
                }
                if (visual_rows_above < 3 && scroll_y > 0) {
                    while (scroll_y > 0 && visual_rows_above < 3) {
                        scroll_y--;
                        visual_rows_above = 0;
                        for (int i = scroll_y; i < current_line; i++) {
                            int l_len = strlen(lines[i]);
                            visual_rows_above += (l_len == 0) ? 1 : (l_len / available_width) + 1;
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
                int available_width = COLS - 7;
                int visual_row_index = 0;
                for (int i = scroll_y; i <= current_line; i++) {
                    int l_len = strlen(lines[i]);
                    int l_rows = (l_len == 0) ? 1 : (l_len / available_width) + 1;
                    if (i < current_line) visual_row_index += l_rows;
                    else visual_row_index += (cursor_x / available_width);
                }
                if (max_displayable_lines - visual_row_index <= 3) {
                    scroll_y++;
                    if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
                    if (scroll_y < 0) scroll_y = 0;
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
        else if (ch == KEY_END || ch == 5) {
            cursor_x = (int)strlen(lines[current_line]);
            (void)0;
        }
        else if (ch == 9) {
            /* Tab */
            int len = strlen(lines[current_line]);
            int is_makefile = (strstr(current_filename, "Makefile") != NULL);
            int tab_size = is_makefile ? 1 : 4;
            char *new_line = malloc(len + tab_size + 1);
            if (new_line) {
                save_undo_state_single(current_line);
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
        else if (ch == 10 || ch == 13) {
            /* Enter */
            if (!paste_batch_active) save_undo_state_single(current_line);
            /* Split current line at cursor */
            char *tail = xstrdup(lines[current_line] + cursor_x);
            lines[current_line][cursor_x] = '\0';
            insert_line_at(current_line + 1, tail);
            free(tail);
            current_line++; cursor_x = 0;
            is_modified = 1;

            int max_displayable_lines = LINES - 4;
            int available_width = COLS - 7;
            int visual_row_index = 0;
            for (int i = scroll_y; i < current_line; i++) {
                int l_len = strlen(lines[i]);
                visual_row_index += (l_len == 0) ? 1 : (l_len / available_width) + 1;
            }
            if (max_displayable_lines - visual_row_index <= 3) {
                scroll_y++;
                if (scroll_y > line_count - max_displayable_lines) scroll_y = line_count - max_displayable_lines;
                if (scroll_y < 0) scroll_y = 0;
            }
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (cursor_x > 0) {
                save_undo_state_single(current_line);
                int len = strlen(lines[current_line]);
                memmove(lines[current_line] + cursor_x - 1,
                        lines[current_line] + cursor_x,
                        len - cursor_x + 1);
                cursor_x--; is_modified = 1;
            } else if (current_line > 0) {
                save_undo_state_single(current_line);
                int target = current_line - 1;
                int target_len = strlen(lines[target]);
                int cur_len = strlen(lines[current_line]);
                char *merged = malloc(target_len + cur_len + 1);
                if (merged) {
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
                save_undo_state_single(current_line);
                memmove(lines[current_line] + cursor_x,
                        lines[current_line] + cursor_x + 1,
                        len - cursor_x);
                is_modified = 1;
            } else if (current_line < line_count - 1) {
                int next = current_line + 1;
                int next_len = strlen(lines[next]);
                char *merged = malloc(len + next_len + 1);
                if (merged) {
                    save_undo_state_single(current_line);
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
                if (cursor_x == 0 || (lines[current_line][cursor_x-1] == ' ' && ch != ' '))
                    save_undo_state_single(current_line);
                else if (mod_count == 0 || cursor_x % 10 == 0)
                    save_undo_state_single(current_line);
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

    undo_stack_top = -1;
    tracker_free();
    for (int i = 0; i < line_count; i++) free(lines[i]);
    free(lines);
    endwin();
    return 0;
}
