#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ncurses.h>
#include <termios.h> 
#include <unistd.h> 

#define MAX_LINES 1024
#define MAX_LINE_LEN 512
#define CTRL_KEY(k) ((k) & 0x1f)
#define MAX_UNDO 50

typedef struct {
    char buffer[MAX_LINES][MAX_LINE_LEN];
    int line_count;
    int current_line;
    int cursor_x;
    int scroll_y;
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
}

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

    undo_stack_top--; // Pop it off
    snprintf(status_msg, sizeof(status_msg), "Undo!");
}

// Function to handle the "Open" command
// 1. Silent file loader used by both main() and the menu
void load_file(const char *filename) {
    undo_stack_top = -1; // --- ADD THIS TO CLEAR UNDO ON LOAD ---
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
    } else {
        // If file doesn't exist, treat it as a new file under that name
        strncpy(current_filename, filename, sizeof(current_filename) - 1);
        line_count = 1;
        buffer[0][0] = '\0';
    }
}


// 2. Interactive menu command when you press Ctrl+O
void interactive_open() {
    char filename[256];
    echo();
    
    // Using raw() now instead of cbreak(), so we toggle raw() modes
    noraw(); 

    mvprintw(LINES - 1, 0, "Enter filename to open: ");
    clrtoeol(); 
    refresh();  

    getstr(filename);

    noecho();
    raw(); 

    // Only load if the user actually typed a name
    if (strlen(filename) > 0) {
        FILE *fp = fopen(filename, "r");
        if (fp) {
            fclose(fp);
            load_file(filename); // Pass it to our silent loader
        } else {
            mvprintw(LINES - 1, 0, "File not found! Press any key...");
            clrtoeol();
            refresh();
            getch();
        }
    }
}

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

        snprintf(status_msg, sizeof(status_msg), "Saved successfully to '%s'!", current_filename);
    } else {
        mvprintw(LINES - 1, 0, "Error: Could not save file! Press any key...");
        clrtoeol();
        refresh();
        getch(); // Keep this one blocking because it's a critical error
    }
}

void draw_screen() {
    clear();
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK); 

    // 1. Draw the top header area (Row 0)
    move(0, 0);
    clrtoeol();
    // Highlight the file name with our attention color pair
    attron(COLOR_PAIR(1));
    printw(" File: %s", current_filename);
    attroff(COLOR_PAIR(1));

    // 2. Draw the thin top horizontal separator line (Row 1)
    move(1, 0);
    clrtoeol();
    for (int x = 0; x < COLS; x++) {
        mvaddch(1, x, ACS_HLINE);
    }

    // FIX: Reduce by 4 now (2 rows for header/top bar, 2 rows for bottom bar/commands)
    int max_displayable_lines = LINES - 4; 

    // Render the file content lines
    for (int i = 0; i < max_displayable_lines; i++) {
        int file_line_index = scroll_y + i;
        
        // Stop drawing if we reach the end of the file contents
        if (file_line_index >= line_count) break;

        // Calculate screen row index (offset by +2 to stay below the top header bar)
        int screen_row = i + 2;

        // Draw the line number
        if (file_line_index == current_line) {
            attron(COLOR_PAIR(1));
            mvprintw(screen_row, 0, "%3d ", file_line_index + 1); 
            attroff(COLOR_PAIR(1));
        } else {
            mvprintw(screen_row, 0, "%3d ", file_line_index + 1);
        }

        // Draw the vertical separator
        mvaddch(screen_row, 4, ACS_VLINE);

        // Draw the text string
        mvprintw(screen_row, 6, "%s", buffer[file_line_index]);
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
        // Force the status message to clip precisely at terminal width (COLS - 1)
        mvprintw(LINES - 1, 0, "%.*s", COLS - 1, status_msg);
        attroff(COLOR_PAIR(1));
    } else {
        // Compact menu bar to prevent line wrapping completely
        mvprintw(LINES - 1, 0, "^O Open | ^W Save | ^F Find | ^G GoTo | ^D DelLines | ^U Undo | ^Q Quit");
    }
    
    // Adjust physical cursor position to match the shifted text view (+2 offset)
    move((current_line - scroll_y) + 2, cursor_x + 6); 

    refresh();
}
        
void find_text() {
    char search_query[256];
    echo();
    noraw(); // Temporarily disable raw mode for clean typing/Enter handling

    mvprintw(LINES - 1, 0, "Find: ");
    clrtoeol();
    refresh();

    getstr(search_query);

    noecho();
    raw(); // Re-enable raw mode

    if (strlen(search_query) == 0) return;

    // Search sequentially starting from the top of the file
    for (int i = 0; i < line_count; i++) {
        char *match = strstr(buffer[i], search_query);
        if (match != NULL) {
            current_line = i;
            cursor_x = match - buffer[i]; // Calculate the column offset index

            // Adjust the viewport camera so the match is visibly centered/on-screen
            int max_displayable_lines = LINES - 4;
            if (current_line < scroll_y || current_line >= scroll_y + max_displayable_lines) {
                scroll_y = current_line - (max_displayable_lines / 2);
                if (scroll_y < 0) scroll_y = 0;
            }
            return; // Found a match, exit early
        }
    }

    // If it falls through the loop, no matches were found
    mvprintw(LINES - 1, 0, "Pattern not found! Press any key...");
    clrtoeol();
    refresh();
    getch();
}

void goto_line() {
    char line_input[32];
    echo();
    noraw(); // Temporarily disable raw mode for clean typing/Enter handling

    mvprintw(LINES - 1, 0, "Go to line: ");
    clrtoeol();
    refresh();

    getstr(line_input);

    noecho();
    raw(); // Re-enable raw mode

    if (strlen(line_input) == 0) return;

    // Convert the string input into an integer index (1-based from user input)
    int target = atoi(line_input);

    // Bound check: Ensure the target line exists within the document
    if (target < 1 || target > line_count) {
        snprintf(status_msg, sizeof(status_msg), "Line %d out of bounds! (Total lines: %d)", target, line_count);
        return;
    }

    // Convert to 0-based index for our internal buffer array
    current_line = target - 1;
    cursor_x = 0; // Snap cursor to the beginning of the targeted line

    // Center the viewport camera vertically on the target line
    int max_displayable_lines = LINES - 4;
    scroll_y = current_line - (max_displayable_lines / 2);
    
    // Safety boundaries for the camera scroll
    if (scroll_y < 0) scroll_y = 0;
    if (scroll_y > line_count - max_displayable_lines) {
        scroll_y = line_count - max_displayable_lines;
        if (scroll_y < 0) scroll_y = 0;
    }
}

void delete_lines_interactive() {
    char input[256];
    echo();
    noraw(); // Temporarily disable raw mode for clean typing[span_3](start_span)[span_3](end_span)

    mvprintw(LINES - 1, 0, "Delete lines (e.g., 3, 5, 10-25): ");
    clrtoeol();
    refresh();

    getstr(input);

    noecho();
    raw(); // Re-enable raw mode[span_4](start_span)[span_4](end_span)

    if (strlen(input) == 0) return;

    // Create a boolean-like array to flag lines marked for deletion (0-indexed)
    // Initialize everything to 0 (don't delete)
    char to_delete[MAX_LINES];
    memset(to_delete, 0, sizeof(to_delete));

    // Save a single undo checkpoint before we make any changes[span_5](start_span)[span_5](end_span)
    save_undo_state();

    // Parse the input string
    char *token = strtok(input, ",");
    while (token != NULL) {
        // Trim leading spaces if any
        while (*token == ' ') token++;

        // Check if this token defines a range (e.g., "10-25")
        char *dash = strchr(token, '-');
        if (dash != NULL) {
            int start = atoi(token);
            int end = atoi(dash + 1);

            // Bound check and flag the entire range
            if (start > 0 && end >= start) {
                for (int i = start; i <= end; i++) {
                    if (i <= line_count) {
                        to_delete[i - 1] = 1; // Convert to 0-indexed
                    }
                }
            }
        } else {
            // It's a single line number
            int line_num = atoi(token);
            if (line_num > 0 && line_num <= line_count) {
                to_delete[line_num - 1] = 1; // Convert to 0-indexed
            }
        }
        token = strtok(NULL, ",");
    }

    // Process deletions from the BOTTOM UP to preserve indices[span_6](start_span)[span_6](end_span)
    int deleted_count = 0;
    for (int i = line_count - 1; i >= 0; i--) {
        if (to_delete[i]) {
            // Shift all subsequent lines down[span_7](start_span)[span_7](end_span)
            for (int j = i; j < line_count - 1; j++) {
                strcpy(buffer[j], buffer[j + 1]); //[span_8](start_span)[span_8](end_span)
            }
            buffer[line_count - 1][0] = '\0'; // Clear the trailing duplicated line[span_9](start_span)[span_9](end_span)
            line_count--; //[span_10](start_span)[span_10](end_span)
            deleted_count++;

            // If we deleted the current line or a line above it, adjust cursor tracking
            if (i <= current_line && current_line > 0) {
                current_line--;
            }
        }
    }

    // Edge case: If we deleted everything, leave one blank line
    if (line_count == 0) {
        line_count = 1;
        buffer[0][0] = '\0';
        current_line = 0;
        cursor_x = 0;
    }

    // Ensure cursor column safety on the current line[span_11](start_span)[span_11](end_span)
    int len = strlen(buffer[current_line]); //[span_12](start_span)[span_12](end_span)
    if (cursor_x > len) cursor_x = len; //[span_13](start_span)[span_13](end_span)

    // Ensure the viewport camera isn't stranded past the end of the file
    int max_displayable_lines = LINES - 4;
    if (scroll_y > line_count - max_displayable_lines) {
        scroll_y = line_count - max_displayable_lines;
    }
    if (scroll_y < 0) scroll_y = 0;

    snprintf(status_msg, sizeof(status_msg), "Deleted %d line(s).", deleted_count); //[span_14](start_span)[span_14](end_span)
}

int main(int argc, char *argv[]) {

    // 1. First, configure low-level TTY flow control
    struct termios tty;
    if (tcgetattr(STDIN_FILENO, &tty) == 0) {
        tty.c_iflag &= ~IXON;
        tcsetattr(STDIN_FILENO, TCSANOW, &tty);
    }

    // 2. Initialize ncurses completely
    initscr();
    raw();
    keypad(stdscr, TRUE);
    start_color();
    init_pair(1, COLOR_YELLOW, COLOR_BLACK);
    noecho();
    curs_set(1);

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

        if (ch == CTRL_KEY('q')) break;
        else if (ch == CTRL_KEY('o')) interactive_open();
        else if (ch == CTRL_KEY('w')) save_file();
        else if (ch == CTRL_KEY('f')) find_text();
        else if (ch == CTRL_KEY('g')) goto_line();
        else if (ch == CTRL_KEY('u')) undo();
        else if (ch == CTRL_KEY('d')) delete_lines_interactive();
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
        } else if (ch == 9) {
            // Intercept Tab and insert 4 regular spaces
            int tab_size = 4;
            int len = strlen(buffer[current_line]);
            
            // Safety check: make sure adding 4 spaces won't overflow the maximum line limit
            if (len + tab_size < MAX_LINE_LEN) {
                save_undo_state();
                // Shift everything to the right by 4 spaces
                memmove(&buffer[current_line][cursor_x + tab_size], &buffer[current_line][cursor_x], len - cursor_x + 1);
                
                // Fill the new empty slot with 4 space characters
                for (int i = 0; i < tab_size; i++) {
                    buffer[current_line][cursor_x + i] = ' ';
                }
                
                cursor_x += tab_size; // Advance the cursor forward 4 spaces
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
        } else if (ch >= 32 && ch <= 126) {
            // Handle typing a character
            int len = strlen(buffer[current_line]);
            if (cursor_x <= len) {

                if (cursor_x == 0 || buffer[current_line][cursor_x - 1] == ' ') {
                    save_undo_state();
                }

                memmove(&buffer[current_line][cursor_x + 1], &buffer[current_line][cursor_x], len - cursor_x + 1);
                buffer[current_line][cursor_x] = (char)ch;
                cursor_x++;
            }
        }
    } // <--- Closes the while(1) loop

    endwin();
    return 0;
} // <--- Closes main()
