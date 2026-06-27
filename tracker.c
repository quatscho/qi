#include <stdlib.h>
#include <string.h>
#include "tracker.h"

static char *change_grid = NULL;
static int total_lines = 0;
static int line_length = 0;

void tracker_init(int max_lines, int max_line_len) {
    total_lines = max_lines;
    line_length = max_line_len;
    change_grid = calloc(total_lines * line_length, sizeof(char));
}

void tracker_set_modified(int line, int col, int is_mod) {
    if (change_grid && line < total_lines && col < line_length) {
        change_grid[(line * line_length) + col] = is_mod;
    }
}

int tracker_is_modified(int line, int col) {
    if (change_grid && line < total_lines && col < line_length) {
        return change_grid[(line * line_length) + col];
    }
    return 0;
}

void tracker_clear(void) {
    if (change_grid) {
        memset(change_grid, 0, total_lines * line_length * sizeof(char));
    }
}

void tracker_free(void) {
    free(change_grid);
    change_grid = NULL;
}
