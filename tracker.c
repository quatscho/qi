#include <stdlib.h>
#include <string.h>
#include "tracker.h"

static char *dirty = NULL;
static int dirty_cap = 0;

void tracker_init(int max_lines, int max_line_len) {
    (void)max_line_len;
    free(dirty);
    dirty_cap = max_lines > 0 ? max_lines : 1;
    dirty = calloc(dirty_cap, sizeof(char));
}

void tracker_set_modified(int line, int col, int is_mod) {
    (void)col;
    if (!dirty || line < 0) return;
    if (line >= dirty_cap) {
        int new_cap = dirty_cap * 2;
        while (new_cap <= line) new_cap *= 2;
        char *tmp = realloc(dirty, new_cap * sizeof(char));
        if (!tmp) return;
        memset(tmp + dirty_cap, 0, (new_cap - dirty_cap) * sizeof(char));
        dirty = tmp;
        dirty_cap = new_cap;
    }
    dirty[line] = (char)is_mod;
}

int tracker_is_modified(int line, int col) {
    (void)col;
    if (dirty && line >= 0 && line < dirty_cap)
        return dirty[line];
    return 0;
}

void tracker_clear(void) {
    if (dirty)
        memset(dirty, 0, dirty_cap * sizeof(char));
}

void tracker_free(void) {
    free(dirty);
    dirty = NULL;
    dirty_cap = 0;
}
