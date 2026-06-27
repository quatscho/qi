/* 
 * qi - A Lightweight Terminal Text Editor
 * Author: Christopher Camacho
 * Version: 1.0.14 (2026)
 *
 * A minimalist, ncurses-based text editor featuring dynamic line counting,
 * interactive search and replace, multi-line deletion tools, visual state 
 * change tracking, and basic C syntax highlighting.
 */

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <ncurses.h>
#include <termios.h> 
#include <unistd.h> 
#include "tracker.h"

#define MAX_LINES 50000
#define MAX_LINE_LEN 512
#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_UNDO 50
#define VERSION "1.0.14"

typedef struct {
    char buffer[MAX_LINES][MAX_LINE_LEN];
    int line_count;
    int current_line;
    int cursor_x;
    int scroll_y;
    int is_modified;
} UndoState;

UndoState undo_stack[MAX_UNDO];
int undo_stack_top = -1; // -1 means stack is currently empty

// Global state
char buffer[MAX_LINES][MAX_LINE_LEN];
int line_count = 0;
int current_line = 0;
int cursor_x = 0;
int scroll_y = 0;
char current_filename[256] = "untitled.txt";
char status_msg[256] = "";
int is_modified = 0;
int line_modified[MAX_LINES] = {0};

void save_undo_state() {
    // If the stack is full, shift everything left to discard the oldest state
    if (undo_stack_top >= MAX_UNDO - 1) {
        for (int i = 0; i < MAX_UNDO - 1; i++) {
            undo_stack[i] = undo_stack[i + 1];
        }
        undo_stack_top--;
    }

    undo_stack_top++;
    
    // Copy current state into the stack slot
    memcpy(undo_stack[undo_stack_top].buffer, buffer, sizeof(buffer));
    undo_stack[undo_stack_top].line_count = line_count;
    undo_stack[undo_stack_top].current_line = current_line;
    undo_stack[undo_stack_top].cursor_x = cursor_x;
    undo_stack[undo_stack_top].scroll_y = scroll_y;
    undo_stack[undo_stack_top].is_modified = is_modified;

    is_modified = 1;
} // End save_undo_state

void undo() {
    if (undo_stack_top < 0) {
        snprintf(status_msg, sizeof(status_msg), "Nothing to undo!");
        return;
    }

    // Restore state from stack top
    memcpy(buffer, undo_stack[undo_stack_top].buffer, sizeof(buffer));
    line_count = undo_stack[undo_stack_top].line_count;
    current_line = undo_stack[undo_stack_top].current_line;
    cursor_x = undo_stack[undo_stack_top].cursor_x;
    scroll_y = undo_stack[undo_stack_top].scroll_y;
    is_modified = undo_stack[undo_stack_top].is_modified;

    undo_stack_top--; // Pop it off
    snprintf(status_msg, sizeof(status_msg), "Undo!");
} // End undo

// Function to handle the "Open" command
// 1. Silent file loader used by both main() and the menu
void load_file(const char *filename) {
    undo_stack_top = -1; // --- ADD THIS TO CLEAR UNDO ON LOAD ---

    memset(line_modified, 0, sizeof(line_modified));

    FILE *fp = fopen(filename, "r");
    if (fp) {
        line_count = 0;
        while (fgets(buffer[line_count], MAX_LINE_LEN, fp) && line_count < MAX_LINES) {
            buffer[line_count][strcspn(buffer[line_count], "\r\n")] = 0;
            line_count++;
        }
        fclose(fp);
        strncpy(current_filename, filename, sizeof(current_filename) - 1);
        
        // Reset view positions to the top of the newly loaded file
        scroll_y = 0;
        current_line = 0;
        cursor_x = 0;
        is_modified = 0;
    } else {
        // If file doesn't exist, treat it as a new file under that name
        strncpy(current_filename, filename, sizeof(current_filename) - 1);
        line_count = 1;
        buffer[0][0] = '\0';
        is_modified = 0;
    }
} // End load_file

// 2. Interactive menu command when you press Ctrl+O
void interactive_open() {
    char filename[256];
    int idx = 0;
    filename[0] = '\0';

    // Keep raw() active so we can catch individual keys like ESC instantly!
    echo();

    mvprintw(LINES - 1, 0, "Enter filename to open (ESC to cancel): ");
    clrtoeol(); 
    refresh();  

    while (idx < (int)sizeof(filename) - 1) {
        int ch = getch();

        if (ch == 27) { // ESC key caught instantly
            noecho();
            status_msg[0] = '\0';
            return;
        }
        else if (ch == 10 || ch == 13) { // Enter key finishes input
            break;
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { // Handle backspace
            if (idx > 0) {
                idx--;
                filename[idx] = '\0';
                // Move back, wipe character visually, and step back again
                mvprintw(LINES - 1, 40 + idx, " ");
                move(LINES - 1, 40 + idx);
                refresh();
            }
        }
        else if (ch >= 32 && ch <= 126) { // Visible characters
            filename[idx++] = (char)ch;
            filename[idx] = '\0';
        }
    }

    noecho(); 

    // Only load if the user actually typed a name
    if (strlen(filename) > 0) {
        FILE *fp = fopen(filename, "r");
        if (fp) {
            fclose(fp);
            load_file(filename); // Pass it to our silent loader[span_4](start_span)[span_4](end_span)
        } else {
            mvprintw(LINES - 1, 0, "File not found! Press any key...");
            clrtoeol();
            refresh();
            raw(); // Ensure raw state is safe before blocking
            getch();
        }
    }
} // End interactive_open

// Function to handle the "Save" command
void save_file() {
    // If it's a new file (untitled), interactively prompt for a name
    if (strcmp(current_filename, "untitled.txt") == 0) {
        char filename[256];
        echo();
        noraw(); // Use noraw() instead of nocbreak() since we use raw() mode now

        mvprintw(LINES - 1, 0, "Enter filename to save: ");
        clrtoeol();
        refresh();

        getstr(filename);

        noecho();
        raw(); // Re-enable raw mode

        if (strlen(filename) > 0) {
            strncpy(current_filename, filename, 256);
        } else {
            return; // User hit enter without typing anything, cancel save
        }
    }

    // Now safely write out to current_filename (either loaded on start or typed above)
    FILE *fp = fopen(current_filename, "w");
    if (fp) {
        for (int i = 0; i < line_count; i++) {
            fprintf(fp, "%s\n", buffer[i]);
        }
        fclose(fp);

        is_modified = 0;

        memset(line_modified, 0, sizeof(line_modified));

        snprintf(status_msg, sizeof(status_msg), "Saved successfully to '%s'!", current_filename);
    } else {
        mvprintw(LINES - 1, 0, "Error: Could not save file! Press any key...");
        clrtoeol();
        refresh();
        getch(); // Keep this one blocking because it's a critical error
    }
} // End save_file

void draw_screen() {
    clear();
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK); 
    init_pair(2, COLOR_RED, COLOR_BLACK);
    
    // --- NEW COLOR INITIALIZATIONS FOR SYNTAX ---
    init_pair(3, COLOR_CYAN, COLOR_BLACK);   // Keywords (if, while, etc.)
    init_pair(4, COLOR_MAGENTA, COLOR_BLACK);// Strings & Numbers
    init_pair(5, COLOR_GREEN, COLOR_BLACK);  // Comments (//)

    // --- NEW COLOR PAIR FOR NEW TEXT ---
    init_pair(6, COLOR_YELLOW, COLOR_BLACK);

    // 1. Draw the top header area (Row 0)
    move(0, 0);
    clrtoeol();
    attron(COLOR_PAIR(1));
    if (is_modified) {
        printw(" File: %s * (unsaved) (%d lines)", current_filename, line_count);
    } else {
        printw(" File: %s (%d lines)", current_filename, line_count);
    }
    attroff(COLOR_PAIR(1));

    // 2. Draw the thin top horizontal separator line (Row 1)
    move(1, 0);
    clrtoeol();
    for (int x = 0; x < COLS; x++) {
        mvaddch(1, x, ACS_HLINE);
    }

    int max_displayable_lines = LINES - 4; 
    int physical_row = 2; // Start rendering text at physical row 2

    // Render the file content lines
    for (int i = 0; i < max_displayable_lines; i++) {
        int file_line_index = scroll_y + i;
        
        if (file_line_index >= line_count) break;
        if (physical_row >= LINES - 2) break; // Don't overwrite the bottom border!

        // Draw the line number at the start of this buffer line's physical space
        if (file_line_index == current_line) {
            attron(COLOR_PAIR(1));
            mvprintw(physical_row, 0, "%3d ", file_line_index + 1); 
            attroff(COLOR_PAIR(1));
        } else {
            mvprintw(physical_row, 0, "%3d ", file_line_index + 1);
        }

        // Draw the vertical separator
        mvaddch(physical_row, 4, ACS_VLINE);

        // --- UPGRADED SYNTAX HIGHLIGHT SCANNER ---
        char *line = buffer[file_line_index];
        int len = strlen(line);
        int in_string = 0;
        int in_char = 0;

        // Track where we are drawing *within* the editor line text area
        int available_width = COLS - 6; 
        int current_phys_row = physical_row;
        int current_phys_col = 6;

        move(current_phys_row, current_phys_col);

        int leading_space = 0;
        while (leading_space < len && (line[leading_space] == ' ' || line[leading_space] == '\t')) {
            printw("%c", line[leading_space]);
            current_phys_col++;
            if (current_phys_col >= COLS) {
                current_phys_row++;
                current_phys_col = 6;
                move(current_phys_row, current_phys_col);
            }
            leading_space++;
        }

        for (int j = leading_space; j < len; j++) {
            // Check if we need to wrap the screen row manually for printing
            if (current_phys_col >= COLS) {
                current_phys_row++;
                current_phys_col = 6;
                move(current_phys_row, current_phys_col);
            }

            if (j == leading_space && line[j] == '#') {
                attron(COLOR_PAIR(3));
                printw("%s", &line[j]);
                attroff(COLOR_PAIR(3));
                break;
            }

            if (!in_string && !in_char && line[j] == '/' && line[j+1] == '/') {
                attron(COLOR_PAIR(5));
                printw("%s", &line[j]);
                attroff(COLOR_PAIR(5));
                break; 
            }

            if (line[j] == '\'' && !in_string) {
                if (in_char) {
                    printw("%c", line[j]);
                    attroff(COLOR_PAIR(4));
                    in_char = 0;
                } else {
                    attron(COLOR_PAIR(4));
                    printw("%c", line[j]);
                    in_char = 1;
                }
                current_phys_col++;
                continue;
            }
            if (in_char) {
                printw("%c", line[j]);
                current_phys_col++;
                continue;
            }

            if (line[j] == '"' && !in_char) {
                if (in_string) {
                    printw("%c", line[j]);
                    attroff(COLOR_PAIR(4));
                    in_string = 0;
                } else {
                    attron(COLOR_PAIR(4));
                    printw("%c", line[j]);
                    in_string = 1;
                }
                current_phys_col++;
                continue;
            }
            if (in_string) {
                printw("%c", line[j]);
                current_phys_col++;
                continue;
            }

            if (j == 0 || (!isalnum((unsigned char)line[j-1]) && line[j-1] != '_')) {
                char *keywords[] = {
                    "if", "else", "while", "for", "return", "break", "continue", "switch", "case", "default",
                    "int", "char", "void", "struct", "typedef", "double", "float", "long", "short", "unsigned",
                    "static", "const", "extern", "sizeof"
                };
                int num_keywords = sizeof(keywords) / sizeof(keywords[0]);
                int matched = 0;

                for (int k = 0; k < num_keywords; k++) {
                    int kw_len = strlen(keywords[k]);
                    if (strncmp(&line[j], keywords[k], kw_len) == 0) {
                        char next = line[j + kw_len];
                        if (!isalnum((unsigned char)next) && next != '_') {
                            attron(COLOR_PAIR(3));
                            // Printing a keyword word safely character-by-character to monitor screen margins
                            for (int m = 0; m < kw_len; m++) {
                                if (current_phys_col >= COLS) {
                                    current_phys_row++;
                                    current_phys_col = 6;
                                    move(current_phys_row, current_phys_col);
                                }
                                printw("%c", keywords[k][m]);
                                current_phys_col++;
                            }
                            attroff(COLOR_PAIR(3));
                            j += (kw_len - 1); 
                            matched = 1;
                            break;
                        }
                    }
                }
                if (matched) continue;
            }

            if (isdigit((unsigned char)line[j])) {
                attron(COLOR_PAIR(4));
                printw("%c", line[j]);
                attroff(COLOR_PAIR(4));
            } else {
                if (tracker_is_modified(file_line_index, j)) {
                    attron(COLOR_PAIR(1));
                    printw("%c", line[j]);
                    attroff(COLOR_PAIR(1));
                } else { 
                    printw("%c", line[j]);
                }
            }
            current_phys_col++;
        }

        // Advance physical row allocation by how many vertical tracks this buffer line consumed
        int lines_consumed = 1 + (len / available_width);
        physical_row += lines_consumed;
    }

    // 3. Draw the bottom horizontal separator line (LINES - 2)
    move(LINES - 2, 0);
    clrtoeol();
    for (int x = 0; x < COLS; x++) {
        mvaddch(LINES - 2, x, ACS_HLINE); 
    }    

    // 4. Draw the absolute bottom command row (LINES - 1)
    move(LINES - 1, 0);
    clrtoeol();
    
    if (strlen(status_msg) > 0) {
        attron(COLOR_PAIR(1));
        mvprintw(LINES - 1, 0, "%.*s", COLS - 1, status_msg);
        attroff(COLOR_PAIR(1));
    } else {
        if (!is_modified) {
            // Initial state: Show the standard editor tagline
            mvprintw(LINES - 1, 0, "qi text editor, v.%s (c) 2026, Christopher Camacho (^? for Help)", VERSION);
        } else {
            // Modified state: Calculate metrics using buffer and tracker data
            int total_chars = 0;
            int modified_chars = 0;
            int modified_lines = 0;

            for (int i = 0; i < line_count; i++) {
                int len = strlen(buffer[i]);
                total_chars += len;

                int line_has_mod = 0;
                for (int j = 0; j < len; j++) {
                    if (tracker_is_modified(i, j)) {
                        modified_chars++;
                        line_has_mod = 1;
                    }
                }
                if (line_has_mod) {
                    modified_lines++;
                }
            }

            // Display live counts while keeping the help hint persistent
            mvprintw(LINES - 1, 0, "Lines Mod: %d | Chars Mod: %d | Total Chars: %d | (^? for Help)", 
                     modified_lines, modified_chars, total_chars);
        }
    }
    
    // --- INTEGRATED PHYSICAL CURSOR MATH FOR WRAPPING ---
    int cursor_physical_row = 2;
    int available_width = COLS - 6;

    for (int i = scroll_y; i < current_line; i++) {
        int l_len = strlen(buffer[i]);
        int l_rows = (l_len / available_width) + 1;
        if (l_len == 0) l_rows = 1;
        cursor_physical_row += l_rows;
    }

    cursor_physical_row += (cursor_x / available_width);
    int cursor_physical_col = 6 + (cursor_x % available_width);

    if (cursor_physical_row < LINES - 2) {
        move(cursor_physical_row, cursor_physical_col);
    } else {
        move(LINES - 3, COLS - 1); // Clamp gracefully if out of visual view bounds
    }

    refresh();
} // END draw_screen

void find_text() {
    char search_str[128];
    int idx = 0;
    search_str[0] = '\0';
    const char *prompt = "Find: ";
    int prompt_len = strlen(prompt);

    echo();
    mvprintw(LINES - 1, 0, "%s", prompt);
    clrtoeol();
    refresh();

    while (idx < (int)sizeof(search_str) - 1) {
        int ch = getch();
        if (ch == 27) { // ESC
            noecho();
            status_msg[0] = '\0';
            return;
        }
        else if (ch == 10 || ch == 13) { // Enter
            break;
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--;
                search_str[idx] = '\0';
                mvprintw(LINES - 1, prompt_len + idx, " ");
                move(LINES - 1, prompt_len + idx);
                refresh();
            }
        }
        else if (ch >= 32 && ch <= 126) {
            search_str[idx++] = (char)ch;
            search_str[idx] = '\0';
        }
    }
    noecho();

    if (strlen(search_str) == 0) return;

    // Structure to hold coordinates of matches
    struct { int line; int col; } matches[500];
    int match_count = 0;
    int current_match_idx = 0;

    // 1. Scan entire file buffer for occurrences
    for (int i = 0; i < line_count; i++) {
        char *ptr = buffer[i];
        while ((ptr = strstr(ptr, search_str)) != NULL) {
            if (match_count < 500) {
                matches[match_count].line = i;
                matches[match_count].col = (int)(ptr - buffer[i]);
                match_count++;
            }
            ptr++; // Move forward 1 char to catch overlapping matches if any
        }
    }

    if (match_count == 0) {
        snprintf(status_msg, sizeof(status_msg), "No matches found for '%s'.", search_str);
        return;
    }

    // 2. Interactive Navigation Loop
    while (1) {
        // Snap view to the currently selected match index
        current_line = matches[current_match_idx].line;
        cursor_x = matches[current_match_idx].col;

        // Keep viewport camera pinned to center on match if possible
        int max_displayable_lines = LINES - 4;
        scroll_y = current_line - (max_displayable_lines / 2);
        if (scroll_y < 0) scroll_y = 0;
        if (scroll_y > line_count - max_displayable_lines) {
            scroll_y = line_count - max_displayable_lines;
        }
        if (scroll_y < 0) scroll_y = 0;

        // Force a UI repaint to highlight the match position immediately
        draw_screen(); 
        snprintf(status_msg, sizeof(status_msg), 
                 "Match %d of %d [Next: Right/Down | Prev: Left/Up | Enter: Done]", 
                 current_match_idx + 1, match_count);
        draw_screen(); 

        int ch = getch();
        if (ch == 10 || ch == 13) { // Enter confirms selection
            snprintf(status_msg, sizeof(status_msg), "Found match at line %d.", current_line + 1);
            break; 
        } 
        else if (ch == 27) { // Escape clears status and drops out
            status_msg[0] = '\0';
            break; 
        } 
        else if (ch == KEY_RIGHT || ch == KEY_DOWN) {
            // Cycle forward
            current_match_idx = (current_match_idx + 1) % match_count;
        } 
        else if (ch == KEY_LEFT || ch == KEY_UP) {
            // Cycle backward
            current_match_idx = (current_match_idx - 1 + match_count) % match_count;
        }
    }
} // End find_text

void replace_text() {
    char search_str[128];
    char replace_str[128];
    int idx = 0;
    search_str[0] = '\0';
    replace_str[0] = '\0';
    
    echo();

    // Prompt 1: Find text
    const char *prompt1 = "Find text to replace: ";
    int p1_len = strlen(prompt1);
    mvprintw(LINES - 1, 0, "%s", prompt1);
    clrtoeol();
    refresh();

    while (idx < (int)sizeof(search_str) - 1) {
        int ch = getch();
        if (ch == 27) { noecho(); status_msg[0] = '\0'; return; }
        else if (ch == 10 || ch == 13) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--; search_str[idx] = '\0';
                mvprintw(LINES - 1, p1_len + idx, " ");
                move(LINES - 1, p1_len + idx); refresh();
            }
        }
        else if (ch >= 32 && ch <= 126) {
            search_str[idx++] = (char)ch; search_str[idx] = '\0';
        }
    }
    if (strlen(search_str) == 0) { noecho(); return; }

    // Prompt 2: Replace with
    idx = 0;
    const char *prompt2 = "Replace with: ";
    int p2_len = strlen(prompt2);
    mvprintw(LINES - 1, 0, "%s", prompt2);
    clrtoeol();
    refresh();

    while (idx < (int)sizeof(replace_str) - 1) {
        int ch = getch();
        if (ch == 27) { noecho(); status_msg[0] = '\0'; return; }
        else if (ch == 10 || ch == 13) break;
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--; replace_str[idx] = '\0';
                mvprintw(LINES - 1, p2_len + idx, " ");
                move(LINES - 1, p2_len + idx); refresh();
            }
        }
        else if (ch >= 32 && ch <= 126) {
            replace_str[idx++] = (char)ch; replace_str[idx] = '\0';
        }
    }
    noecho();

    // 3. Collect Match Coordinates
    struct { int line; int col; } matches[500];
    int match_count = 0;
    
    for (int i = 0; i < line_count; i++) {
        char *ptr = buffer[i];
        while ((ptr = strstr(ptr, search_str)) != NULL) {
            if (match_count < 500) {
                matches[match_count].line = i;
                matches[match_count].col = (int)(ptr - buffer[i]);
                match_count++;
            }
            ptr += strlen(search_str); // Advance past this match
        }
    }

    if (match_count == 0) {
        snprintf(status_msg, sizeof(status_msg), "No matches found for '%s'.", search_str);
        return;
    }

    // Save a single checkpoint so the entire batch or any single choice can be undone!
    save_undo_state();

    int current_idx = 0;
    int replaced_count = 0;
    int force_all = 0; // Flags if user pressed 'all'

    // 4. Interactive Replace Loop
    while (current_idx < match_count) {
        int line = matches[current_idx].line;
        int col = matches[current_idx].col;

        // Snap view to match
        current_line = line;
        cursor_x = col;
        int max_displayable_lines = LINES - 4;
        scroll_y = current_line - (max_displayable_lines / 2);
        if (scroll_y < 0) scroll_y = 0;

        void draw_screen();
        
        int choice = 'n';
        if (!force_all) {
            snprintf(status_msg, sizeof(status_msg), 
                     "Match %d of %d: Replace? (y: Yes | n: No | a: All | q: Quit/ESC)", 
                     current_idx + 1, match_count);
            draw_screen();
            choice = getch();
        } else {
            choice = 'y';
        }

        if (choice == 'q' || choice == 27) {
            break;
        }
        else if (choice == 'a') {
            force_all = 1;
            choice = 'y';
        }

        if (choice == 'y') {
            char temp[MAX_LINE_LEN] = "";
            int search_len = strlen(search_str);
            int replace_len = strlen(replace_str);

            // Safety limit check
            if (strlen(buffer[line]) - search_len + replace_len < MAX_LINE_LEN) {
                // Construct modified line string safely
                strncpy(temp, buffer[line], col);
                temp[col] = '\0';
                strcat(temp, replace_str);
                strcat(temp, &buffer[line][col + search_len]);
                strcpy(buffer[line], temp);

                replaced_count++;

                // Offset subsequent matches on this EXACT line because string shifted sizes
                int delta = replace_len - search_len;
                for (int j = current_idx + 1; j < match_count; j++) {
                    if (matches[j].line == line) {
                        matches[j].col += delta;
                    } else {
                        break; // Matches are gathered in order, safely break to next line
                    }
                }
            }
        }
        current_idx++;
    }

    if (replaced_count > 0) {
        snprintf(status_msg, sizeof(status_msg), "Replaced %s with %s (%d instances%s)", search_str, replace_str, replaced_count, replaced_count == 1 ? "" : "s");
    } else {
        snprintf(status_msg, sizeof(status_msg), "No replacements made.");
    }
} // End replace_text

void goto_line() {
    char line_input[32];
    int idx = 0;
    line_input[0] = '\0';
    const char *prompt = "Go to line: ";
    int prompt_len = strlen(prompt);

    echo();
    mvprintw(LINES - 1, 0, "%s", prompt);
    clrtoeol();
    refresh();

    while (idx < (int)sizeof(line_input) - 1) {
        int ch = getch();
        if (ch == 27) { // ESC
            noecho();
            status_msg[0] = '\0';
            return;
        }
        else if (ch == 10 || ch == 13) { // Enter
            break;
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--;
                line_input[idx] = '\0';
                mvprintw(LINES - 1, prompt_len + idx, " ");
                move(LINES - 1, prompt_len + idx);
                refresh();
            }
        }
        else if (ch >= 32 && ch <= 126) {
            // Only accept digits for Go To Line configuration
            if (isdigit((unsigned char)ch)) {
                line_input[idx++] = (char)ch;
                line_input[idx] = '\0';
            }
        }
    }
    noecho();

    if (strlen(line_input) == 0) return;

    int target = atoi(line_input);

    if (target < 1 || target > line_count) {
        snprintf(status_msg, sizeof(status_msg), "Line %d out of bounds! (Total lines: %d)", target, line_count);
        return;
    }

    current_line = target - 1;
    cursor_x = 0;

    int max_displayable_lines = LINES - 4;
    scroll_y = current_line - (max_displayable_lines / 2);
    
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > line_count - max_displayable_lines) {
        scroll_y = line_count - max_displayable_lines;
        if (scroll_y < 0) scroll_y = 0;
    }
} // End goto_line

void delete_lines_interactive() {
    char input[256];
    int idx = 0;
    input[0] = '\0';
    const char *prompt = "Delete lines (e.g., 3, 5, 10-25): ";
    int prompt_len = strlen(prompt);

    echo();
    mvprintw(LINES - 1, 0, "%s", prompt);
    clrtoeol();
    refresh();

    while (idx < (int)sizeof(input) - 1) {
        int ch = getch();
        if (ch == 27) { // ESC
            noecho();
            status_msg[0] = '\0';
            return;
        }
        else if (ch == 10 || ch == 13) { // Enter
            break;
        }
        else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (idx > 0) {
                idx--;
                input[idx] = '\0';
                mvprintw(LINES - 1, prompt_len + idx, " ");
                move(LINES - 1, prompt_len + idx);
                refresh();
            }
        }
        else if (ch >= 32 && ch <= 126) {
            input[idx++] = (char)ch;
            input[idx] = '\0';
        }
    }
    noecho();

    if (strlen(input) == 0) return;

    // Save a clean copy of the user's raw input string before strtok cuts it up
    char saved_input_copy[256];
    strncpy(saved_input_copy, input, sizeof(saved_input_copy) - 1);
    saved_input_copy[sizeof(saved_input_copy) - 1] = '\0';

    char to_delete[MAX_LINES];
    memset(to_delete, 0, sizeof(to_delete));

    save_undo_state();

    char *token = strtok(input, ",");
    while (token != NULL) {
        while (*token == ' ') token++;

        char *dash = strchr(token, '-');
        if (dash != NULL) {
            int start = atoi(token);
            int end = atoi(dash + 1);

            if (start > 0 && end >= start) {
                for (int i = start; i <= end; i++) {
                    if (i <= line_count) {
                        to_delete[i - 1] = 1;
                    }
                }
            }
        } else {
            int line_num = atoi(token);
            if (line_num > 0 && line_num <= line_count) {
                to_delete[line_num - 1] = 1;
            }
        }
        token = strtok(NULL, ",");
    }

    int deleted_count = 0;
    for (int i = line_count - 1; i >= 0; i--) {
        if (to_delete[i]) {
            for (int j = i; j < line_count - 1; j++) {
                strcpy(buffer[j], buffer[j + 1]); 
            }
            buffer[line_count - 1][0] = '\0'; 
            line_count--; 
            deleted_count++;

            if (i <= current_line && current_line > 0) {
                current_line--;
            }
        }
    }

    if (line_count == 0) {
        line_count = 1;
        buffer[0][0] = '\0';
        current_line = 0;
        cursor_x = 0;
    }

    int len = strlen(buffer[current_line]); 
    if (cursor_x > len) cursor_x = len; 

    int max_displayable_lines = LINES - 4;
    if (scroll_y > line_count - max_displayable_lines) {
        scroll_y = line_count - max_displayable_lines;
    }
    if (scroll_y < 0) scroll_y = 0;

    if (deleted_count > 0) {
        snprintf(status_msg, sizeof(status_msg), "Deleted lines %s", saved_input_copy);
    } else {
        snprintf(status_msg, sizeof(status_msg), "No lines deleted.");
    }
} // End delete_lines_interactive

void show_help_window() {
    // 1. Calculate dimensions and centering
    int height = 16;
    int width = 50;
    int start_y = (LINES - height) / 2;
    int start_x = (COLS - width) / 2;

    // 2. Create the window
    WINDOW *help_win = newwin(height, width, start_y, start_x);
    
    // Allow the window to catch all keypad inputs seamlessly
    keypad(help_win, TRUE);

    // 3. Draw the thin ANSI/NCURSES box borders
    // Arguments: win, vline, hline
    box(help_win, 0, 0); 

    // 4. Render the command list content
    const char *header = "--- qi text editor help ---";
    int string_length = 27;
    int header_x = (width - string_length) / 2;

    mvwprintw(help_win, 1, header_x, "%s", header);
    mvwprintw(help_win, 3, 4, "^O  Open File");
    mvwprintw(help_win, 4, 4, "^W  Save File");
    mvwprintw(help_win, 5, 4, "^F  Find Text");
    mvwprintw(help_win, 6, 4, "^R  Replace Text");
    mvwprintw(help_win, 7, 4, "^G  Go To Line");
    mvwprintw(help_win, 8, 4, "^D  Delete Line(s)");
    mvwprintw(help_win, 9, 4, "^U  Undo Action");
    mvwprintw(help_win, 10, 4, "^T Jump to Start of File");
    mvwprintw(help_win, 11, 4, "^B Jump to End of File");
    mvwprintw(help_win, 12, 4, "^Q  Quit Editor");
    
    mvwprintw(help_win, 14, (width - 24) / 2, "Press any key to close");

    // 5. Refresh to show the popup overlay
    wrefresh(help_win);

    // 6. Wait for a keypress to dismiss
    wgetch(help_win);

    // 7. Clean up memory and remove the window
    delwin(help_win);

    // 8. Force standard screen to redraw over the cleared popup area
    touchwin(stdscr);
    refresh();
} // End show_help_window

int main(int argc, char *argv[]) {

    // 1. First, configure low-level TTY flow control
    struct termios tty;
    if (tcgetattr(STDIN_FILENO, &tty) == 0) {
        tty.c_iflag &= ~IXON;
        tcsetattr(STDIN_FILENO, TCSANOW, &tty);
    }

    // 2. Initialize ncurses completely
    initscr();
    set_escdelay(25);
    raw();
    keypad(stdscr, TRUE);
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    noecho();
    curs_set(1);

    // Start tracking
    tracker_init(MAX_LINES, MAX_LINE_LEN);

    // Check if a filename argument was provided
    if (argc > 1) {
        load_file(argv[1]);
    } else {
        // Default name if no argument is passed
        strcpy(current_filename, "untitled.txt");
        line_count = 1;
        buffer[0][0] = '\0';
    }

    while (1) {
        draw_screen();
        int ch = getch();

        status_msg[0] = '\0';

        if (ch == CTRL_KEY('q')) {
            if (is_modified) {
                mvprintw(LINES - 1, 0, "");
                clrtoeol();

                attron(COLOR_PAIR(2)); // --- TURN ON RED ---
                printw("Unsaved changes! Quit anyway? (y/n): ");
                attroff(COLOR_PAIR(2)); // --- TURN OFF RED ---                

                refresh();
                int confirm = getch();
                if (confirm == 'y' || confirm == 'Y') {
                    break; 
                } else {
                    continue; // Skip the rest of the loop and redraw the screen safely
                }
            } else {
                break; // No changes, exit immediately
            }
        }
        else if (ch == CTRL_KEY('o')) interactive_open();
        else if (ch == CTRL_KEY('w')) save_file();
        else if (ch == CTRL_KEY('f')) find_text();
        else if (ch == CTRL_KEY('?')) show_help_window();
        else if (ch == CTRL_KEY('r')) replace_text();
        else if (ch == CTRL_KEY('g')) goto_line();
        else if (ch == CTRL_KEY('u')) undo();
        else if (ch == CTRL_KEY('d')) delete_lines_interactive();
        else if (ch == CTRL_KEY('t')) { // Top of file
            current_line = 0;
            cursor_x = 0;
            scroll_y = 0;
        }
        else if (ch == CTRL_KEY('b')) { // Bottom of file
            current_line = line_count - 1;
            cursor_x = 0;
            int max_displayable_lines = LINES - 4;
            scroll_y = current_line - max_displayable_lines + 1;
            if (scroll_y < 0) scroll_y = 0;
        }
        else if (ch == KEY_UP) {
            if (current_line > 0) {
                current_line--;
                
                // If the cursor moves above the top visible line, slide the camera up
                if (current_line < scroll_y) {
                    scroll_y = current_line;
                }

                int len = strlen(buffer[current_line]);
                if (cursor_x > len) cursor_x = len;
            }
      } else if (ch == KEY_DOWN) {
            if (current_line < line_count - 1) {
                current_line++;
                
                // Track the max available vertical rows for text
                int max_displayable_lines = LINES - 4;
                
                // If the active line pushes past the bottom boundary, slide the camera down
                if (current_line >= scroll_y + max_displayable_lines) {
                    scroll_y++;
                }

                int len = strlen(buffer[current_line]);
                if (cursor_x > len) cursor_x = len;
            }
        } else if (ch == KEY_LEFT) {
            if (cursor_x > 0) cursor_x--;
        } else if (ch == KEY_RIGHT) {
            if (cursor_x < (int)strlen(buffer[current_line])) cursor_x++;
        } else if (ch == 393 || ch == 545 || ch == 260 || ch == 543) { 
            // --- BACKUP / macOS WORD HOP LEFT (Option + Left Arrow) ---
            if (cursor_x > 0) {
                while (cursor_x > 0 && isspace((unsigned char)buffer[current_line][cursor_x - 1])) cursor_x--;
                while (cursor_x > 0 && !isspace((unsigned char)buffer[current_line][cursor_x - 1])) cursor_x--;
            }
            continue;
        } else if (ch == 402 || ch == 560 || ch == 261 || ch == 544) { 
            // --- BACKUP / macOS WORD HOP RIGHT (Option + Right Arrow) ---
            int len = strlen(buffer[current_line]);
            if (cursor_x < len) {
                while (cursor_x < len && !isspace((unsigned char)buffer[current_line][cursor_x])) cursor_x++;
                while (cursor_x < len && isspace((unsigned char)buffer[current_line][cursor_x])) cursor_x++;
            }
            continue;
        } else if (ch == KEY_PPAGE) {
            // --- PAGE UP ---
            int max_displayable_lines = LINES - 4;
            
            // Move cursor up by a full page
            current_line -= max_displayable_lines;
            if (current_line < 0) current_line = 0;
            
            // Move the viewport camera up by the same distance
            scroll_y -= max_displayable_lines;
            if (scroll_y < 0) scroll_y = 0;
            
            // Adjust cursor column safety on the new line
            int len = strlen(buffer[current_line]);
            if (cursor_x > len) cursor_x = len;

        } else if (ch == KEY_NPAGE) {
            // --- PAGE DOWN ---
            int max_displayable_lines = LINES - 4;
            
            // Move cursor down by a full page
            current_line += max_displayable_lines;
            if (current_line >= line_count) current_line = line_count - 1;
            
            // Move the viewport camera down, keeping it in bounds
            scroll_y += max_displayable_lines;
            if (scroll_y > line_count - max_displayable_lines) {
                scroll_y = line_count - max_displayable_lines;
            }
            if (scroll_y < 0) scroll_y = 0;
            
            // Adjust cursor column safety on the new line
            int len = strlen(buffer[current_line]);
            if (cursor_x > len) cursor_x = len;

        } else if (ch == KEY_HOME || ch == 1 || ch == 27) {
            // Handle Home, Ctrl+A (\001), or manual escape parsing
            if (ch == KEY_HOME || ch == 1) {
                cursor_x = 0;
            } else { // It's an Escape byte (27)
                int next1 = getch();
                
                // --- macOS OPTION + ARROW SHORTCUTS ---
                if (next1 == 'b') { 
                    // Option + Left Arrow (Hop Word Left)
                    if (cursor_x > 0) {
                        while (cursor_x > 0 && isspace((unsigned char)buffer[current_line][cursor_x - 1])) cursor_x--;
                        while (cursor_x > 0 && !isspace((unsigned char)buffer[current_line][cursor_x - 1])) cursor_x--;
                    }
                    continue;
                } 
                else if (next1 == 'f') { 
                    // Option + Right Arrow (Hop Word Right)
                    int len = strlen(buffer[current_line]);
                    if (cursor_x < len) {
                        while (cursor_x < len && !isspace((unsigned char)buffer[current_line][cursor_x])) cursor_x++;
                        while (cursor_x < len && isspace((unsigned char)buffer[current_line][cursor_x])) cursor_x++;
                    }
                    continue;
                }
                
                // --- EXISTING EXTENDED KEY ESCAPES ---
                int next2 = getch();
                // Check if it's the sequence for Home: \033[1~ or \033[H
                if (next1 == '[' && (next2 == '1' || next2 == 'H')) {
                    if (next2 == '1') getch(); // Swallow the trailing '~'
                    cursor_x = 0;
                } 
                // Check if it's the sequence for End: \033[4~ or \033[F
                else if (next1 == '[' && (next2 == '4' || next2 == 'F')) {
                    if (next2 == '4') getch(); // Swallow the trailing '~'
                    cursor_x = (int)strlen(buffer[current_line]);
                } else {
                    ungetch(next2);
                    ungetch(next1);
                }
            }
            
        } else if (ch == KEY_END || ch == 5) {
            // Handle End or Ctrl+E (\005)
            cursor_x = (int)strlen(buffer[current_line]);         

        } else if (ch == 9) {
            // Intercept Tab (ASCII 9)
            int len = strlen(buffer[current_line]);
            
            // Check if we are working on a Makefile to determine tab style
            int is_makefile = (strstr(current_filename, "Makefile") != NULL);
            int tab_size = is_makefile ? 1 : 4;
            
            if (len + tab_size < MAX_LINE_LEN) {
                save_undo_state();
                
                // Shift text to the right to make room
                memmove(&buffer[current_line][cursor_x + tab_size], 
                        &buffer[current_line][cursor_x], 
                        len - cursor_x + 1);
                
                if (is_makefile) {
                    // Insert a true hardware tab character
                    buffer[current_line][cursor_x] = '\t';
                    tracker_set_modified(current_line, cursor_x, 1);
                } else {
                    // Fill the slot with 4 space characters
                    for (int i = 0; i < tab_size; i++) {
                        buffer[current_line][cursor_x + i] = ' ';
                        tracker_set_modified(current_line, cursor_x + i, 1);
                    }
                }
                
                cursor_x += tab_size; // Advance the cursor forward
            }

        } else if (ch == 10 || ch == 13) {
            // Handle Enter Key
            if (line_count < MAX_LINES) {
                save_undo_state();
                for (int i = line_count; i > current_line + 1; i--) {
                    strcpy(buffer[i], buffer[i - 1]);
                }
                strcpy(buffer[current_line + 1], &buffer[current_line][cursor_x]);
                buffer[current_line][cursor_x] = '\0';
                current_line++;
                cursor_x = 0;
                line_count++;

                // --- NEW: Slide viewport camera up if cursor pushes past bottom edge ---
                int max_displayable_lines = LINES - 4;
                if (current_line >= scroll_y + max_displayable_lines) {
                    scroll_y++;
                }
            }
        } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            // Handle Backspace
            if (cursor_x > 0) {
                save_undo_state();
                int len = strlen(buffer[current_line]);
                memmove(&buffer[current_line][cursor_x - 1], &buffer[current_line][cursor_x], len - cursor_x + 1);
                cursor_x--;
            } else if (current_line > 0) {
                save_undo_state();
                int target_line = current_line - 1;
                int target_len = strlen(buffer[target_line]);
                
                if (target_len + strlen(buffer[current_line]) < MAX_LINE_LEN) {
                    strcat(buffer[target_line], buffer[current_line]);
                    for (int i = current_line; i < line_count - 1; i++) {
                        strcpy(buffer[i], buffer[i + 1]);
                    }
                    buffer[line_count - 1][0] = '\0';
                    current_line--;
                    cursor_x = target_len;
                    line_count--;
                    if (current_line < scroll_y) {
                        scroll_y = current_line;
                    }
                }
            }

        } else if (ch == KEY_DC || ch == 330) {
            // Handle Forward Delete (Delete key)
            int len = strlen(buffer[current_line]);
            
            // Case 1: Cursor is within the text, delete the character directly under it
            if (cursor_x < len) {
                save_undo_state();
                // Shift everything after the cursor left by 1 position
                memmove(&buffer[current_line][cursor_x], &buffer[current_line][cursor_x + 1], len - cursor_x);
            } 
            // Case 2: Cursor is at the absolute end of the line, merge the NEXT line up
            else if (current_line < line_count - 1) {
                int next_line = current_line + 1;
                int next_len = strlen(buffer[next_line]);
                
                // Ensure merging won't overflow the maximum allowable line buffer width
                if (len + next_len < MAX_LINE_LEN) {
                    save_undo_state();
                    
                    // Append the contents of the next line directly onto this one
                    strcat(buffer[current_line], buffer[next_line]);
                    
                    // Shift all subsequent lines up by 1 slot to fill the gap
                    for (int i = next_line; i < line_count - 1; i++) {
                        strcpy(buffer[i], buffer[i + 1]);
                    }
                    buffer[line_count - 1][0] = '\0'; // Clear the trailing duplicated row
                    line_count--;
                }
            }

        } else if (ch >= 32 && ch <= 126) {
            // Handle typing a character
            int len = strlen(buffer[current_line]);
            if (cursor_x <= len) {

                if (cursor_x == 0 || buffer[current_line][cursor_x - 1] == ' ') {
                    save_undo_state();
                }

                memmove(&buffer[current_line][cursor_x + 1], &buffer[current_line][cursor_x], len - cursor_x + 1);
                buffer[current_line][cursor_x] = (char)ch;
                tracker_set_modified(current_line, cursor_x, 1);
                cursor_x++;
            }
        }
    } // <--- Closes the while(1) loop

    endwin();
    return 0;
} // <--- Closes main()
