#define _XOPEN_SOURCE 700
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include "trump.h"

static const char *trump_items[] = {
    "Trump is mentioned over 38,000 times in the Epstein files",
    "More than 25 women have accused him of raping them or otherwise committing some sort of sexual misconduct against them",
    "He has made comments that he would have sex with his own daughter",
    "He told Howard Stern that 12 is his lower limit",
    "He has bragged about sexually assaulting women (\"Grab them by the pussy\")",
    "He was convicted of 34 felony counts of Falsifying Business Records",
    "In his first term, Trump made 30,573 false or misleading statements",
    "Trump is the only President to have been impeached twice",
    "He attempted to overturn an election by sending a mob of insurrectionists to the capitol",
    "He did nothing for three hours while sitting Congress Persons and Senators -AND THE VICE-PRESIDENT- sheltered for their lives",
    "Under his leadership, nearly half a million Americans died from COVID-19 before the vaccine started bringing it under control",
    "He has destroyed the White House, the Reflecting Pool, the Ellipse, and unilaterally renamed a monument to President Kennedy after himself",
    "He started a war against Iran that he almost immediately began losing",
    "He is severely cognitively impaired",
    "He has destroyed all norms that kept things relatively civil in Washington",
    "He regularly attacks the press",
    "He is blatantly racist",
    "Families that he forcibly separated in his first term are still struggling to reunite",
    "He cozies up to dictators and openly admires them",
    "He has irreparably damaged our relationships with our allies to the point they no longer trust the US",
    "He made America's 250th Anniversary celebration about himself",
    "He mocks the disabled",
    "He suggested that his disabled great-nephew should be allowed to die",
    "He has taken action to allow the institutionalization of disabled people",
    "He has demonized trans people to the point that they are being murdered at a higher rate than in previous years",
    NULL
};

/* Word-wrap a single string into lines of at most `width` chars.
 * Continuation lines are indented by 2 spaces.
 * Appends wrapped lines into `out[]`; returns new count. */
static int wrap_item(const char *text, int width, char **out, int count) {
    int len = (int)strlen(text);
    int pos = 0;
    int first = 1;
    while (pos < len) {
        int indent = first ? 0 : 2;
        int avail = width - indent;
        if (avail <= 0) avail = 1;
        int remaining = len - pos;
        if (remaining <= avail) {
            char *buf = malloc(indent + remaining + 1);
            if (!buf) break;
            memset(buf, ' ', indent);
            memcpy(buf + indent, text + pos, remaining);
            buf[indent + remaining] = '\0';
            out[count++] = buf;
            break;
        }
        /* find last space within avail */
        int cut = avail;
        while (cut > 0 && text[pos + cut] != ' ') cut--;
        if (cut == 0) cut = avail; /* no space found, hard break */
        char *buf = malloc(indent + cut + 1);
        if (!buf) break;
        memset(buf, ' ', indent);
        memcpy(buf + indent, text + pos, cut);
        buf[indent + cut] = '\0';
        out[count++] = buf;
        pos += cut;
        while (pos < len && text[pos] == ' ') pos++;
        first = 0;
    }
    return count;
}

void show_trump_window(void) {
    int win_w = 62;
    int win_h = 22;
    if (win_h > LINES - 2) win_h = LINES - 2;
    if (win_w > COLS  - 2) win_w = COLS  - 2;

    int text_w = win_w - 4;
    int inner_h = win_h - 5;

    /* Pre-wrap all items; allocate generously */
    char **dlines = malloc(512 * sizeof(char *));
    if (!dlines) return;
    int total = 0;
    for (int i = 0; trump_items[i]; i++)
        total = wrap_item(trump_items[i], text_w, dlines, total);

    int start_y = (LINES - win_h) / 2;
    int start_x = (COLS  - win_w) / 2;

    WINDOW *tw = newwin(win_h, win_w, start_y, start_x);
    keypad(tw, TRUE);

    int scroll = 0;
    int at_bottom = 0;

    for (;;) {
        werase(tw);
        box(tw, 0, 0);

        /* Title bump */
        {
            const char *title = " Reasons Trump is an awful person ";
            int tlen = (int)strlen(title);
            int tx = (win_w - tlen) / 2;
            if (tx < 1) tx = 1;
            mvwaddch(tw, 0, tx - 1,    ACS_RTEE);
            mvwaddch(tw, 0, tx + tlen, ACS_LTEE);
            wattron(tw, A_BOLD);
            mvwprintw(tw, 0, tx, "%s", title);
            wattroff(tw, A_BOLD);
        }

        /* Render visible wrapped lines */
        for (int i = 0; i < inner_h; i++) {
            int idx = scroll + i;
            if (idx >= total) break;
            mvwprintw(tw, 1 + i, 2, "%-*.*s", text_w, text_w, dlines[idx]);
        }

        /* Scroll indicators */
        if (scroll > 0)
            mvwprintw(tw, 1, win_w - 4, " ^ ");

        at_bottom = (scroll + inner_h >= total);

        if (!at_bottom)
            mvwprintw(tw, inner_h, win_w - 4, " v ");

        /* Footer separator */
        for (int x = 1; x < win_w - 1; x++)
            mvwaddch(tw, win_h - 4, x, ACS_HLINE);

        wattron(tw, A_DIM);
        mvwprintw(tw, win_h - 3, 2, "Arrow keys / PgUp / PgDn to scroll");
        wattroff(tw, A_DIM);

        if (at_bottom) {
            wattron(tw, COLOR_PAIR(1));
            mvwprintw(tw, win_h - 2, 2, "Press any key to close...");
            wattroff(tw, COLOR_PAIR(1));
        } else {
            wattron(tw, A_DIM);
            mvwprintw(tw, win_h - 2, 2, "Scroll to the bottom to close.");
            wattroff(tw, A_DIM);
        }

        wrefresh(tw);

        int ch = wgetch(tw);

        if (ch == KEY_UP || ch == 'k') {
            if (scroll > 0) scroll--;
        } else if (ch == KEY_DOWN || ch == 'j') {
            if (scroll + inner_h < total) scroll++;
        } else if (ch == KEY_PPAGE) {
            scroll -= inner_h;
            if (scroll < 0) scroll = 0;
        } else if (ch == KEY_NPAGE) {
            scroll += inner_h;
            if (scroll + inner_h > total) scroll = total - inner_h;
            if (scroll < 0) scroll = 0;
        } else {
            if (at_bottom) break;
        }
    }

    delwin(tw);
    touchwin(stdscr);
    refresh();

    for (int i = 0; i < total; i++) free(dlines[i]);
    free(dlines);
}
