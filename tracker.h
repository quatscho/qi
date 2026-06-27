#ifndef TRACKER_H
#define TRACKER_H

// Initializes or resizes memory for tracking
void tracker_init(int max_lines, int max_line_len);

// Marks a specific character slot as modified
void tracker_set_modified(int line, int col, int is_mod);

// Checks if a specific character slot is modified (returns 1 or 0)
int tracker_is_modified(int line, int col);

// Resets all changes (call this on Save/Load)
void tracker_clear(void);

// Frees all allocated memory on exit
void tracker_free(void);

#endif
