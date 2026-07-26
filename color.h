#ifndef COLOR_H
#define COLOR_H

#include <ncurses.h>

/* Named Color Pairs */
typedef enum {
    PAIR_DEFAULT    = 0,
    PAIR_YELLOW     = 1, /* Status messages / File headers */
    PAIR_RED        = 2, /* Errors / Read-Only indicators */
    PAIR_CYAN       = 3, /* Syntax elements */
    PAIR_MAGENTA    = 4, /* Syntax elements */
    PAIR_GREEN      = 5, /* Active gutter line / RW Mode */
    PAIR_YELLOW_ALT = 6, /* Alternate accent */
    PAIR_MATCH      = 7, /* Bracket matching highlight */
    PAIR_BLUE       = 8, /* Auxiliary UI elements */
    PAIR_SELECT     = 9  /* Mouse double/triple-click selection highlight */
} QiColorPair;

/* Initialize ncurses colors and color pairs */
void colors_init(void);

#endif /* COLOR_H */
