#ifndef SYNTAX_H
#define SYNTAX_H

/* Token types — map directly to ncurses colour pairs in qi.c */
typedef enum {
    TOK_NORMAL   = 0,
    TOK_KEYWORD  = 1,  /* colour pair 2 — red    */
    TOK_STRING   = 2,  /* colour pair 4 — magenta */
    TOK_COMMENT  = 3,  /* colour pair 5 — green   */
    TOK_PREPROC  = 4,  /* colour pair 3 — cyan    */
} TokenType;

/* A single highlighted span within a line.
 * start and end are byte offsets into the line string; end is exclusive. */
typedef struct {
    int       start;
    int       end;
    TokenType type;
} Span;

#define MAX_SPANS 256

/* Detect the language from a filename and store it internally.
 * Call once after loading or switching files. */
void syntax_set_file(const char *filename);

/* Scan all lines up-front to resolve multi-line block-comment state.
 * Call after load_file() and after any edit that could open/close a block comment.
 * lines     — the line pointer array
 * count     — number of lines */
void syntax_scan(char **lines, int count);

/* Fill spans[] with highlighted regions for line number line_idx.
 * Returns the number of spans written (0 = plain text). */
int syntax_spans(int line_idx, const char *line, Span *spans);

#endif
