#include "color.h"

void colors_init(void) {
    if (!has_colors()) return;

    start_color();
    use_default_colors(); /* Optional: uses terminal default background if supported */

    init_pair(PAIR_YELLOW,     COLOR_YELLOW,  COLOR_BLACK);
    init_pair(PAIR_RED,        COLOR_RED,     COLOR_BLACK);
    init_pair(PAIR_CYAN,       COLOR_CYAN,    COLOR_BLACK);
    init_pair(PAIR_MAGENTA,    COLOR_MAGENTA, COLOR_BLACK);
    init_pair(PAIR_GREEN,      COLOR_GREEN,   COLOR_BLACK);
    init_pair(PAIR_YELLOW_ALT, COLOR_YELLOW,  COLOR_BLACK);
    init_pair(PAIR_MATCH,      COLOR_BLACK,   COLOR_YELLOW);
    init_pair(PAIR_BLUE,       COLOR_BLUE,    COLOR_BLACK);
}
