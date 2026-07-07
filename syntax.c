#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "syntax.h"

/* ---------- language IDs ---------- */
typedef enum {
    LANG_NONE,
    LANG_C,
    LANG_PYTHON,
    LANG_SHELL,
    LANG_MAKEFILE,
    LANG_MARKDOWN,
    LANG_LUA,
    LANG_JAVASCRIPT,
    LANG_RUST,
    LANG_PHP,
} Language;

static Language current_lang = LANG_NONE;

/* ---------- per-line block-comment state ----------
 * in_block[i] == 1 means line i starts inside a block comment (C only). */
static char *in_block = NULL;
static int   in_block_cap = 0;

/* ---------- keyword tables ---------- */
static const char *kw_c[] = {
    "auto","break","case","char","const","continue","default","do","double",
    "else","enum","extern","float","for","goto","if","inline","int","long",
    "register","restrict","return","short","signed","sizeof","static","struct",
    "switch","typedef","union","unsigned","void","volatile","while",
    /* common C99/C11 additions */
    "_Bool","_Complex","_Generic","_Imaginary","_Noreturn","_Static_assert",
    "_Thread_local","NULL","true","false",
    NULL
};

static const char *kw_python[] = {
    "False","None","True","and","as","assert","async","await","break","class",
    "continue","def","del","elif","else","except","finally","for","from",
    "global","if","import","in","is","lambda","nonlocal","not","or","pass",
    "raise","return","try","while","with","yield",
    NULL
};

static const char *kw_shell[] = {
    "if","then","else","elif","fi","for","while","do","done","case","esac",
    "in","function","return","export","local","readonly","shift","unset",
    "true","false","exit","echo","source",
    NULL
};

static const char *kw_lua[] = {
    "and","break","do","else","elseif","end","false","for","function","goto",
    "if","in","local","nil","not","or","repeat","return","then","true",
    "until","while",
    NULL
};

static const char *kw_js[] = {
    "break","case","catch","class","const","continue","debugger","default",
    "delete","do","else","export","extends","false","finally","for","function",
    "if","import","in","instanceof","let","new","null","return","static",
    "super","switch","this","throw","true","try","typeof","undefined","var",
    "void","while","with","yield","async","await","of",
    NULL
};

static const char *kw_php[] = {
    "abstract","and","array","as","break","callable","case","catch","class",
    "clone","const","continue","declare","default","die","do","echo","else",
    "elseif","empty","enddeclare","endfor","endforeach","endif","endswitch",
    "endwhile","eval","exit","extends","final","finally","fn","for",
    "foreach","function","global","goto","if","implements","include",
    "include_once","instanceof","insteadof","interface","isset","list",
    "match","namespace","new","null","or","print","private","protected",
    "public","readonly","require","require_once","return","static","switch",
    "throw","trait","try","unset","use","var","while","xor","yield",
    "true","false","NULL","TRUE","FALSE",
    NULL
};

static const char *kw_rust[] = {
    "as","async","await","break","const","continue","crate","dyn","else",
    "enum","extern","false","fn","for","if","impl","in","let","loop","match",
    "mod","move","mut","pub","ref","return","self","Self","static","struct",
    "super","trait","true","type","union","unsafe","use","where","while",
    NULL
};

/* ---------- language detection ---------- */
void syntax_set_file(const char *filename) {
    current_lang = LANG_NONE;
    if (!filename) return;

    /* Check for Makefile by name first */
    const char *base = strrchr(filename, '/');
    base = base ? base + 1 : filename;
    if (strcmp(base, "Makefile") == 0 || strcmp(base, "makefile") == 0 ||
        strncmp(base, "Makefile.", 9) == 0) {
        current_lang = LANG_MAKEFILE; return;
    }

    const char *ext = strrchr(base, '.');
    if (!ext) return;
    ext++; /* skip the dot */

    if (strcmp(ext,"c")==0 || strcmp(ext,"h")==0 ||
        strcmp(ext,"cc")==0|| strcmp(ext,"cpp")==0||
        strcmp(ext,"cxx")==0|| strcmp(ext,"hpp")==0)
        current_lang = LANG_C;
    else if (strcmp(ext,"py")==0 || strcmp(ext,"pyw")==0)
        current_lang = LANG_PYTHON;
    else if (strcmp(ext,"sh")==0 || strcmp(ext,"bash")==0 ||
             strcmp(ext,"zsh")==0 || strcmp(ext,"ksh")==0)
        current_lang = LANG_SHELL;
    else if (strcmp(ext,"md")==0 || strcmp(ext,"markdown")==0)
        current_lang = LANG_MARKDOWN;
    else if (strcmp(ext,"lua")==0)
        current_lang = LANG_LUA;
    else if (strcmp(ext,"js")==0 || strcmp(ext,"mjs")==0 ||
             strcmp(ext,"ts")==0 || strcmp(ext,"tsx")==0 ||
             strcmp(ext,"jsx")==0)
        current_lang = LANG_JAVASCRIPT;
    else if (strcmp(ext,"rs")==0)
        current_lang = LANG_RUST;
    else if (strcmp(ext,"php")==0 || strcmp(ext,"php3")==0 ||
             strcmp(ext,"php4")==0 || strcmp(ext,"php5")==0 ||
             strcmp(ext,"phtml")==0)
        current_lang = LANG_PHP;
}

/* ---------- block-comment pre-scan (C only) ---------- */
void syntax_scan(char **lines, int count) {
    /* Grow state array if needed */
    if (count > in_block_cap) {
        free(in_block);
        in_block = calloc(count + 64, sizeof(char));
        in_block_cap = count + 64;
    } else if (in_block) {
        memset(in_block, 0, in_block_cap);
    }

    if ((current_lang != LANG_C && current_lang != LANG_PHP) || !in_block) return;

    int inside = 0;
    for (int i = 0; i < count; i++) {
        in_block[i] = (char)inside;
        const char *p = lines[i];
        int in_str = 0, in_chr = 0;
        while (*p) {
            if (!inside && !in_str && !in_chr && p[0]=='/' && p[1] && p[1]=='*') {
                inside = 1; p += 2; continue;
            }
            if (inside && p[0]=='*' && p[1] && p[1]=='/') {
                inside = 0; p += 2; continue;
            }
            if (!inside) {
                if (!in_chr && p[0]=='"') in_str = !in_str;
                if (!in_str && p[0]=='\'') in_chr = !in_chr;
                /* line comment ends the scan for this line */
                if (!in_str && !in_chr && p[0]=='/' && p[1] && p[1]=='/') break;
            }
            p++;
        }
    }
}

/* ---------- helper: try to match a keyword at position j ---------- */
static int match_keyword(const char *line, int j, int len,
                         const char **kws, int *kw_len_out) {
    for (int k = 0; kws[k]; k++) {
        int kl = (int)strlen(kws[k]);
        if (j + kl > len) continue;
        if (strncmp(line + j, kws[k], kl) != 0) continue;
        /* must not be preceded by an identifier character */
        if (j > 0 && (isalnum((unsigned char)line[j-1]) || line[j-1]=='_'))
            continue;
        /* must not be followed by an identifier character */
        char next = line[j + kl];
        if (isalnum((unsigned char)next) || next == '_') continue;
        *kw_len_out = kl;
        return 1;
    }
    return 0;
}

/* ---------- add a span (merge with previous if same type and adjacent) ---------- */
static int add_span(Span *spans, int n, int start, int end, TokenType type) {
    if (n > 0 && spans[n-1].type == type && spans[n-1].end == start) {
        spans[n-1].end = end;
        return n;
    }
    if (n >= MAX_SPANS) return n;
    spans[n].start = start;
    spans[n].end   = end;
    spans[n].type  = type;
    return n + 1;
}

/* ---------- main span generator ---------- */
int syntax_spans(int line_idx, const char *line, Span *spans) {
    if (current_lang == LANG_NONE) return 0;

    int len = (int)strlen(line);
    int n   = 0;

    /* --- Markdown: very simple heading / code fence detection --- */
    if (current_lang == LANG_MARKDOWN) {
        if (len > 0 && line[0] == '#')
            return add_span(spans, n, 0, len, TOK_PREPROC);
        if (len >= 3 && strncmp(line,"```",3)==0)
            return add_span(spans, n, 0, len, TOK_COMMENT);
        return 0;
    }

    /* --- Makefile --- */
    if (current_lang == LANG_MAKEFILE) {
        /* comment */
        for (int j = 0; j < len; j++) {
            if (line[j] == '#') {
                if (j > 0) n = add_span(spans, n, 0, j, TOK_NORMAL);
                return add_span(spans, n, j, len, TOK_COMMENT);
            }
        }
        /* target rule: first non-whitespace word ending in ':' */
        int j = 0;
        while (j < len && (line[j]==' '||line[j]=='\t')) j++;
        int start = j;
        while (j < len && line[j] != ':' && line[j] != ' ') j++;
        if (j < len && line[j] == ':' && j > start)
            n = add_span(spans, n, start, j+1, TOK_KEYWORD);
        return n;
    }

    /* --- Languages with single-char # line comments (Python, Shell) --- */
    if (current_lang == LANG_PYTHON || current_lang == LANG_SHELL) {
        const char **kws = (current_lang == LANG_PYTHON) ? kw_python : kw_shell;
        int in_str  = 0;
        char str_ch = 0;
        int j = 0;
        /* skip leading whitespace — emit as normal */
        while (j < len && (line[j]==' '||line[j]=='\t')) j++;

        for (; j < len; j++) {
            /* line comment */
            if (!in_str && line[j] == '#') {
                n = add_span(spans, n, j, len, TOK_COMMENT);
                return n;
            }
            /* string toggle */
            if (!in_str && (line[j]=='"' || line[j]=='\'')) {
                in_str = 1; str_ch = line[j];
                int start2 = j;
                j++;
                while (j < len) {
                    if (line[j]=='\\' && j+1<len) { j+=2; continue; }
                    if (line[j]==str_ch) { j++; break; }
                    j++;
                }
                n = add_span(spans, n, start2, j, TOK_STRING);
                in_str = 0; j--; continue;
            }
            /* keyword */
            int kl = 0;
            if (match_keyword(line, j, len, kws, &kl)) {
                n = add_span(spans, n, j, j+kl, TOK_KEYWORD);
                j += kl - 1;
            }
        }
        return n;
    }

    /* --- Lua --- */
    if (current_lang == LANG_LUA) {
        int j = 0;
        int in_str = 0; char str_ch = 0;
        for (; j < len; j++) {
            /* -- line comment */
            if (!in_str && line[j]=='-' && j+1<len && line[j+1]=='-') {
                n = add_span(spans, n, j, len, TOK_COMMENT);
                return n;
            }
            if (!in_str && (line[j]=='"'||line[j]=='\'')) {
                in_str=1; str_ch=line[j];
                int s=j; j++;
                while(j<len){if(line[j]=='\\'){j+=2;continue;}if(line[j]==str_ch){j++;break;}j++;}
                n=add_span(spans,n,s,j,TOK_STRING); in_str=0; j--; continue;
            }
            int kl=0;
            if(match_keyword(line,j,len,kw_lua,&kl)){
                n=add_span(spans,n,j,j+kl,TOK_KEYWORD); j+=kl-1;
            }
        }
        return n;
    }

    /* --- JavaScript / TypeScript --- */
    if (current_lang == LANG_JAVASCRIPT) {
        int j=0; int in_str=0; char str_ch=0;
        for(;j<len;j++){
            if(!in_str && line[j]=='/' && j+1<len && line[j+1]=='/'){
                n=add_span(spans,n,j,len,TOK_COMMENT); return n;
            }
            if(!in_str && (line[j]=='"'||line[j]=='\''||line[j]=='`')){
                in_str=1; str_ch=line[j]; int s=j; j++;
                while(j<len){if(line[j]=='\\'){j+=2;continue;}if(line[j]==str_ch){j++;break;}j++;}
                n=add_span(spans,n,s,j,TOK_STRING); in_str=0; j--; continue;
            }
            int kl=0;
            if(match_keyword(line,j,len,kw_js,&kl)){
                n=add_span(spans,n,j,j+kl,TOK_KEYWORD); j+=kl-1;
            }
        }
        return n;
    }

    /* --- PHP --- */
    if (current_lang == LANG_PHP) {
        int j=0; int in_str=0; char str_ch=0;
        /* block-comment state */
        int inside_block_php = (in_block && line_idx < in_block_cap) ? in_block[line_idx] : 0;
        if (inside_block_php) {
            const char *close = strstr(line, "*/");
            if (!close) return add_span(spans, n, 0, len, TOK_COMMENT);
            int ce = (int)(close - line) + 2;
            n = add_span(spans, n, 0, ce, TOK_COMMENT);
            j = ce;
        }
        for(;j<len;j++){
            /* block comment */
            if(!in_str && line[j]=='/' && j+1<len && line[j+1]=='*'){
                int s=j; j+=2;
                while(j<len){if(line[j]=='*'&&j+1<len&&line[j+1]=='/'){j+=2;break;}j++;}
                n=add_span(spans,n,s,j,TOK_COMMENT); j--; continue;
            }
            /* line comment // */
            if(!in_str && line[j]=='/' && j+1<len && line[j+1]=='/'){
                n=add_span(spans,n,j,len,TOK_COMMENT); return n;
            }
            /* line comment # */
            if(!in_str && line[j]=='#'){
                n=add_span(spans,n,j,len,TOK_COMMENT); return n;
            }
            /* PHP open/close tags as preprocessor */
            if(!in_str && line[j]=='<' && j+4<len && strncmp(line+j,"<?php",5)==0){
                n=add_span(spans,n,j,j+5,TOK_PREPROC); j+=4; continue;
            }
            if(!in_str && line[j]=='<' && j+1<len && line[j+1]=='?'){
                n=add_span(spans,n,j,j+2,TOK_PREPROC); j+=1; continue;
            }
            if(!in_str && line[j]=='?' && j+1<len && line[j+1]=='>'){
                n=add_span(spans,n,j,j+2,TOK_PREPROC); j+=1; continue;
            }
            /* strings */
            if(!in_str && (line[j]=='"'||line[j]=='\'' )){
                in_str=1; str_ch=line[j]; int s=j; j++;
                while(j<len){if(line[j]=='\\'){j+=2;continue;}if(line[j]==str_ch){j++;break;}j++;}
                n=add_span(spans,n,s,j,TOK_STRING); in_str=0; j--; continue;
            }
            /* variables: $identifier */
            if(!in_str && line[j]=='$' && j+1<len && (isalpha((unsigned char)line[j+1])||line[j+1]=='_')){
                int s=j; j++;
                while(j<len&&(isalnum((unsigned char)line[j])||line[j]=='_')) j++;
                n=add_span(spans,n,s,j,TOK_PREPROC); j--; continue;
            }
            int kl=0;
            if(match_keyword(line,j,len,kw_php,&kl)){
                n=add_span(spans,n,j,j+kl,TOK_KEYWORD); j+=kl-1; continue;
            }
        }
        return n;
    }

    /* --- Rust --- */
    if (current_lang == LANG_RUST) {
        int j=0; int in_str=0;
        for(;j<len;j++){
            if(!in_str && line[j]=='/' && j+1<len && line[j+1]=='/'){
                n=add_span(spans,n,j,len,TOK_COMMENT); return n;
            }
            if(!in_str && line[j]=='"'){
                in_str=1; int s=j; j++;
                while(j<len){if(line[j]=='\\'){j+=2;continue;}if(line[j]=='"'){j++;break;}j++;}
                n=add_span(spans,n,s,j,TOK_STRING); in_str=0; j--; continue;
            }
            int kl=0;
            if(match_keyword(line,j,len,kw_rust,&kl)){
                n=add_span(spans,n,j,j+kl,TOK_KEYWORD); j+=kl-1;
            }
        }
        return n;
    }

    /* --- C / C++ --- */
    /* block-comment state carried in from syntax_scan */
    int inside_block = (in_block && line_idx < in_block_cap) ? in_block[line_idx] : 0;
    int in_str  = 0;
    int in_chr  = 0;

    /* leading whitespace — always normal */
    int j = 0;
    while (j < len && (line[j]==' '||line[j]=='\t')) j++;

    /* preprocessor directive: # as first non-whitespace token */
    if (j < len && line[j] == '#' && !inside_block) {
        return add_span(spans, n, j, len, TOK_PREPROC);
    }

    /* if whole line is inside a block comment */
    if (inside_block) {
        /* scan for closing */ 
        const char *close = strstr(line, "*/");
        if (!close) return add_span(spans, n, 0, len, TOK_COMMENT);
        int close_end = (int)(close - line) + 2;
        n = add_span(spans, n, 0, close_end, TOK_COMMENT);
        inside_block = 0;
        j = close_end;
    }

    for (; j < len; j++) {
        /* block comment open */
        if (!in_str && !in_chr && line[j]=='/' && j+1<len && line[j+1]=='*') {
            int s = j;
            j += 2;
            while (j < len) {
                if (line[j]=='*' && j+1<len && line[j+1]=='/') { j+=2; break; }
                j++;
            }
            n = add_span(spans, n, s, j, TOK_COMMENT);
            j--; continue;
        }
        /* line comment */
        if (!in_str && !in_chr && line[j]=='/' && j+1<len && line[j+1]=='/') {
            n = add_span(spans, n, j, len, TOK_COMMENT);
            return n;
        }
        /* char literal */
        if (!in_str && line[j]=='\'') {
            int s=j; j++;
            while(j<len){if(line[j]=='\\'){j+=2;continue;}if(line[j]=='\''){j++;break;}j++;}
            n=add_span(spans,n,s,j,TOK_STRING); j--; continue;
        }
        /* string literal */
        if (!in_chr && line[j]=='"') {
            int s=j; j++;
            while(j<len){if(line[j]=='\\'){j+=2;continue;}if(line[j]=='"'){j++;break;}j++;}
            n=add_span(spans,n,s,j,TOK_STRING); j--; continue;
        }
        /* keyword */
        int kl=0;
        if(match_keyword(line,j,len,kw_c,&kl)){
            n=add_span(spans,n,j,j+kl,TOK_KEYWORD); j+=kl-1; continue;
        }
        (void)in_str; (void)in_chr;
    }
    return n;
}
