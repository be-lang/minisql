/* ===========================================================================
 * minisql — a tiny SQL engine in C, no dependencies.
 * Copyright (c) 2026 Benjamin Lang. All rights reserved.
 *
 * Loads CSV files as in-memory tables and runs SELECT queries over them:
 * WHERE, INNER/LEFT JOIN, GROUP BY + aggregates, HAVING, ORDER BY, LIMIT.
 * Includes a small cost-based planner (hash vs nested-loop join selection,
 * predicate pushdown, greedy join reordering) and an EXPLAIN command.
 *
 * The whole engine lives in this one file, in top-to-bottom dependency order:
 *   StringList  ->  CSV loader  ->  data model (Value/Row/Table)  ->  type
 *   inference  ->  tokenizer  ->  recursive-descent parser (AST)  ->
 *   executor (over an intermediate Rel)  ->  query planner  ->  REPL.
 *
 * Build:  make          (or: cc -Wall -Wextra -std=c11 -O2 -o minisql main.c)
 * Run:    ./minisql     (then .help)      Test: make test      Bench: bench/bench.sh
 * ========================================================================= */
#define _POSIX_C_SOURCE 200809L  /* makes getline() and strdup() available */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include <unistd.h>
#include <time.h>

/* ---------------------------------------------------------------------------
 * Checked allocators: a query engine mid-operation has no sensible way to
 * recover from OOM, so fail fast with a message rather than crash on NULL.
 * ------------------------------------------------------------------------- */
static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p && n) { fprintf(stderr, "fatal: out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}
static void *xcalloc(size_t n, size_t sz) {
    void *p = calloc(n, sz);
    if (!p && n && sz) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }
    return p;
}
static void *xrealloc(void *q, size_t n) {
    void *p = realloc(q, n);
    if (!p && n) { fprintf(stderr, "fatal: out of memory (%zu bytes)\n", n); exit(1); }
    return p;
}
static char *xstrdup(const char *s) {
    char *p = strdup(s);
    if (!p) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }
    return p;
}

/* ---------------------------------------------------------------------------
 * StringList: a growable array of strings.
 * Used for BOTH the lines of a file and the fields of a line.
 * This is the fundamental building block of the whole project — Table will be
 * a growable array of rows, Row a growable array of values, same idea.
 * ------------------------------------------------------------------------- */
typedef struct {
    char **items;      /* the strings; items[i] is one string           */
    size_t count;      /* how many are currently stored                 */
    size_t capacity;   /* how many we have room for before we must grow */
} StringList;

static void sl_init(StringList *sl) {
    sl->items = NULL;
    sl->count = 0;
    sl->capacity = 0;
}

/* Append s to the list. Takes ownership of s (will be freed by sl_free). */
static void sl_push(StringList *sl, char *s) {
    if (sl->count == sl->capacity) {
        /* out of room: double the capacity (start at 8) */
        sl->capacity = sl->capacity ? sl->capacity * 2 : 8;
        sl->items = xrealloc(sl->items, sl->capacity * sizeof(char *));
    }
    sl->items[sl->count++] = s;
}

static void sl_free(StringList *sl) {
    for (size_t i = 0; i < sl->count; i++)
        free(sl->items[i]);
    free(sl->items);
    sl_init(sl);  /* reset to a clean empty state */
}

/* ---------------------------------------------------------------------------
 * The data model: how a relation lives in memory.
 *
 *   ColumnType  — what kind of data a cell holds.
 *   Value       — ONE cell (the atomic value of the relational model).
 *   Row         — one tuple: an array of Values.
 *   Table       — one relation: a schema (column names + types) plus rows.
 * ------------------------------------------------------------------------- */
typedef enum {
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_STRING,
    TYPE_NULL      /* an empty / missing cell */
} ColumnType;

/* A Value is a "tagged union": `type` says which member of `as` is the live one.
 * The union stores only ONE of these at a time (they share memory), so the tag
 * is the ONLY way to know how to read it. Reading the wrong member = garbage.
 * Storing a long + a double + a char* separately would waste space; the union
 * says "it's exactly one of these" and the tag says which. */
typedef struct {
    ColumnType type;
    union {
        long   int_val;
        double float_val;
        char  *str_val;   /* owned: freed when the Value is freed */
    } as;
} Value;

/* A Row is one tuple: `count` values, in column order. */
typedef struct {
    Value *values;
    size_t count;
} Row;

/* A Table is a relation. Column names and types are kept as two parallel
 * arrays of length `col_count` (col_names[i] has type col_types[i]).
 * `rows` is a growable array, same doubling trick as StringList. */
struct Index;                          /* defined near the executor (needs value hashing) */
static void indexes_free(struct Index *arr, size_t n);   /* frees an index array */

typedef struct {
    char       *name;         /* usually derived from the filename        */
    char      **col_names;    /* length col_count                         */
    ColumnType *col_types;    /* length col_count                         */
    size_t      col_count;
    Row        *rows;         /* length row_count, room for row_capacity  */
    size_t      row_count;
    size_t      row_capacity;
    struct Index *indexes;    /* optional secondary indexes (see .index)  */
    size_t      nindex;
} Table;

static void table_free(Table *t) {
    free(t->name);
    for (size_t i = 0; i < t->col_count; i++)
        free(t->col_names[i]);
    free(t->col_names);
    free(t->col_types);
    for (size_t i = 0; i < t->row_count; i++) {
        for (size_t j = 0; j < t->rows[i].count; j++)
            if (t->rows[i].values[j].type == TYPE_STRING)
                free(t->rows[i].values[j].as.str_val);
        free(t->rows[i].values);
    }
    free(t->rows);
    indexes_free(t->indexes, t->nindex);
}

/* ---------------------------------------------------------------------------
 * read_lines: read a whole file into a StringList of lines.
 * Each line has its trailing newline stripped. On error, returns an empty list.
 * ------------------------------------------------------------------------- */
static StringList read_lines(const char *filename) {
    StringList lines;
    sl_init(&lines);

    FILE *file = fopen(filename, "r");
    if (file == NULL) {
        perror("Error opening file");
        return lines;  /* empty list — caller can check .count == 0 */
    }

    /* getline grows `buf` as needed, so lines of ANY length work — no more
       fixed 256-byte limit. It returns the length read, or -1 at EOF. */
    char *buf = NULL;
    size_t bufsize = 0;
    ssize_t len;
    while ((len = getline(&buf, &bufsize, file)) != -1) {
        /* strip trailing newline / carriage-return so fields stay clean */
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
            buf[--len] = '\0';
        /* strdup makes our OWN copy — getline reuses `buf` next loop */
        sl_push(&lines, xstrdup(buf));
    }

    free(buf);
    fclose(file);
    return lines;
}

/* ---------------------------------------------------------------------------
 * split_line: cut one line into fields, honoring RFC 4180 quoting.
 *
 *   plain fields  : read until a comma.               a,b,c      -> a | b | c
 *   quoted fields : wrapped in "..." — a comma inside is literal.
 *                     "Lennon, John"                  -> Lennon, John
 *   escaped quote : a doubled "" inside a quoted field is one literal ".
 *                     "she said ""hi"""               -> she said "hi"
 *
 * Known limitation: a quoted field containing a NEWLINE would span two physical
 * lines, but read_lines already split on newlines, so we can't see it here.
 * Supporting that needs record-based reading — deferred, not needed for our data.
 * ------------------------------------------------------------------------- */
static StringList split_line(const char *line) {
    StringList fields;
    sl_init(&fields);

    /* A single field can never be longer than the whole line, so one buffer of
       that size is always big enough; we reuse it for every field. */
    char *buf = xmalloc(strlen(line) + 1);
    size_t i = 0;   /* read cursor into `line` */

    for (;;) {
        size_t b = 0;   /* write cursor into `buf` for the current field */

        if (line[i] == '"') {
            i++;                                    /* consume opening quote */
            while (line[i] != '\0') {
                if (line[i] == '"' && line[i + 1] == '"') {
                    buf[b++] = '"';                 /* "" -> one literal quote */
                    i += 2;
                } else if (line[i] == '"') {
                    i++;                            /* closing quote */
                    break;
                } else {
                    buf[b++] = line[i++];           /* literal char (incl. ',') */
                }
            }
            while (line[i] != '\0' && line[i] != ',')
                i++;                                /* skip anything until the comma */
        } else {
            while (line[i] != '\0' && line[i] != ',')
                buf[b++] = line[i++];               /* plain field */
        }

        buf[b] = '\0';
        sl_push(&fields, xstrdup(buf));

        if (line[i] == ',') { i++; continue; }      /* another field follows */
        break;                                       /* end of line */
    }

    free(buf);
    return fields;
}

/* ---------------------------------------------------------------------------
 * parse_header: build the SCHEMA of a Table from the header line.
 * Column names come from splitting line 0. Types are left as TYPE_NULL
 * placeholders — inference (the next step) will fill them in. No rows yet.
 * ------------------------------------------------------------------------- */
static Table parse_header(const char *name, const char *header) {
    StringList cols = split_line(header);

    Table t;
    t.name         = xstrdup(name);
    t.col_count    = cols.count;
    t.col_names    = xmalloc(t.col_count * sizeof(char *));
    t.col_types    = xmalloc(t.col_count * sizeof(ColumnType));
    t.rows         = NULL;
    t.row_count    = 0;
    t.row_capacity = 0;
    t.indexes      = NULL;
    t.nindex       = 0;

    for (size_t i = 0; i < t.col_count; i++) {
        t.col_names[i] = xstrdup(cols.items[i]);
        t.col_types[i] = TYPE_NULL;   /* unknown until we infer it */
    }

    sl_free(&cols);
    return t;
}

/* ---------------------------------------------------------------------------
 * Type inference: decide a column's type by looking at its raw string values.
 *
 * looks_like_int / looks_like_float use the strtol/strtod "endptr trick":
 * the parse function sets `end` to the first character it could NOT consume.
 * If `end` lands on the '\0', the WHOLE string was a valid number. If it stops
 * early (e.g. at '.' or a letter), the string only *starts* like a number, so
 * we reject it. errno catches overflow.
 * ------------------------------------------------------------------------- */
static int looks_like_int(const char *s) {
    if (*s == '\0') return 0;              /* empty is not an int */
    char *end;
    errno = 0;
    strtol(s, &end, 10);
    return errno == 0 && *end == '\0';     /* consumed everything, no overflow */
}

static int looks_like_float(const char *s) {
    if (*s == '\0') return 0;
    char *end;
    errno = 0;
    strtod(s, &end);
    return errno == 0 && *end == '\0';
}

/* Scan every value in a column and decide its type.
 * Order matters: an all-int column is INT, but "1, 2.5, 3" widens to FLOAT.
 * Empty cells don't vote (they become NULL later); an all-empty column is STRING. */
static ColumnType infer_column_type(const StringList *col) {
    int seen_value = 0, all_int = 1, all_float = 1;
    for (size_t i = 0; i < col->count; i++) {
        const char *s = col->items[i];
        if (*s == '\0') continue;          /* empty cell: skip */
        seen_value = 1;
        if (!looks_like_int(s))   all_int = 0;
        if (!looks_like_float(s)) all_float = 0;
    }
    if (!seen_value) return TYPE_STRING;   /* column was entirely empty */
    if (all_int)   return TYPE_INT;        /* test int BEFORE float */
    if (all_float) return TYPE_FLOAT;
    return TYPE_STRING;
}

/* Convert one raw field into a typed Value, given its column's decided type.
 * An empty field becomes NULL regardless of the column type. */
static Value make_value_from_string(const char *raw, ColumnType type) {
    Value v;
    if (*raw == '\0') {                     /* missing cell */
        v.type = TYPE_NULL;
        return v;
    }
    switch (type) {
        case TYPE_INT:
            v.type = TYPE_INT;
            v.as.int_val = strtol(raw, NULL, 10);
            break;
        case TYPE_FLOAT:
            v.type = TYPE_FLOAT;
            v.as.float_val = strtod(raw, NULL);
            /* NaN breaks compare/hash consistency (it is not equal to itself),
               so store it as NULL — exactly what sqlite does on insert. */
            if (v.as.float_val != v.as.float_val) v.type = TYPE_NULL;
            break;
        default:                            /* STRING (or a NULL-typed column) */
            v.type = TYPE_STRING;
            v.as.str_val = xstrdup(raw);
            break;
    }
    return v;
}

/* Small helper: human-readable name for a type, handy for printing schemas. */
static const char *type_name(ColumnType t) {
    switch (t) {
        case TYPE_INT:    return "int";
        case TYPE_FLOAT:  return "float";
        case TYPE_STRING: return "string";
        case TYPE_NULL:   return "null";
    }
    return "?";
}

/* Append a row to a table, growing the row array by doubling (same trick as
 * sl_push). The table takes ownership of the row's Values. */
static void table_add_row(Table *t, Row row) {
    if (t->row_count == t->row_capacity) {
        t->row_capacity = t->row_capacity ? t->row_capacity * 2 : 8;
        t->rows = xrealloc(t->rows, t->row_capacity * sizeof(Row));
    }
    t->rows[t->row_count++] = row;
}

/* ---------------------------------------------------------------------------
 * load_table: the conductor. Turns a CSV file into a fully typed Table by
 * wiring together every function built so far.
 * ------------------------------------------------------------------------- */
static Table load_table(const char *name, const char *filename) {
    StringList lines = read_lines(filename);

    Table t;
    if (lines.count == 0) {                 /* empty / missing file */
        memset(&t, 0, sizeof t);
        t.name = xstrdup(name);
        sl_free(&lines);
        return t;
    }

    t = parse_header(name, lines.items[0]);

    /* Split every data row ONCE and keep the field lists — we use them twice:
       first to infer column types, then to build the typed Values. */
    size_t nrows = lines.count - 1;
    StringList *rowfields = xmalloc(nrows * sizeof(StringList));
    size_t ragged = 0;
    for (size_t r = 0; r < nrows; r++) {
        rowfields[r] = split_line(lines.items[r + 1]);
        /* extra fields would be silently dropped below — never lose data silently */
        if (rowfields[r].count > t.col_count && ragged++ < 5)
            fprintf(stderr, "warning: %s row %zu has %zu fields, expected %zu (extra dropped)\n",
                    filename, r + 2, rowfields[r].count, t.col_count);
    }
    if (ragged > 5)
        fprintf(stderr, "warning: %s: %zu more rows with extra fields\n", filename, ragged - 5);

    /* Infer each column's type from its values across all rows. We BORROW the
       already-split field pointers (no strdup) — inference only reads them —
       and free just the pointer array, not the strings. */
    char **colvals = xmalloc(nrows * sizeof(char *));
    for (size_t c = 0; c < t.col_count; c++) {
        StringList col = { colvals, 0, nrows };
        for (size_t r = 0; r < nrows; r++)
            if (c < rowfields[r].count) colvals[col.count++] = rowfields[r].items[c];
        t.col_types[c] = infer_column_type(&col);
    }
    free(colvals);

    /* Build one typed Row per data line. A missing field (ragged row) is
       treated as empty -> NULL, so every row has exactly col_count values. */
    for (size_t r = 0; r < nrows; r++) {
        Row row;
        row.count  = t.col_count;
        row.values = xmalloc(t.col_count * sizeof(Value));
        for (size_t c = 0; c < t.col_count; c++) {
            const char *raw = (c < rowfields[r].count) ? rowfields[r].items[c] : "";
            row.values[c] = make_value_from_string(raw, t.col_types[c]);
        }
        table_add_row(&t, row);
    }

    for (size_t r = 0; r < nrows; r++) sl_free(&rowfields[r]);
    free(rowfields);
    sl_free(&lines);
    return t;
}

/* ---------------------------------------------------------------------------
 * print_table: render a Table as an aligned grid — your eyes for the whole
 * rest of the project, since query results are Tables too.
 * ------------------------------------------------------------------------- */
static void value_str(const Value *v, char *buf, size_t n) {
    switch (v->type) {
        case TYPE_INT:    snprintf(buf, n, "%ld", v->as.int_val);   break;
        case TYPE_FLOAT:  snprintf(buf, n, "%g",  v->as.float_val); break;
        case TYPE_STRING: snprintf(buf, n, "%s",  v->as.str_val);   break;
        case TYPE_NULL:   snprintf(buf, n, "NULL");                 break;
    }
}

static void print_table(const Table *t) {
    char buf[512];

    /* First pass: each column is as wide as the widest of its name, its type
       label, and any value in it. */
    size_t *w = xmalloc(t->col_count * sizeof(size_t));
    for (size_t c = 0; c < t->col_count; c++) {
        w[c] = strlen(t->col_names[c]);
        size_t tl = strlen(type_name(t->col_types[c]));
        if (tl > w[c]) w[c] = tl;
    }
    for (size_t r = 0; r < t->row_count; r++)
        for (size_t c = 0; c < t->col_count; c++) {
            value_str(&t->rows[r].values[c], buf, sizeof buf);
            size_t len = strlen(buf);
            if (len > w[c]) w[c] = len;
        }

    /* Column names, then a types row, then a separator, then the data. */
    for (size_t c = 0; c < t->col_count; c++)
        printf("%-*s  ", (int)w[c], t->col_names[c]);
    printf("\n");
    for (size_t c = 0; c < t->col_count; c++)
        printf("%-*s  ", (int)w[c], type_name(t->col_types[c]));
    printf("\n");
    for (size_t c = 0; c < t->col_count; c++) {
        for (size_t i = 0; i < w[c]; i++) putchar('-');
        printf("  ");
    }
    printf("\n");
    for (size_t r = 0; r < t->row_count; r++) {
        for (size_t c = 0; c < t->col_count; c++) {
            value_str(&t->rows[r].values[c], buf, sizeof buf);
            printf("%-*s  ", (int)w[c], buf);
        }
        printf("\n");
    }

    free(w);
    printf("(%zu row%s)\n", t->row_count, t->row_count == 1 ? "" : "s");
}

/* ===========================================================================
 * STEP 2 — TOKENIZER (lexer): SQL text -> a stream of tokens.
 * Tokens are the atoms of the language. No meaning yet, just "what kind of
 * chunk is this": a keyword, a name, a number, a string, or an operator.
 * ========================================================================= */
typedef enum {
    /* keywords */
    TOK_SELECT, TOK_FROM, TOK_WHERE, TOK_JOIN, TOK_ON, TOK_LEFT, TOK_INNER,
    TOK_GROUP, TOK_BY, TOK_ORDER, TOK_ASC, TOK_DESC, TOK_HAVING, TOK_LIMIT,
    TOK_AND, TOK_OR, TOK_NOT, TOK_AS, TOK_NULL_KW,
    TOK_COUNT, TOK_SUM, TOK_AVG, TOK_MIN, TOK_MAX,
    TOK_DISTINCT, TOK_LIKE, TOK_IN, TOK_BETWEEN, TOK_IS,
    /* literals / names */
    TOK_IDENT, TOK_STRING, TOK_INT, TOK_FLOAT,
    /* punctuation & operators */
    TOK_STAR, TOK_COMMA, TOK_DOT, TOK_LPAREN, TOK_RPAREN,
    TOK_PLUS, TOK_MINUS, TOK_SLASH,
    TOK_EQ, TOK_NE, TOK_LT, TOK_GT, TOK_LE, TOK_GE,
    TOK_EOF, TOK_ERROR
} TokenType;

typedef struct {
    TokenType type;
    char     *text;   /* owned lexeme for ident/string/number; else NULL */
    int       pos;    /* offset into the SQL string, for error messages  */
} Token;

typedef struct {
    Token *items;
    size_t count;
    size_t capacity;
} TokenList;

/* keyword table: text (lowercase) -> token type */
static const struct { const char *kw; TokenType type; } KEYWORDS[] = {
    {"select", TOK_SELECT}, {"from", TOK_FROM}, {"where", TOK_WHERE},
    {"join", TOK_JOIN}, {"on", TOK_ON}, {"left", TOK_LEFT}, {"inner", TOK_INNER},
    {"group", TOK_GROUP}, {"by", TOK_BY}, {"order", TOK_ORDER},
    {"asc", TOK_ASC}, {"desc", TOK_DESC}, {"having", TOK_HAVING},
    {"limit", TOK_LIMIT}, {"and", TOK_AND}, {"or", TOK_OR}, {"not", TOK_NOT},
    {"as", TOK_AS}, {"null", TOK_NULL_KW},
    {"count", TOK_COUNT}, {"sum", TOK_SUM}, {"avg", TOK_AVG},
    {"min", TOK_MIN}, {"max", TOK_MAX},
    {"distinct", TOK_DISTINCT}, {"like", TOK_LIKE}, {"in", TOK_IN},
    {"between", TOK_BETWEEN}, {"is", TOK_IS},
};

static int ci_equal(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == '\0' && *b == '\0';
}

static void tl_push(TokenList *tl, TokenType type, char *text, int pos) {
    if (tl->count == tl->capacity) {
        tl->capacity = tl->capacity ? tl->capacity * 2 : 16;
        tl->items = xrealloc(tl->items, tl->capacity * sizeof(Token));
    }
    tl->items[tl->count].type = type;
    tl->items[tl->count].text = text;
    tl->items[tl->count].pos  = pos;
    tl->count++;
}

static void tl_free(TokenList *tl) {
    for (size_t i = 0; i < tl->count; i++)
        free(tl->items[i].text);
    free(tl->items);
    tl->items = NULL; tl->count = 0; tl->capacity = 0;
}

/* Copy the substring sql[start..end) into a fresh NUL-terminated string. */
static char *substr(const char *sql, int start, int end) {
    int n = end - start;
    char *s = xmalloc(n + 1);
    memcpy(s, sql + start, n);
    s[n] = '\0';
    return s;
}

/* Turn a whole SQL string into a TokenList (always ends with TOK_EOF).
 * On a bad character, pushes a TOK_ERROR token and stops. */
static TokenList tokenize(const char *sql) {
    TokenList tl = {0};
    int i = 0;

    while (sql[i] != '\0') {
        char c = sql[i];

        if (isspace((unsigned char)c)) { i++; continue; }

        /* identifiers & keywords: [A-Za-z_][A-Za-z0-9_]* */
        if (isalpha((unsigned char)c) || c == '_') {
            int start = i;
            while (isalnum((unsigned char)sql[i]) || sql[i] == '_') i++;
            char *word = substr(sql, start, i);
            TokenType type = TOK_IDENT;
            for (size_t k = 0; k < sizeof(KEYWORDS)/sizeof(KEYWORDS[0]); k++)
                if (ci_equal(word, KEYWORDS[k].kw)) { type = KEYWORDS[k].type; break; }
            if (type == TOK_IDENT) tl_push(&tl, TOK_IDENT, word, start);
            else { free(word); tl_push(&tl, type, NULL, start); }
            continue;
        }

        /* numbers: digits, with an optional single '.' making it a float */
        if (isdigit((unsigned char)c)) {
            int start = i;
            int is_float = 0;
            while (isdigit((unsigned char)sql[i])) i++;
            if (sql[i] == '.') { is_float = 1; i++; while (isdigit((unsigned char)sql[i])) i++; }
            tl_push(&tl, is_float ? TOK_FLOAT : TOK_INT, substr(sql, start, i), start);
            continue;
        }

        /* string literal: '...'  ('' is an escaped single quote) */
        if (c == '\'') {
            int start = i;
            i++;                              /* skip opening quote */
            /* build the unescaped contents into a buffer */
            char *out = xmalloc(strlen(sql) + 1);
            int b = 0, closed = 0;
            while (sql[i] != '\0') {
                if (sql[i] == '\'' && sql[i+1] == '\'') { out[b++] = '\''; i += 2; }
                else if (sql[i] == '\'') { i++; closed = 1; break; }
                else out[b++] = sql[i++];
            }
            out[b] = '\0';
            if (!closed) {                    /* ran off the end without a closing quote */
                free(out);
                fprintf(stderr, "tokenize: unterminated string literal at %d\n", start);
                tl_push(&tl, TOK_ERROR, NULL, start); goto done;
            }
            tl_push(&tl, TOK_STRING, out, start);
            continue;
        }

        /* operators & punctuation */
        int start = i;
        switch (c) {
            case '*': tl_push(&tl, TOK_STAR,   NULL, start); i++; break;
            case '+': tl_push(&tl, TOK_PLUS,   NULL, start); i++; break;
            case '-': tl_push(&tl, TOK_MINUS,  NULL, start); i++; break;
            case '/': tl_push(&tl, TOK_SLASH,  NULL, start); i++; break;
            case ',': tl_push(&tl, TOK_COMMA,  NULL, start); i++; break;
            case '.': tl_push(&tl, TOK_DOT,    NULL, start); i++; break;
            case '(': tl_push(&tl, TOK_LPAREN, NULL, start); i++; break;
            case ')': tl_push(&tl, TOK_RPAREN, NULL, start); i++; break;
            case '=': tl_push(&tl, TOK_EQ,     NULL, start); i++; break;
            case '<':
                if (sql[i+1] == '=') { tl_push(&tl, TOK_LE, NULL, start); i += 2; }
                else if (sql[i+1] == '>') { tl_push(&tl, TOK_NE, NULL, start); i += 2; }
                else { tl_push(&tl, TOK_LT, NULL, start); i++; }
                break;
            case '>':
                if (sql[i+1] == '=') { tl_push(&tl, TOK_GE, NULL, start); i += 2; }
                else { tl_push(&tl, TOK_GT, NULL, start); i++; }
                break;
            case '!':
                if (sql[i+1] == '=') { tl_push(&tl, TOK_NE, NULL, start); i += 2; }
                else { fprintf(stderr, "tokenize: stray '!' at %d\n", i);
                       tl_push(&tl, TOK_ERROR, NULL, start); goto done; }
                break;
            default:
                fprintf(stderr, "tokenize: unexpected character '%c' at %d\n", c, i);
                tl_push(&tl, TOK_ERROR, NULL, start);
                goto done;
        }
    }
done:
    tl_push(&tl, TOK_EOF, NULL, i);
    return tl;
}

/* debug helper: readable name for a token type */
static const char *tok_name(TokenType t) {
    switch (t) {
        case TOK_SELECT: return "SELECT"; case TOK_FROM: return "FROM";
        case TOK_WHERE: return "WHERE"; case TOK_JOIN: return "JOIN";
        case TOK_ON: return "ON"; case TOK_LEFT: return "LEFT";
        case TOK_INNER: return "INNER"; case TOK_GROUP: return "GROUP";
        case TOK_BY: return "BY"; case TOK_ORDER: return "ORDER";
        case TOK_ASC: return "ASC"; case TOK_DESC: return "DESC";
        case TOK_HAVING: return "HAVING"; case TOK_LIMIT: return "LIMIT";
        case TOK_AND: return "AND"; case TOK_OR: return "OR";
        case TOK_NOT: return "NOT"; case TOK_AS: return "AS";
        case TOK_NULL_KW: return "NULL"; case TOK_COUNT: return "COUNT";
        case TOK_SUM: return "SUM"; case TOK_AVG: return "AVG";
        case TOK_MIN: return "MIN"; case TOK_MAX: return "MAX";
        case TOK_IDENT: return "ident"; case TOK_STRING: return "string";
        case TOK_INT: return "int"; case TOK_FLOAT: return "float";
        case TOK_DISTINCT: return "DISTINCT"; case TOK_LIKE: return "LIKE";
        case TOK_IN: return "IN"; case TOK_BETWEEN: return "BETWEEN"; case TOK_IS: return "IS";
        case TOK_PLUS: return "+"; case TOK_MINUS: return "-"; case TOK_SLASH: return "/";
        case TOK_STAR: return "*"; case TOK_COMMA: return ","; case TOK_DOT: return ".";
        case TOK_LPAREN: return "("; case TOK_RPAREN: return ")";
        case TOK_EQ: return "="; case TOK_NE: return "!="; case TOK_LT: return "<";
        case TOK_GT: return ">"; case TOK_LE: return "<="; case TOK_GE: return ">=";
        case TOK_EOF: return "EOF"; case TOK_ERROR: return "ERROR";
    }
    return "?";
}

/* ===========================================================================
 * STEP 3 — PARSER (recursive descent): tokens -> an AST (a SelectStmt).
 * Each grammar rule becomes one function that consumes tokens and builds a
 * node. This is the "logical plan" in its simplest form.
 * ========================================================================= */

/* A column reference, optionally qualified by a table/alias: [table.]column */
typedef struct { char *table; char *column; } ColRef;

/* --- expression tree (for WHERE / HAVING / JOIN ON / SELECT items) --- */
typedef enum {
    EXPR_COLUMN, EXPR_LITERAL, EXPR_BINARY, EXPR_AGG,
    EXPR_UNARY,     /* -x, NOT x                     */
    EXPR_FUNC,      /* UPPER(x), LENGTH(x), ...      */
    EXPR_IN,        /* x IN (a, b, c)                */
    EXPR_ISNULL,    /* x IS [NOT] NULL               */
    EXPR_BETWEEN    /* x BETWEEN lo AND hi           */
} ExprKind;
typedef struct Expr {
    ExprKind  kind;
    ColRef    col;           /* EXPR_COLUMN                          */
    Value     literal;       /* EXPR_LITERAL                         */
    TokenType op;            /* EXPR_BINARY/UNARY: operator or LIKE  */
    TokenType agg;           /* EXPR_AGG: COUNT/SUM/AVG/MIN/MAX       */
    int       agg_star;      /* EXPR_AGG: 1 for COUNT(*)             */
    int       distinct;      /* EXPR_AGG: COUNT(DISTINCT x)          */
    char     *fname;         /* EXPR_FUNC: function name             */
    struct Expr **args;      /* EXPR_FUNC / EXPR_IN: sub-expressions */
    size_t    nargs;
    int       negated;       /* EXPR_IN / EXPR_ISNULL: NOT variant   */
    struct Expr *left, *right, *lo, *hi;  /* operands (lo/hi for BETWEEN) */
} Expr;

/* --- one item in the SELECT list --- */
typedef enum { SEL_STAR, SEL_EXPR } SelKind;
typedef struct {
    SelKind kind;
    Expr   *expr;            /* SEL_EXPR: any scalar/aggregate expression */
    char   *alias;           /* optional AS alias                        */
} SelItem;

typedef enum { JOIN_INNER, JOIN_LEFT } JoinType;
typedef struct { JoinType type; char *table; char *alias; Expr *on; } JoinClause;
/* an ORDER BY key: an expression, or a positional index (`ORDER BY 2`) */
typedef struct { Expr *expr; long pos; int desc; } OrderItem;

typedef struct {
    SelItem    *items;    size_t nitems;
    int         distinct;                 /* SELECT DISTINCT */
    char       *from_table; char *from_alias;
    JoinClause *joins;    size_t njoins;
    Expr       *where;                    /* NULL if absent */
    ColRef     *group;    size_t ngroup;
    Expr       *having;                   /* NULL if absent */
    OrderItem  *order;    size_t norder;
    long        limit;                    /* -1 = no LIMIT  */
} SelectStmt;

/* --- parser state --- */
typedef struct { TokenList *tl; size_t pos; int error; int depth; } Parser;
#define PARSE_MAX_DEPTH 500   /* guards against stack overflow on deeply nested input */

static Token *peek(Parser *p)            { return &p->tl->items[p->pos]; }
static int    check(Parser *p, TokenType t){ return peek(p)->type == t; }
static Token *advance(Parser *p)         { Token *t = peek(p); if (t->type != TOK_EOF) p->pos++; return t; }
static int    match(Parser *p, TokenType t){ if (check(p, t)) { advance(p); return 1; } return 0; }

static void perr(Parser *p, const char *msg) {
    if (!p->error)  /* report only the first error */
        fprintf(stderr, "parse error at token %zu (%s): %s\n",
                p->pos, tok_name(peek(p)->type), msg);
    p->error = 1;
}
static void expect(Parser *p, TokenType t, const char *msg) {
    if (!match(p, t)) perr(p, msg);
}

/* [table '.'] column   — an identifier, optionally qualified */
static ColRef parse_colref(Parser *p) {
    ColRef c = {0};
    if (!check(p, TOK_IDENT)) { perr(p, "expected a column name"); return c; }
    char *first = advance(p)->text;
    if (match(p, TOK_DOT)) {
        c.table = xstrdup(first);
        if (!check(p, TOK_IDENT)) { perr(p, "expected column after '.'"); return c; }
        c.column = xstrdup(advance(p)->text);
    } else {
        c.column = xstrdup(first);
    }
    return c;
}

/* forward decls */
static Expr *parse_expr(Parser *p);

/* primary = literal | column | '(' expr ')' */
/* primary = literal | NULL | agg'('[DISTINCT] expr | '*'')' | func'('args')'
 *         | column | '(' expr ')' */
static Expr *parse_primary(Parser *p) {
    Expr *e = xcalloc(1, sizeof(Expr));
    if (match(p, TOK_LPAREN)) {
        free(e);
        if (++p->depth > PARSE_MAX_DEPTH) { perr(p, "expression nested too deeply"); return NULL; }
        Expr *inner = parse_expr(p);
        p->depth--;
        expect(p, TOK_RPAREN, "expected ')'");
        return inner;
    }
    if (check(p, TOK_INT)) {
        e->kind = EXPR_LITERAL; e->literal.type = TYPE_INT;
        e->literal.as.int_val = strtol(advance(p)->text, NULL, 10); return e;
    }
    if (check(p, TOK_FLOAT)) {
        e->kind = EXPR_LITERAL; e->literal.type = TYPE_FLOAT;
        e->literal.as.float_val = strtod(advance(p)->text, NULL); return e;
    }
    if (check(p, TOK_STRING)) {
        e->kind = EXPR_LITERAL; e->literal.type = TYPE_STRING;
        e->literal.as.str_val = xstrdup(advance(p)->text); return e;
    }
    if (match(p, TOK_NULL_KW)) { e->kind = EXPR_LITERAL; e->literal.type = TYPE_NULL; return e; }
    TokenType t = peek(p)->type;
    if (t == TOK_COUNT || t == TOK_SUM || t == TOK_AVG || t == TOK_MIN || t == TOK_MAX) {
        e->kind = EXPR_AGG; e->agg = t; advance(p);
        expect(p, TOK_LPAREN, "expected '(' after aggregate");
        if (match(p, TOK_STAR)) e->agg_star = 1;
        else { if (match(p, TOK_DISTINCT)) e->distinct = 1; e->left = parse_expr(p); }
        expect(p, TOK_RPAREN, "expected ')'");
        return e;
    }
    /* scalar function call: IDENT '(' args ')' */
    if (check(p, TOK_IDENT) && p->tl->items[p->pos + 1].type == TOK_LPAREN) {
        e->kind = EXPR_FUNC; e->fname = xstrdup(advance(p)->text);
        advance(p);  /* '(' */
        if (!check(p, TOK_RPAREN)) do {
            e->args = xrealloc(e->args, (e->nargs + 1) * sizeof(Expr *));
            e->args[e->nargs++] = parse_expr(p);
        } while (match(p, TOK_COMMA));
        expect(p, TOK_RPAREN, "expected ')'");
        return e;
    }
    if (check(p, TOK_IDENT)) { e->kind = EXPR_COLUMN; e->col = parse_colref(p); return e; }
    perr(p, "expected a value or column"); free(e); return NULL;
}

static Expr *mk(ExprKind k) { Expr *e = xcalloc(1, sizeof(Expr)); e->kind = k; return e; }
static Expr *make_not(Expr *inner) { Expr *e = mk(EXPR_UNARY); e->op = TOK_NOT; e->left = inner; return e; }

/* unary = '-' unary | primary */
static Expr *parse_unary(Parser *p) {
    if (match(p, TOK_MINUS)) {
        if (++p->depth > PARSE_MAX_DEPTH) { perr(p, "expression nested too deeply"); return NULL; }
        Expr *e = mk(EXPR_UNARY); e->op = TOK_MINUS; e->left = parse_unary(p); p->depth--; return e;
    }
    return parse_primary(p);
}
/* mul = unary (('*' | '/') unary)* */
static Expr *parse_mul(Parser *p) {
    Expr *l = parse_unary(p);
    while (check(p, TOK_STAR) || check(p, TOK_SLASH)) {
        Expr *e = mk(EXPR_BINARY); e->op = advance(p)->type; e->left = l; e->right = parse_unary(p); l = e;
    }
    return l;
}
/* add = mul (('+' | '-') mul)* */
static Expr *parse_add(Parser *p) {
    Expr *l = parse_mul(p);
    while (check(p, TOK_PLUS) || check(p, TOK_MINUS)) {
        Expr *e = mk(EXPR_BINARY); e->op = advance(p)->type; e->left = l; e->right = parse_mul(p); l = e;
    }
    return l;
}
/* comparison = add [ compop add | IS [NOT] NULL | [NOT] IN(...) | [NOT] BETWEEN | [NOT] LIKE ] */
static Expr *parse_comparison(Parser *p) {
    Expr *left = parse_add(p);
    if (match(p, TOK_IS)) {
        int neg = match(p, TOK_NOT);
        expect(p, TOK_NULL_KW, "expected NULL after IS");
        Expr *e = mk(EXPR_ISNULL); e->left = left; e->negated = neg; return e;
    }
    int neg = 0;
    if (check(p, TOK_NOT)) { advance(p); neg = 1; }   /* NOT IN / NOT BETWEEN / NOT LIKE */
    TokenType t = peek(p)->type;
    if (t == TOK_IN) {
        advance(p); expect(p, TOK_LPAREN, "expected '(' after IN");
        Expr *e = mk(EXPR_IN); e->left = left;
        if (!check(p, TOK_RPAREN)) do {
            e->args = xrealloc(e->args, (e->nargs + 1) * sizeof(Expr *)); e->args[e->nargs++] = parse_expr(p);
        } while (match(p, TOK_COMMA));
        expect(p, TOK_RPAREN, "expected ')'");
        return neg ? make_not(e) : e;
    }
    if (t == TOK_BETWEEN) {
        advance(p);
        Expr *e = mk(EXPR_BETWEEN); e->left = left;
        e->lo = parse_add(p); expect(p, TOK_AND, "expected AND in BETWEEN"); e->hi = parse_add(p);
        return neg ? make_not(e) : e;
    }
    if (t == TOK_LIKE) {
        advance(p);
        Expr *e = mk(EXPR_BINARY); e->op = TOK_LIKE; e->left = left; e->right = parse_add(p);
        return neg ? make_not(e) : e;
    }
    if (neg) { perr(p, "expected IN, BETWEEN or LIKE after NOT"); return left; }
    if (t == TOK_EQ || t == TOK_NE || t == TOK_LT || t == TOK_GT || t == TOK_LE || t == TOK_GE) {
        Expr *e = mk(EXPR_BINARY); e->op = advance(p)->type; e->left = left; e->right = parse_add(p); return e;
    }
    return left;
}
/* not = NOT not | comparison */
static Expr *parse_not(Parser *p) {
    if (match(p, TOK_NOT)) {
        if (++p->depth > PARSE_MAX_DEPTH) { perr(p, "expression nested too deeply"); return NULL; }
        Expr *e = make_not(parse_not(p)); p->depth--; return e;
    }
    return parse_comparison(p);
}
/* and = not (AND not)* */
static Expr *parse_and(Parser *p) {
    Expr *left = parse_not(p);
    while (match(p, TOK_AND)) { Expr *e = mk(EXPR_BINARY); e->op = TOK_AND; e->left = left; e->right = parse_not(p); left = e; }
    return left;
}
/* expr = and (OR and)* — the single entry point for every subexpression
 * (parens, function args, aggregate operands, IN-list elements, ...), so the
 * recursion-depth guard here covers ALL nesting paths, not just parens. */
static Expr *parse_expr(Parser *p) {
    if (++p->depth > PARSE_MAX_DEPTH) { perr(p, "expression nested too deeply"); p->depth--; return NULL; }
    Expr *left = parse_and(p);
    while (match(p, TOK_OR)) { Expr *e = mk(EXPR_BINARY); e->op = TOK_OR; e->left = left; e->right = parse_and(p); left = e; }
    p->depth--;
    return left;
}

/* one SELECT item: '*' | expr [AS alias] */
static SelItem parse_sel_item(Parser *p) {
    SelItem it = {0};
    if (check(p, TOK_STAR)) { advance(p); it.kind = SEL_STAR; return it; }
    it.kind = SEL_EXPR; it.expr = parse_expr(p);
    if (match(p, TOK_AS)) {
        if (check(p, TOK_IDENT)) it.alias = xstrdup(advance(p)->text);
        else perr(p, "expected alias after AS");
    }
    return it;
}

/* table_ref = ident [AS ident | ident] ; fills *table and *alias */
static void parse_table_ref(Parser *p, char **table, char **alias) {
    if (!check(p, TOK_IDENT)) { perr(p, "expected a table name"); return; }
    *table = xstrdup(advance(p)->text);
    *alias = NULL;
    if (match(p, TOK_AS)) {
        if (check(p, TOK_IDENT)) *alias = xstrdup(advance(p)->text);
        else perr(p, "expected alias after AS");
    } else if (check(p, TOK_IDENT)) {
        *alias = xstrdup(advance(p)->text);
    }
}

static SelectStmt *parse_select(Parser *p) {
    SelectStmt *s = xcalloc(1, sizeof(SelectStmt));
    s->limit = -1;

    expect(p, TOK_SELECT, "expected SELECT");
    if (match(p, TOK_DISTINCT)) s->distinct = 1;

    /* select list */
    do {
        s->items = xrealloc(s->items, (s->nitems + 1) * sizeof(SelItem));
        s->items[s->nitems++] = parse_sel_item(p);
    } while (match(p, TOK_COMMA));

    expect(p, TOK_FROM, "expected FROM");
    parse_table_ref(p, &s->from_table, &s->from_alias);

    /* joins */
    while (check(p, TOK_JOIN) || check(p, TOK_INNER) || check(p, TOK_LEFT)) {
        JoinClause j = {0};
        j.type = JOIN_INNER;
        if (match(p, TOK_LEFT))  { j.type = JOIN_LEFT; }
        else match(p, TOK_INNER);
        expect(p, TOK_JOIN, "expected JOIN");
        parse_table_ref(p, &j.table, &j.alias);
        expect(p, TOK_ON, "expected ON");
        j.on = parse_expr(p);
        s->joins = xrealloc(s->joins, (s->njoins + 1) * sizeof(JoinClause));
        s->joins[s->njoins++] = j;
    }

    if (match(p, TOK_WHERE)) s->where = parse_expr(p);

    if (match(p, TOK_GROUP)) {
        expect(p, TOK_BY, "expected BY after GROUP");
        do {
            s->group = xrealloc(s->group, (s->ngroup + 1) * sizeof(ColRef));
            s->group[s->ngroup++] = parse_colref(p);
        } while (match(p, TOK_COMMA));
        if (match(p, TOK_HAVING)) s->having = parse_expr(p);
    }

    if (match(p, TOK_ORDER)) {
        expect(p, TOK_BY, "expected BY after ORDER");
        do {
            OrderItem o = {0}; o.pos = -1;
            if (check(p, TOK_INT)) o.pos = strtol(advance(p)->text, NULL, 10);  /* positional */
            else { o.expr = mk(EXPR_COLUMN); o.expr->col = parse_colref(p); }
            if (match(p, TOK_DESC)) o.desc = 1; else match(p, TOK_ASC);
            s->order = xrealloc(s->order, (s->norder + 1) * sizeof(OrderItem));
            s->order[s->norder++] = o;
        } while (match(p, TOK_COMMA));
    }

    if (match(p, TOK_LIMIT)) {
        if (check(p, TOK_INT)) s->limit = strtol(advance(p)->text, NULL, 10);
        else perr(p, "expected a number after LIMIT");
    }

    expect(p, TOK_EOF, "unexpected trailing tokens");
    return s;
}

/* --- freeing the AST --- */
static void free_colref(ColRef *c) { free(c->table); free(c->column); }
static void free_expr(Expr *e) {
    if (!e) return;
    if (e->kind == EXPR_COLUMN) free_colref(&e->col);
    if (e->kind == EXPR_LITERAL && e->literal.type == TYPE_STRING) free(e->literal.as.str_val);
    free(e->fname);
    for (size_t i = 0; i < e->nargs; i++) free_expr(e->args[i]);
    free(e->args);
    free_expr(e->left); free_expr(e->right); free_expr(e->lo); free_expr(e->hi);
    free(e);
}
static void free_select(SelectStmt *s) {
    if (!s) return;
    for (size_t i = 0; i < s->nitems; i++) { free_expr(s->items[i].expr); free(s->items[i].alias); }
    free(s->items);
    free(s->from_table); free(s->from_alias);
    for (size_t i = 0; i < s->njoins; i++) { free(s->joins[i].table); free(s->joins[i].alias); free_expr(s->joins[i].on); }
    free(s->joins);
    free_expr(s->where);
    for (size_t i = 0; i < s->ngroup; i++) free_colref(&s->group[i]);
    free(s->group);
    free_expr(s->having);
    for (size_t i = 0; i < s->norder; i++) free_expr(s->order[i].expr);
    free(s->order);
    free(s);
}

/* Parse a SQL string into a SelectStmt (NULL on error). */
static SelectStmt *parse_sql(const char *sql) {
    TokenList tl = tokenize(sql);
    Parser p = { &tl, 0, 0, 0 };
    SelectStmt *s = parse_select(&p);
    tl_free(&tl);
    if (p.error) { free_select(s); return NULL; }
    return s;
}

/* --- debug printer for the AST --- */
static void print_colref(const ColRef *c) {
    if (c->table) printf("%s.", c->table);
    printf("%s", c->column ? c->column : "?");
}
static void print_expr(const Expr *e) {
    if (!e) { printf("(null)"); return; }
    switch (e->kind) {
        case EXPR_COLUMN: print_colref(&e->col); break;
        case EXPR_LITERAL: {
            char b[128]; value_str(&e->literal, b, sizeof b);
            if (e->literal.type == TYPE_STRING) printf("'%s'", b); else printf("%s", b);
            break;
        }
        case EXPR_BINARY:
            printf("("); print_expr(e->left);
            printf(" %s ", tok_name(e->op));
            print_expr(e->right); printf(")");
            break;
        case EXPR_AGG:
            printf("%s(", tok_name(e->agg));
            if (e->agg_star) printf("*");
            else { if (e->distinct) printf("DISTINCT "); print_expr(e->left); }
            printf(")");
            break;
        case EXPR_UNARY:
            printf("%s", e->op == TOK_MINUS ? "-" : "NOT ");
            print_expr(e->left); break;
        case EXPR_FUNC:
            printf("%s(", e->fname);
            for (size_t i = 0; i < e->nargs; i++) { if (i) printf(", "); print_expr(e->args[i]); }
            printf(")"); break;
        case EXPR_IN:
            print_expr(e->left); printf(" IN (");
            for (size_t i = 0; i < e->nargs; i++) { if (i) printf(", "); print_expr(e->args[i]); }
            printf(")"); break;
        case EXPR_ISNULL:
            print_expr(e->left); printf(e->negated ? " IS NOT NULL" : " IS NULL"); break;
        case EXPR_BETWEEN:
            print_expr(e->left); printf(" BETWEEN "); print_expr(e->lo);
            printf(" AND "); print_expr(e->hi); break;
    }
}
static void print_ast(const SelectStmt *s) {
    printf("SELECT %s", s->distinct ? "DISTINCT " : "");
    for (size_t i = 0; i < s->nitems; i++) {
        if (i) printf(", ");
        SelItem *it = &s->items[i];
        if (it->kind == SEL_STAR) printf("*");
        else print_expr(it->expr);
        if (it->alias) printf(" AS %s", it->alias);
    }
    printf("\n  FROM %s", s->from_table);
    if (s->from_alias) printf(" %s", s->from_alias);
    for (size_t i = 0; i < s->njoins; i++) {
        printf("\n  %s JOIN %s", s->joins[i].type == JOIN_LEFT ? "LEFT" : "INNER", s->joins[i].table);
        if (s->joins[i].alias) printf(" %s", s->joins[i].alias);
        printf(" ON "); print_expr(s->joins[i].on);
    }
    if (s->where) { printf("\n  WHERE "); print_expr(s->where); }
    if (s->ngroup) {
        printf("\n  GROUP BY ");
        for (size_t i = 0; i < s->ngroup; i++) { if (i) printf(", "); print_colref(&s->group[i]); }
    }
    if (s->having) { printf("\n  HAVING "); print_expr(s->having); }
    if (s->norder) {
        printf("\n  ORDER BY ");
        for (size_t i = 0; i < s->norder; i++) {
            if (i) printf(", ");
            if (s->order[i].pos >= 0) printf("%ld", s->order[i].pos); else print_expr(s->order[i].expr);
            printf(s->order[i].desc ? " DESC" : " ASC");
        }
    }
    if (s->limit >= 0) printf("\n  LIMIT %ld", s->limit);
    printf("\n");
}

/* ===========================================================================
 * STEP 4-7 — EXECUTOR: run an AST against loaded tables, producing a result.
 *
 * We execute over an intermediate relation `Rel` (like Table, but each column
 * remembers which table/alias it came from, so qualified refs like a.id work).
 * The logical pipeline: FROM -> JOIN -> WHERE -> GROUP/aggregate -> HAVING ->
 * project (SELECT) -> ORDER BY -> LIMIT.
 * ========================================================================= */
typedef struct { char *qualifier; char *name; ColumnType type; } ColMeta;
typedef struct { ColMeta *cols; size_t ncols; Row *rows; size_t nrows, cap; } Rel;

static Value value_dup(const Value *v) {
    Value o = *v;
    if (v->type == TYPE_STRING) o.as.str_val = xstrdup(v->as.str_val);
    return o;
}
static int is_true(Value v) {
    return (v.type == TYPE_INT   && v.as.int_val   != 0) ||
           (v.type == TYPE_FLOAT && v.as.float_val != 0);
}
/* Compare two values. Returns 0 if incomparable (either NULL, or string vs
 * number); otherwise returns 1 and sets *out to -1/0/1. */
static int value_compare(const Value *a, const Value *b, int *out) {
    if (a->type == TYPE_NULL || b->type == TYPE_NULL) return 0;
    int as = a->type == TYPE_STRING, bs = b->type == TYPE_STRING;
    if (as != bs) return 0;                       /* string vs number */
    if (as) { int c = strcmp(a->as.str_val, b->as.str_val);
              *out = c < 0 ? -1 : (c > 0 ? 1 : 0); return 1; }
    double x = a->type == TYPE_FLOAT ? a->as.float_val : (double)a->as.int_val;
    double y = b->type == TYPE_FLOAT ? b->as.float_val : (double)b->as.int_val;
    *out = x < y ? -1 : (x > y ? 1 : 0); return 1;
}

static void rel_add_row(Rel *r, Row row) {
    if (r->nrows == r->cap) { r->cap = r->cap ? r->cap * 2 : 16;
                              r->rows = xrealloc(r->rows, r->cap * sizeof(Row)); }
    r->rows[r->nrows++] = row;
}
static void rel_free(Rel *r) {
    for (size_t i = 0; i < r->ncols; i++) { free(r->cols[i].qualifier); free(r->cols[i].name); }
    free(r->cols);
    for (size_t i = 0; i < r->nrows; i++) {
        for (size_t j = 0; j < r->rows[i].count; j++)
            if (r->rows[i].values[j].type == TYPE_STRING) free(r->rows[i].values[j].as.str_val);
        free(r->rows[i].values);
    }
    free(r->rows);
    memset(r, 0, sizeof *r);
}
static void copy_schema(Rel *dst, const Rel *src) {
    dst->ncols = src->ncols;
    dst->cols = xmalloc(dst->ncols * sizeof(ColMeta));
    for (size_t i = 0; i < dst->ncols; i++) {
        dst->cols[i].qualifier = src->cols[i].qualifier ? xstrdup(src->cols[i].qualifier) : NULL;
        dst->cols[i].name = xstrdup(src->cols[i].name);
        dst->cols[i].type = src->cols[i].type;
    }
}
/* Find a column by (optional qualifier + name). -1 if absent or ambiguous. */
static int rel_find_col(const Rel *r, const ColRef *c) {
    int found = -1;
    for (size_t i = 0; i < r->ncols; i++) {
        if (strcmp(r->cols[i].name, c->column) != 0) continue;
        if (c->table && (!r->cols[i].qualifier || strcmp(r->cols[i].qualifier, c->table) != 0)) continue;
        if (found >= 0) return -1;                /* ambiguous */
        found = (int)i;
    }
    return found;
}

/* Row-level expression evaluation (WHERE, JOIN ON). Returned Values BORROW any
 * string pointers from the row/expr — never free them. */
/* forward decl: value_hash lives in the hash-join section below */
static unsigned long value_hash(const Value *v);

/* --- string arena: functions like UPPER() produce NEW strings; we stash them
 * here and free them after each row/group is evaluated, so eval() can keep its
 * "returned Values borrow their strings" contract. --- */
static char **g_arena = NULL; static size_t g_arena_n = 0, g_arena_cap = 0;
static char *arena_take(char *s) {
    if (g_arena_n == g_arena_cap) { g_arena_cap = g_arena_cap ? g_arena_cap * 2 : 16;
        g_arena = xrealloc(g_arena, g_arena_cap * sizeof(char *)); }
    g_arena[g_arena_n++] = s; return s;
}
static void arena_reset(void) { for (size_t i = 0; i < g_arena_n; i++) free(g_arena[i]); g_arena_n = 0; }

/* Evaluation context: a single row (row-level), or a group of rows (grouped,
 * so aggregates and HAVING can be computed). */
typedef struct { const Rel *rel; const Row *row; Row **grows; size_t gn; int grouped; } EvalCtx;
static Value eval(const Expr *e, EvalCtx *cx);

/* Case-insensitive SQL LIKE: % = any run, _ = any single char (ASCII).
 * Iterative two-pointer matcher: on mismatch, retry from the most recent '%'
 * with its span extended by one. O(len(s) * len(p)) worst case — a recursive
 * matcher is exponential on patterns like '%a%a%a...%b'. */
static int like_match(const char *s, const char *p) {
    const char *star_p = NULL, *star_s = NULL;
    while (*s) {
        if (*p == '%') { star_p = ++p; star_s = s; }
        else if (*p == '_' ||
                 (*p && tolower((unsigned char)*s) == tolower((unsigned char)*p))) { s++; p++; }
        else if (star_p) { p = star_p; s = ++star_s; }
        else return 0;
    }
    while (*p == '%') p++;
    return *p == '\0';
}

/* Scalar functions: UPPER LOWER LENGTH ABS ROUND COALESCE TRIM. */
static Value eval_func(const Expr *e, EvalCtx *cx) {
    Value v; v.type = TYPE_NULL;
    const char *fn = e->fname;
    if (ci_equal(fn, "coalesce")) {
        for (size_t i = 0; i < e->nargs; i++) { Value a = eval(e->args[i], cx); if (a.type != TYPE_NULL) return a; }
        return v;
    }
    if (e->nargs < 1) return v;
    Value a = eval(e->args[0], cx);
    if (ci_equal(fn, "upper") || ci_equal(fn, "lower")) {
        if (a.type != TYPE_STRING) return a;
        char *o = xstrdup(a.as.str_val); int up = ci_equal(fn, "upper");
        for (char *q = o; *q; q++) *q = up ? toupper((unsigned char)*q) : tolower((unsigned char)*q);
        v.type = TYPE_STRING; v.as.str_val = arena_take(o); return v;
    }
    if (ci_equal(fn, "length")) {
        if (a.type == TYPE_NULL) return v;
        char b[64]; if (a.type != TYPE_STRING) value_str(&a, b, sizeof b);
        v.type = TYPE_INT; v.as.int_val = (long)strlen(a.type == TYPE_STRING ? a.as.str_val : b); return v;
    }
    if (ci_equal(fn, "abs")) {
        if (a.type == TYPE_INT) {
            if (a.as.int_val == LONG_MIN) { v.type = TYPE_FLOAT; v.as.float_val = -(double)LONG_MIN; }
            else { v.type = TYPE_INT; v.as.int_val = a.as.int_val < 0 ? -a.as.int_val : a.as.int_val; }
        }
        if (a.type == TYPE_FLOAT) { v.type = TYPE_FLOAT; v.as.float_val = a.as.float_val < 0 ? -a.as.float_val : a.as.float_val; }
        return v;
    }
    if (ci_equal(fn, "round")) {
        if (a.type != TYPE_INT && a.type != TYPE_FLOAT) return v;  /* strings: strict typing -> NULL */
        double d = a.type == TYPE_FLOAT ? a.as.float_val : (double)a.as.int_val;
        int nd = 0;
        if (e->nargs > 1) {
            Value b = eval(e->args[1], cx);
            if (b.type == TYPE_NULL) return v;                     /* sqlite: NULL digits -> NULL */
            double bd = b.type == TYPE_INT ? (double)b.as.int_val :
                        b.type == TYPE_FLOAT ? b.as.float_val : 0;
            if (bd != bd) bd = 0;                                  /* NaN digits */
            nd = bd < 0 ? 0 : bd > 30 ? 30 : (int)bd;              /* clamp, like sqlite */
        }
        if (d != d) return v;                                      /* computed NaN -> NULL */
        double av = d < 0 ? -d : d;
        if (av > 1.7e308) { v.type = TYPE_FLOAT; v.as.float_val = d; return v; }   /* +-inf */
        if (nd == 0 && av < 9223372036854774784.0) {               /* < 2^63 - 1024: cast is safe */
            double r = (double)(long)(av + 0.5);                   /* sqlite: binary half-away here */
            d = d < 0 ? -r : r;
        } else {
            /* Round on the EXACT decimal expansion — %f prints it in full (a
               double has < 1075 fractional digits, so %.1100f never rounds) —
               truncating at nd digits, half away from zero. This matches
               sqlite's dtoa ROUND: 2.675 (really 2.67499...) -> 2.67, but the
               exact tie 0.125 -> 0.13. Scaling by 10^nd instead would re-round
               through a double multiply and turn 2.675*100 into exactly 267.5. */
            char rb[1536];
            snprintf(rb, sizeof rb, "%.1100f", d);
            char *dot = strchr(rb, '.');
            int start = rb[0] == '-' ? 1 : 0;
            int cut = (int)(dot - rb) + (nd ? nd + 1 : 0);         /* keep the dot only if nd > 0 */
            if (dot[1 + nd] >= '5') {                              /* guard digit >= 5: round away */
                int i;
                for (i = cut - 1; i >= start; i--) {
                    if (rb[i] == '.') continue;
                    if (rb[i] < '9') { rb[i]++; break; }
                    rb[i] = '0';
                }
                if (i < start) {                                   /* carry out of the top digit */
                    memmove(rb + start + 1, rb + start, (size_t)(cut - start));
                    rb[start] = '1'; cut++;
                }
            }
            rb[cut] = '\0';
            d = strtod(rb, NULL);
        }
        if (d == 0) d = 0;                                         /* -0.25 rounds to +0, like sqlite */
        v.type = TYPE_FLOAT; v.as.float_val = d; return v;
    }
    if (ci_equal(fn, "trim")) {
        if (a.type != TYPE_STRING) return a;
        const char *s = a.as.str_val; while (*s == ' ') s++;
        size_t len = strlen(s); while (len && s[len - 1] == ' ') len--;
        char *o = xmalloc(len + 1); memcpy(o, s, len); o[len] = '\0';
        v.type = TYPE_STRING; v.as.str_val = arena_take(o); return v;
    }
    fprintf(stderr, "unknown function: %s\n", fn);
    return v;
}

/* Aggregate of an expression over the group (with optional DISTINCT). */
static Value eval_agg_expr(const Expr *e, EvalCtx *cx) {
    Value out; out.type = TYPE_NULL;
    Row **rows = cx->grows; size_t n = cx->gn;
    if (e->agg == TOK_COUNT && e->agg_star) { out.type = TYPE_INT; out.as.int_val = (long)n; return out; }

    struct DNode { unsigned long h; Value v; struct DNode *next; } **buckets = NULL; size_t nb = 0;
    if (e->distinct) { nb = 16; while (nb < n + 1) nb <<= 1; buckets = xcalloc(nb, sizeof(void *)); }

    /* acc (double) feeds AVG and any float input; iacc keeps integer SUM exact.
       If the integer sum overflows, SUM falls back to the double (as + and * do). */
    double acc = 0; long cnt = 0; int isflt = 0;
    long iacc = 0; int iovf = 0;
    Value best; best.type = TYPE_NULL;   /* MIN/MAX keep the winning Value itself, so strings work */
    EvalCtx sub = *cx; sub.grouped = 0; sub.grows = NULL; sub.gn = 0;
    for (size_t i = 0; i < n; i++) {
        sub.row = rows[i];
        Value val = eval(e->left, &sub);
        if (val.type == TYPE_NULL) continue;
        if (e->distinct) {
            unsigned long h = value_hash(&val); size_t b = h & (nb - 1); int seen = 0;
            for (struct DNode *d = buckets[b]; d; d = d->next) { int c; if (d->h == h && value_compare(&d->v, &val, &c) && c == 0) { seen = 1; break; } }
            if (seen) continue;
            struct DNode *d = xmalloc(sizeof *d); d->h = h; d->v = val; d->next = buckets[b]; buckets[b] = d;
        }
        cnt++;
        if (val.type == TYPE_FLOAT) { isflt = 1; acc += val.as.float_val; }
        else if (val.type == TYPE_INT) {
            acc += (double)val.as.int_val;
            if (!iovf && __builtin_add_overflow(iacc, val.as.int_val, &iacc)) iovf = 1;
        }
        if (e->agg == TOK_MIN || e->agg == TOK_MAX) {
            int c;
            if (best.type == TYPE_NULL) best = val;
            else if (value_compare(&val, &best, &c) && (e->agg == TOK_MIN ? c < 0 : c > 0)) best = val;
        }
    }
    if (e->distinct) { for (size_t b = 0; b < nb; b++) { struct DNode *d = buckets[b]; while (d) { struct DNode *x = d->next; free(d); d = x; } } free(buckets); }

    switch (e->agg) {
        case TOK_COUNT: out.type = TYPE_INT; out.as.int_val = cnt; break;
        /* SUM/AVG of zero (non-NULL) inputs is NULL, not 0 — SQL semantics */
        case TOK_SUM: if (cnt && acc == acc) { if (isflt || iovf) { out.type = TYPE_FLOAT; out.as.float_val = acc; } else { out.type = TYPE_INT; out.as.int_val = iacc; } } break;
        case TOK_AVG: if (cnt && acc == acc) { out.type = TYPE_FLOAT; out.as.float_val = acc / cnt; } break;
        case TOK_MIN: case TOK_MAX: out = best; break;
        default: break;
    }
    return out;
}

/* The core evaluator. Returned Values borrow strings (arena or row); callers
 * dup before the next arena_reset(). */
static Value eval(const Expr *e, EvalCtx *cx) {
    Value v; v.type = TYPE_NULL;
    if (!e) return v;
    switch (e->kind) {
        case EXPR_LITERAL: return e->literal;
        case EXPR_COLUMN: {
            int idx = rel_find_col(cx->rel, &e->col);
            if (idx < 0) return v;
            const Row *row = cx->grouped ? (cx->gn ? cx->grows[0] : NULL) : cx->row;
            return row ? row->values[idx] : v;
        }
        case EXPR_AGG: return cx->grouped ? eval_agg_expr(e, cx) : v;
        case EXPR_FUNC: return eval_func(e, cx);
        case EXPR_UNARY: {
            Value a = eval(e->left, cx);
            if (e->op == TOK_NOT) {
                if (a.type == TYPE_NULL) return v;
                v.type = TYPE_INT; v.as.int_val = !is_true(a); return v;
            }
            if (a.type == TYPE_INT) {
                if (a.as.int_val == LONG_MIN) { v.type = TYPE_FLOAT; v.as.float_val = -(double)LONG_MIN; }
                else { v.type = TYPE_INT; v.as.int_val = -a.as.int_val; }
            }
            if (a.type == TYPE_FLOAT) { v.type = TYPE_FLOAT; v.as.float_val = -a.as.float_val; }
            return v;
        }
        case EXPR_ISNULL: {
            Value a = eval(e->left, cx);
            int isnull = (a.type == TYPE_NULL);
            v.type = TYPE_INT; v.as.int_val = e->negated ? !isnull : isnull; return v;
        }
        case EXPR_IN: {
            Value a = eval(e->left, cx);
            if (a.type == TYPE_NULL) return v;
            int sawnull = 0;
            for (size_t i = 0; i < e->nargs; i++) {
                Value b = eval(e->args[i], cx); int c;
                if (b.type == TYPE_NULL) { sawnull = 1; continue; }
                if (value_compare(&a, &b, &c) && c == 0) { v.type = TYPE_INT; v.as.int_val = 1; return v; }
            }
            if (sawnull) return v;   /* no match but a NULL in the list -> UNKNOWN (SQL 3-valued logic) */
            v.type = TYPE_INT; v.as.int_val = 0; return v;
        }
        case EXPR_BETWEEN: {
            Value a = eval(e->left, cx), lo = eval(e->lo, cx), hi = eval(e->hi, cx);
            int c1, c2;
            if (!value_compare(&a, &lo, &c1) || !value_compare(&a, &hi, &c2)) return v;
            v.type = TYPE_INT; v.as.int_val = (c1 >= 0 && c2 <= 0); return v;
        }
        case EXPR_BINARY: {
            if (e->op == TOK_AND || e->op == TOK_OR) {
                /* SQL three-valued logic: NULL is UNKNOWN, not false. It only
                   collapses when the other side decides the result outright
                   (FALSE AND x, TRUE OR x); otherwise UNKNOWN propagates, so
                   NOT (NULL OR FALSE) stays UNKNOWN instead of becoming TRUE. */
                Value lv = eval(e->left, cx), rv = eval(e->right, cx);
                int l  = lv.type == TYPE_NULL ? -1 : is_true(lv);   /* -1 = UNKNOWN */
                int rr = rv.type == TYPE_NULL ? -1 : is_true(rv);
                if (e->op == TOK_AND) {
                    if (l == 0 || rr == 0) { v.type = TYPE_INT; v.as.int_val = 0; return v; }
                    if (l < 0 || rr < 0) return v;                  /* UNKNOWN */
                    v.type = TYPE_INT; v.as.int_val = 1; return v;
                }
                if (l == 1 || rr == 1) { v.type = TYPE_INT; v.as.int_val = 1; return v; }
                if (l < 0 || rr < 0) return v;                      /* UNKNOWN */
                v.type = TYPE_INT; v.as.int_val = 0; return v;
            }
            Value a = eval(e->left, cx), b = eval(e->right, cx);
            if (e->op == TOK_LIKE) {
                if (a.type != TYPE_STRING || b.type != TYPE_STRING) return v;
                v.type = TYPE_INT; v.as.int_val = like_match(a.as.str_val, b.as.str_val); return v;
            }
            if (e->op == TOK_PLUS || e->op == TOK_MINUS || e->op == TOK_STAR || e->op == TOK_SLASH) {
                if (a.type == TYPE_NULL || b.type == TYPE_NULL) return v;
                if (a.type == TYPE_STRING || b.type == TYPE_STRING) return v;
                int flt = (a.type == TYPE_FLOAT || b.type == TYPE_FLOAT);
                if (flt) {
                    double x = a.type == TYPE_FLOAT ? a.as.float_val : (double)a.as.int_val;
                    double y = b.type == TYPE_FLOAT ? b.as.float_val : (double)b.as.int_val;
                    double r = e->op == TOK_PLUS ? x + y : e->op == TOK_MINUS ? x - y :
                               e->op == TOK_STAR ? x * y : (y != 0 ? x / y : 0);
                    if (e->op == TOK_SLASH && y == 0) return v;
                    if (r != r) return v;              /* inf - inf etc: NaN -> NULL, like sqlite */
                    v.type = TYPE_FLOAT; v.as.float_val = r; return v;
                }
                long x = a.as.int_val, y = b.as.int_val, r;
                if (e->op == TOK_SLASH) {
                    if (y == 0) return v;
                    if (x == LONG_MIN && y == -1) { v.type = TYPE_FLOAT; v.as.float_val = -(double)LONG_MIN; return v; }
                    v.type = TYPE_INT; v.as.int_val = x / y; return v;   /* integer division, like sqlite */
                }
                /* checked add/sub/mul: on overflow, promote to double (as sqlite does) */
                int ovf = e->op == TOK_PLUS  ? __builtin_add_overflow(x, y, &r) :
                          e->op == TOK_MINUS ? __builtin_sub_overflow(x, y, &r) :
                                               __builtin_mul_overflow(x, y, &r);
                if (ovf) {
                    double dx = (double)x, dy = (double)y;
                    v.type = TYPE_FLOAT;
                    v.as.float_val = e->op == TOK_PLUS ? dx + dy : e->op == TOK_MINUS ? dx - dy : dx * dy;
                    return v;
                }
                v.type = TYPE_INT; v.as.int_val = r; return v;
            }
            int cmp; if (!value_compare(&a, &b, &cmp)) return v;
            int res = 0;
            switch (e->op) {
                case TOK_EQ: res = cmp == 0; break; case TOK_NE: res = cmp != 0; break;
                case TOK_LT: res = cmp <  0; break; case TOK_GT: res = cmp >  0; break;
                case TOK_LE: res = cmp <= 0; break; case TOK_GE: res = cmp >= 0; break;
                default: break;
            }
            v.type = TYPE_INT; v.as.int_val = res; return v;
        }
    }
    return v;
}

/* thin wrappers preserving the old call-site signatures */
static Value eval_expr(const Expr *e, const Rel *r, const Row *row) {
    EvalCtx cx = { r, row, NULL, 0, 0 }; return eval(e, &cx);
}
static Value eval_group(const Expr *e, const Rel *r, Row **rows, size_t nrows) {
    EvalCtx cx = { r, nrows ? rows[0] : NULL, rows, nrows, 1 }; return eval(e, &cx);
}

/* --- FROM: copy a stored Table into a Rel, tagging columns with a qualifier. */
static Rel rel_from_table(const Table *t, const char *qual) {
    Rel r = {0};
    r.ncols = t->col_count;
    r.cols = xmalloc(r.ncols * sizeof(ColMeta));
    for (size_t i = 0; i < r.ncols; i++) {
        r.cols[i].qualifier = qual ? xstrdup(qual) : NULL;
        r.cols[i].name = xstrdup(t->col_names[i]);
        r.cols[i].type = t->col_types[i];
    }
    for (size_t i = 0; i < t->row_count; i++) {
        Row row; row.count = t->col_count; row.values = xmalloc(row.count * sizeof(Value));
        for (size_t j = 0; j < row.count; j++) row.values[j] = value_dup(&t->rows[i].values[j]);
        rel_add_row(&r, row);
    }
    return r;
}

/* Like rel_from_table, but only the given `rowidx` rows (an index lookup gave
 * us these) — so we copy just the matches instead of the whole table. */
static Rel rel_from_table_rows(const Table *t, const char *qual, const size_t *rowidx, size_t n) {
    Rel r = {0};
    r.ncols = t->col_count;
    r.cols = xmalloc(r.ncols * sizeof(ColMeta));
    for (size_t i = 0; i < r.ncols; i++) {
        r.cols[i].qualifier = qual ? xstrdup(qual) : NULL;
        r.cols[i].name = xstrdup(t->col_names[i]);
        r.cols[i].type = t->col_types[i];
    }
    for (size_t k = 0; k < n; k++) {
        size_t i = rowidx[k];
        Row row; row.count = t->col_count; row.values = xmalloc(row.count * sizeof(Value));
        for (size_t j = 0; j < row.count; j++) row.values[j] = value_dup(&t->rows[i].values[j]);
        rel_add_row(&r, row);
    }
    return r;
}

/* --- JOIN: nested-loop inner/left join of two relations on a predicate. */
static Rel join_rel(const Rel *L, const Rel *R, const Expr *on, JoinType type) {
    Rel out = {0};
    out.ncols = L->ncols + R->ncols;
    out.cols = xmalloc(out.ncols * sizeof(ColMeta));
    for (size_t i = 0; i < L->ncols; i++) {
        out.cols[i].qualifier = L->cols[i].qualifier ? xstrdup(L->cols[i].qualifier) : NULL;
        out.cols[i].name = xstrdup(L->cols[i].name); out.cols[i].type = L->cols[i].type;
    }
    for (size_t i = 0; i < R->ncols; i++) {
        size_t o = L->ncols + i;
        out.cols[o].qualifier = R->cols[i].qualifier ? xstrdup(R->cols[i].qualifier) : NULL;
        out.cols[o].name = xstrdup(R->cols[i].name); out.cols[o].type = R->cols[i].type;
    }
    Row tmp; tmp.count = out.ncols; tmp.values = xmalloc(out.ncols * sizeof(Value));
    for (size_t li = 0; li < L->nrows; li++) {
        int matched = 0;
        for (size_t k = 0; k < L->ncols; k++) tmp.values[k] = L->rows[li].values[k];
        for (size_t ri = 0; ri < R->nrows; ri++) {
            for (size_t k = 0; k < R->ncols; k++) tmp.values[L->ncols + k] = R->rows[ri].values[k];
            int hit = is_true(eval_expr(on, &out, &tmp));
            arena_reset();   /* ON may call string functions; don't hoard per-pair */
            if (hit) {
                Row row; row.count = out.ncols; row.values = xmalloc(out.ncols * sizeof(Value));
                for (size_t k = 0; k < out.ncols; k++) row.values[k] = value_dup(&tmp.values[k]);
                rel_add_row(&out, row); matched = 1;
            }
        }
        if (type == JOIN_LEFT && !matched) {
            Row row; row.count = out.ncols; row.values = xmalloc(out.ncols * sizeof(Value));
            for (size_t k = 0; k < L->ncols; k++) row.values[k] = value_dup(&L->rows[li].values[k]);
            for (size_t k = 0; k < R->ncols; k++) row.values[L->ncols + k].type = TYPE_NULL;
            rel_add_row(&out, row);
        }
    }
    free(tmp.values);
    return out;
}

/* --- HASH JOIN: O(n+m) equijoin. Build a hash table on the right relation's
 * key column, then probe it with each left row. Only usable when the ON clause
 * is a single equality between a left column and a right column of equal type
 * (checked by equijoin_keys); otherwise we fall back to nested-loop. */
static unsigned long value_hash(const Value *v) {
    if (v->type == TYPE_STRING) {                 /* djb2 */
        unsigned long h = 5381; const unsigned char *s = (const unsigned char *)v->as.str_val;
        while (*s) h = ((h << 5) + h) + *s++;
        return h;
    }
    /* Hash int and float through the SAME canonical form (double) so that
       numerically-equal values — which value_compare() treats as equal, e.g.
       int 5 and float 5.0 — always land in the same bucket. */
    if (v->type == TYPE_INT || v->type == TYPE_FLOAT) {
        double d = v->type == TYPE_FLOAT ? v->as.float_val : (double)v->as.int_val;
        unsigned long u = 0;
        if (d != d) u = 0x7ff8000000000000UL;       /* all NaNs hash alike (they compare equal) */
        else if (d != 0) memcpy(&u, &d, sizeof d);  /* ±0.0 both keep u = 0 */
        return u * 1099511628211UL;
    }
    return 0;
}

/* ===========================================================================
 * SECONDARY INDEXES (experiment): two designs on a single column.
 *   HASH   — value -> bucket of row indices. O(1) average equality lookup.
 *            Cannot answer range queries (no order).
 *   SORTED — row indices sorted by the column's value. O(log n) equality AND
 *            range (<, >, <=, >=) via binary search. Costs a sort to build.
 * Both are secondary indexes over an existing in-memory Table (no persistence).
 * ========================================================================= */
typedef enum { IDX_HASH, IDX_SORTED } IndexKind;
typedef struct IdxNode { unsigned long h; size_t row; struct IdxNode *next; } IdxNode;
typedef struct Index {
    int        col;
    IndexKind  kind;
    /* HASH */
    IdxNode  **buckets;
    size_t     nbuckets;
    IdxNode   *pool;          /* one node per row, contiguous */
    /* SORTED */
    size_t    *sorted;        /* row indices, ascending by column value */
    size_t     nsorted;
} Index;

/* table_free routes here (declared up by the Table definition). */
static void indexes_free(Index *arr, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(arr[i].buckets); free(arr[i].pool); free(arr[i].sorted);
    }
    free(arr);
}

/* --- sorting a column's row indices (for SORTED, and shared compare) --- */
static const Table *g_ix_tbl; static int g_ix_col;
static int idx_row_cmp(const void *pa, const void *pb) {
    size_t ra = *(const size_t *)pa, rb = *(const size_t *)pb;
    const Value *a = &g_ix_tbl->rows[ra].values[g_ix_col];
    const Value *b = &g_ix_tbl->rows[rb].values[g_ix_col];
    if (a->type == TYPE_NULL && b->type == TYPE_NULL) return 0;
    if (a->type == TYPE_NULL) return -1;
    if (b->type == TYPE_NULL) return 1;
    int c; return value_compare(a, b, &c) ? c : 0;
}

/* Build (or rebuild) an index of `kind` on column `col` of table `t`. */
static void table_add_index(Table *t, int col, IndexKind kind) {
    /* replace if one of the same (col,kind) already exists */
    for (size_t i = 0; i < t->nindex; i++)
        if (t->indexes[i].col == col && t->indexes[i].kind == kind) return;

    t->indexes = xrealloc(t->indexes, (t->nindex + 1) * sizeof(Index));
    Index *ix = &t->indexes[t->nindex++];
    memset(ix, 0, sizeof *ix);
    ix->col = col; ix->kind = kind;

    if (kind == IDX_HASH) {
        size_t nb = 16; while (nb < t->row_count) nb <<= 1;
        ix->nbuckets = nb;
        ix->buckets = xcalloc(nb, sizeof(IdxNode *));
        ix->pool = t->row_count ? xmalloc(t->row_count * sizeof(IdxNode)) : NULL;
        for (size_t r = t->row_count; r-- > 0; ) {   /* descending -> ascending chains */
            Value *v = &t->rows[r].values[col];
            if (v->type == TYPE_NULL) continue;
            unsigned long h = value_hash(v); size_t b = h & (nb - 1);
            ix->pool[r].h = h; ix->pool[r].row = r;
            ix->pool[r].next = ix->buckets[b]; ix->buckets[b] = &ix->pool[r];
        }
    } else {   /* IDX_SORTED */
        ix->sorted = xmalloc((t->row_count ? t->row_count : 1) * sizeof(size_t));
        ix->nsorted = 0;
        for (size_t r = 0; r < t->row_count; r++)
            if (t->rows[r].values[col].type != TYPE_NULL) ix->sorted[ix->nsorted++] = r;
        g_ix_tbl = t; g_ix_col = col;
        qsort(ix->sorted, ix->nsorted, sizeof(size_t), idx_row_cmp);
    }
}

static Index *table_find_index(Table *t, int col, int want_range) {
    /* range queries need a SORTED index; equality can use either (prefer hash) */
    Index *hash = NULL, *sorted = NULL;
    for (size_t i = 0; i < t->nindex; i++) {
        if (t->indexes[i].col != col) continue;
        if (t->indexes[i].kind == IDX_HASH)   hash = &t->indexes[i];
        if (t->indexes[i].kind == IDX_SORTED) sorted = &t->indexes[i];
    }
    if (want_range) return sorted;
    return hash ? hash : sorted;
}

/* Lookup rows where column `col` OP `key`. Fills *out (malloc'd) with matching
 * row indices, returns the count. op is a comparison TokenType. */
static size_t index_lookup(Table *t, Index *ix, TokenType op, const Value *key, size_t **out) {
    size_t cap = 16, n = 0; size_t *res = xmalloc(cap * sizeof(size_t));
    #define PUSH(R) do { if (n==cap){cap*=2;res=xrealloc(res,cap*sizeof(size_t));} res[n++]=(R); } while(0)

    if (ix->kind == IDX_HASH) {                 /* equality only */
        unsigned long h = value_hash(key); size_t b = h & (ix->nbuckets - 1);
        for (IdxNode *nd = ix->buckets[b]; nd; nd = nd->next) {
            if (nd->h != h) continue;
            int c; Value *v = &t->rows[nd->row].values[ix->col];
            if (value_compare(v, key, &c) && c == 0) PUSH(nd->row);
        }
    } else {                                    /* SORTED: binary search a range */
        /* find first index whose value >= key (lower bound) */
        size_t lo = 0, hi = ix->nsorted;
        while (lo < hi) { size_t mid = (lo + hi) / 2;
            Value *v = &t->rows[ix->sorted[mid]].values[ix->col];
            int c; int cmp = value_compare(v, key, &c) ? c : 0;
            if (cmp < 0) lo = mid + 1; else hi = mid; }
        size_t lb = lo;
        /* upper bound: first index whose value > key */
        lo = 0; hi = ix->nsorted;
        while (lo < hi) { size_t mid = (lo + hi) / 2;
            Value *v = &t->rows[ix->sorted[mid]].values[ix->col];
            int c; int cmp = value_compare(v, key, &c) ? c : 0;
            if (cmp <= 0) lo = mid + 1; else hi = mid; }
        size_t ub = lo;
        size_t start, end;
        switch (op) {
            case TOK_EQ: start = lb; end = ub; break;
            case TOK_LT: start = 0;  end = lb; break;
            case TOK_LE: start = 0;  end = ub; break;
            case TOK_GT: start = ub; end = ix->nsorted; break;
            case TOK_GE: start = lb; end = ix->nsorted; break;
            default:     start = 0;  end = 0; break;
        }
        for (size_t i = start; i < end; i++) PUSH(ix->sorted[i]);
    }
    #undef PUSH
    *out = res; return n;
}

/* If `on` is `Lcol = Rcol` (in either order) with matching column types, fill
 * the left/right key indices and return 1. Otherwise return 0. */
static int equijoin_keys(const Expr *on, const Rel *L, const Rel *R, int *lk, int *rk) {
    if (!on || on->kind != EXPR_BINARY || on->op != TOK_EQ) return 0;
    if (on->left->kind != EXPR_COLUMN || on->right->kind != EXPR_COLUMN) return 0;
    int l1 = rel_find_col(L, &on->left->col),  r1 = rel_find_col(R, &on->right->col);
    int l2 = rel_find_col(L, &on->right->col), r2 = rel_find_col(R, &on->left->col);
    if (l1 >= 0 && r1 >= 0)      { *lk = l1; *rk = r1; }
    else if (l2 >= 0 && r2 >= 0) { *lk = l2; *rk = r2; }
    else return 0;
    if (L->cols[*lk].type != R->cols[*rk].type) return 0;   /* type mismatch: use nested-loop */
    return 1;
}

typedef struct HNode { unsigned long h; size_t idx; struct HNode *next; } HNode;

static Rel join_hash(const Rel *L, const Rel *R, int lk, int rk, JoinType type) {
    Rel out = {0};
    out.ncols = L->ncols + R->ncols;
    out.cols = xmalloc(out.ncols * sizeof(ColMeta));
    for (size_t i = 0; i < L->ncols; i++) {
        out.cols[i].qualifier = L->cols[i].qualifier ? xstrdup(L->cols[i].qualifier) : NULL;
        out.cols[i].name = xstrdup(L->cols[i].name); out.cols[i].type = L->cols[i].type;
    }
    for (size_t i = 0; i < R->ncols; i++) {
        size_t o = L->ncols + i;
        out.cols[o].qualifier = R->cols[i].qualifier ? xstrdup(R->cols[i].qualifier) : NULL;
        out.cols[o].name = xstrdup(R->cols[i].name); out.cols[o].type = R->cols[i].type;
    }

    /* Build phase: hash every right row by its key (NULL keys never match). */
    size_t nb = 16; while (nb < R->nrows) nb <<= 1;
    HNode **buckets = xcalloc(nb, sizeof(HNode *));
    HNode *pool = R->nrows ? xmalloc(R->nrows * sizeof(HNode)) : NULL;
    /* insert descending so each bucket chain ends up ascending by row index,
       matching nested-loop's output order for identical results */
    for (size_t ri = R->nrows; ri-- > 0; ) {
        Value *k = &R->rows[ri].values[rk];
        if (k->type == TYPE_NULL) continue;
        unsigned long h = value_hash(k);
        size_t b = h & (nb - 1);
        pool[ri].h = h; pool[ri].idx = ri; pool[ri].next = buckets[b]; buckets[b] = &pool[ri];
    }

    /* Probe phase: for each left row, look up matches in O(1) average. */
    Row tmp; tmp.count = out.ncols; tmp.values = xmalloc(out.ncols * sizeof(Value));
    for (size_t li = 0; li < L->nrows; li++) {
        for (size_t k = 0; k < L->ncols; k++) tmp.values[k] = L->rows[li].values[k];
        int matched = 0;
        Value *key = &L->rows[li].values[lk];
        if (key->type != TYPE_NULL) {
            unsigned long h = value_hash(key); size_t b = h & (nb - 1);
            for (HNode *n = buckets[b]; n; n = n->next) {
                if (n->h != h) continue;
                int cmp; Value *rkey = &R->rows[n->idx].values[rk];
                if (!value_compare(key, rkey, &cmp) || cmp != 0) continue;
                for (size_t k = 0; k < R->ncols; k++) tmp.values[L->ncols + k] = R->rows[n->idx].values[k];
                Row row; row.count = out.ncols; row.values = xmalloc(out.ncols * sizeof(Value));
                for (size_t k = 0; k < out.ncols; k++) row.values[k] = value_dup(&tmp.values[k]);
                rel_add_row(&out, row); matched = 1;
            }
        }
        if (type == JOIN_LEFT && !matched) {
            Row row; row.count = out.ncols; row.values = xmalloc(out.ncols * sizeof(Value));
            for (size_t k = 0; k < L->ncols; k++) row.values[k] = value_dup(&L->rows[li].values[k]);
            for (size_t k = 0; k < R->ncols; k++) row.values[L->ncols + k].type = TYPE_NULL;
            rel_add_row(&out, row);
        }
    }
    free(tmp.values); free(pool); free(buckets);
    return out;
}

/* Join-algorithm policy: 0=auto (hash when possible), 1=force nested, 2=force hash. */
static int g_join_mode = 0;
static int g_auto_index = 0;   /* when on, build an index on a base filter column on first use */
static int g_used_index = 0;   /* set per-query if an index served the base scan (for EXPLAIN/report) */
static int g_timer = 0;        /* .timer: print per-query execution time */

/* Pick and run the best join algorithm; report which via *algo. */
static Rel join_dispatch(const Rel *L, const Rel *R, const Expr *on, JoinType type, const char **algo) {
    int lk = 0, rk = 0;
    int equi = equijoin_keys(on, L, R, &lk, &rk);
    int use_hash = (g_join_mode == 1) ? 0 : equi;   /* auto & force-hash both need an equijoin */
    if (use_hash) { if (algo) *algo = "hash"; return join_hash(L, R, lk, rk, type); }
    if (algo) *algo = "nested-loop";
    return join_rel(L, R, on, type);
}

static Rel rel_schema_only(const Table *t, const char *qual);   /* defined with EXPLAIN */

/* --- INDEX JOIN: join left Rel `L` against a base table `rt` that has a hash
 * index `ix` on its join column `rk`. For each left row we probe the index and
 * emit only the matching right rows — WITHOUT copying the whole right table.
 * A big win when L is small (e.g. after a selective filter) and rt is large. */
static Rel join_index(const Rel *L, const Table *rt, const char *rq,
                      int lk, int rk, const Index *ix, JoinType type) {
    Rel out = {0};
    out.ncols = L->ncols + rt->col_count;
    out.cols = xmalloc(out.ncols * sizeof(ColMeta));
    for (size_t i = 0; i < L->ncols; i++) {
        out.cols[i].qualifier = L->cols[i].qualifier ? xstrdup(L->cols[i].qualifier) : NULL;
        out.cols[i].name = xstrdup(L->cols[i].name); out.cols[i].type = L->cols[i].type;
    }
    for (size_t i = 0; i < rt->col_count; i++) { size_t o = L->ncols + i;
        out.cols[o].qualifier = rq ? xstrdup(rq) : NULL;
        out.cols[o].name = xstrdup(rt->col_names[i]); out.cols[o].type = rt->col_types[i];
    }

    Row tmp; tmp.count = out.ncols; tmp.values = xmalloc(out.ncols * sizeof(Value));
    for (size_t li = 0; li < L->nrows; li++) {
        for (size_t k = 0; k < L->ncols; k++) tmp.values[k] = L->rows[li].values[k];
        int matched = 0;
        Value *key = &L->rows[li].values[lk];
        if (key->type != TYPE_NULL) {
            unsigned long h = value_hash(key); size_t b = h & (ix->nbuckets - 1);
            for (IdxNode *n = ix->buckets[b]; n; n = n->next) {
                if (n->h != h) continue;
                int cmp; Value *rkey = &rt->rows[n->row].values[rk];
                if (!value_compare(key, rkey, &cmp) || cmp != 0) continue;
                for (size_t k = 0; k < rt->col_count; k++) tmp.values[L->ncols + k] = rt->rows[n->row].values[k];
                Row row; row.count = out.ncols; row.values = xmalloc(out.ncols * sizeof(Value));
                for (size_t k = 0; k < out.ncols; k++) row.values[k] = value_dup(&tmp.values[k]);
                rel_add_row(&out, row); matched = 1;
            }
        }
        if (type == JOIN_LEFT && !matched) {
            Row row; row.count = out.ncols; row.values = xmalloc(out.ncols * sizeof(Value));
            for (size_t k = 0; k < L->ncols; k++) row.values[k] = value_dup(&L->rows[li].values[k]);
            for (size_t k = 0; k < rt->col_count; k++) row.values[L->ncols + k].type = TYPE_NULL;
            rel_add_row(&out, row);
        }
    }
    free(tmp.values);
    return out;
}

/* --- WHERE: keep rows for which the predicate is true. */
static Rel filter_rel(const Rel *in, const Expr *where) {
    Rel out = {0}; copy_schema(&out, in);
    for (size_t i = 0; i < in->nrows; i++) {
        int keep = is_true(eval_expr(where, in, &in->rows[i]));
        arena_reset();
        if (keep) {
            Row row; row.count = in->ncols; row.values = xmalloc(row.count * sizeof(Value));
            for (size_t j = 0; j < row.count; j++) row.values[j] = value_dup(&in->rows[i].values[j]);
            rel_add_row(&out, row);
        }
    }
    return out;
}

/* Output column name for a SELECT item (alias > column name > function/agg name). */
static const char *sel_name(const SelItem *it) {
    if (it->alias) return it->alias;
    const Expr *e = it->expr;
    if (e->kind == EXPR_COLUMN) return e->col.column;
    if (e->kind == EXPR_AGG)    return tok_name(e->agg);
    if (e->kind == EXPR_FUNC)   return e->fname;
    return "expr";
}
/* Best-effort output type for an expression (only affects the type label/width;
 * printed values use each Value's own type). */
static ColumnType expr_type(const Expr *e, const Rel *in) {
    switch (e->kind) {
        case EXPR_COLUMN: { int idx = rel_find_col(in, &e->col); return idx >= 0 ? in->cols[idx].type : TYPE_NULL; }
        case EXPR_LITERAL: return e->literal.type;
        case EXPR_AGG: return e->agg == TOK_COUNT ? TYPE_INT : e->agg == TOK_AVG ? TYPE_FLOAT
                            : (e->left ? expr_type(e->left, in) : TYPE_INT);
        case EXPR_FUNC:
            if (ci_equal(e->fname, "length")) return TYPE_INT;
            if (ci_equal(e->fname, "round"))  return TYPE_FLOAT;
            if (ci_equal(e->fname, "upper") || ci_equal(e->fname, "lower") || ci_equal(e->fname, "trim")) return TYPE_STRING;
            if (ci_equal(e->fname, "abs") && e->nargs) return expr_type(e->args[0], in);
            return TYPE_NULL;
        case EXPR_BINARY:
            if (e->op == TOK_PLUS || e->op == TOK_MINUS || e->op == TOK_STAR || e->op == TOK_SLASH) {
                ColumnType l = expr_type(e->left, in), r = expr_type(e->right, in);
                return (l == TYPE_FLOAT || r == TYPE_FLOAT) ? TYPE_FLOAT : TYPE_INT;
            }
            return TYPE_INT;
        case EXPR_UNARY: return e->op == TOK_MINUS ? expr_type(e->left, in) : TYPE_INT;
        default: return TYPE_INT;   /* IN / ISNULL / BETWEEN -> boolean */
    }
}

/* Does an expression tree contain an aggregate anywhere? */
static int expr_has_agg(const Expr *e) {
    if (!e) return 0;
    if (e->kind == EXPR_AGG) return 1;
    if (expr_has_agg(e->left) || expr_has_agg(e->right) || expr_has_agg(e->lo) || expr_has_agg(e->hi)) return 1;
    for (size_t i = 0; i < e->nargs; i++) if (expr_has_agg(e->args[i])) return 1;
    return 0;
}

/* Build the output column schema for the SELECT list against input rel `in`. */
static void out_schema(const SelectStmt *s, const Rel *in, ColMeta **cols, size_t *ncols) {
    ColMeta *c = NULL; size_t n = 0;
    for (size_t i = 0; i < s->nitems; i++) {
        SelItem *it = &s->items[i];
        if (it->kind == SEL_STAR) {
            for (size_t k = 0; k < in->ncols; k++) {
                c = xrealloc(c, (n + 1) * sizeof(ColMeta));
                c[n].qualifier = in->cols[k].qualifier ? xstrdup(in->cols[k].qualifier) : NULL;
                c[n].name = xstrdup(in->cols[k].name); c[n].type = in->cols[k].type; n++;
            }
        } else {
            c = xrealloc(c, (n + 1) * sizeof(ColMeta));
            const Expr *e = it->expr;
            /* keep the qualifier of a plain column so ORDER BY t.col resolves */
            c[n].qualifier = (e->kind == EXPR_COLUMN && e->col.table) ? xstrdup(e->col.table) : NULL;
            c[n].name = xstrdup(sel_name(it));
            c[n].type = expr_type(e, in); n++;
        }
    }
    *cols = c; *ncols = n;
}

/* --- projection with no aggregation: one output row per input row. */
static Rel project_plain(const SelectStmt *s, const Rel *in) {
    Rel out = {0};
    out_schema(s, in, &out.cols, &out.ncols);
    for (size_t i = 0; i < in->nrows; i++) {
        Row row; row.count = out.ncols; row.values = xmalloc(out.ncols * sizeof(Value));
        size_t vi = 0;
        EvalCtx cx = { in, &in->rows[i], NULL, 0, 0 };
        for (size_t k = 0; k < s->nitems; k++) {
            SelItem *it = &s->items[k];
            if (it->kind == SEL_STAR)
                for (size_t j = 0; j < in->ncols; j++) row.values[vi++] = value_dup(&in->rows[i].values[j]);
            else { Value a = eval(it->expr, &cx); row.values[vi++] = value_dup(&a); }
        }
        arena_reset();
        rel_add_row(&out, row);
    }
    return out;
}

/* --- GROUP BY + aggregates (+ HAVING). ngroup==0 means one group = all rows. */
typedef struct { Row **rows; size_t nrows, cap; } Group;
typedef struct GNode { unsigned long h; size_t gi; struct GNode *next; } GNode;

/* Hash a row's group-key values together (order-sensitive FNV-1a mix). */
static unsigned long group_hash(const Row *row, const int *gidx, size_t ng) {
    unsigned long h = 1469598103934665603UL;
    for (size_t k = 0; k < ng; k++) {
        int idx = gidx[k]; if (idx < 0) continue;
        const Value *v = &row->values[idx];
        h ^= (v->type == TYPE_NULL) ? 0 : value_hash(v);
        h *= 1099511628211UL;
    }
    return h;
}

static Rel aggregate(const SelectStmt *s, const Rel *in) {
    /* Bucket rows into groups using a hash table on the group-key values.
       A linear search over existing groups would be O(rows * groups) —
       catastrophic for high-cardinality keys — so we hash instead (~O(rows)). */
    Group *groups = NULL; size_t ng = 0, gcap = 0;
    int *gidx = xmalloc(s->ngroup * sizeof(int));
    for (size_t k = 0; k < s->ngroup; k++) gidx[k] = rel_find_col(in, &s->group[k]);

    size_t nb = 1024; while (nb < in->nrows) nb <<= 1;   /* power of two */
    GNode **buckets = xcalloc(nb, sizeof(GNode *));

    for (size_t i = 0; i < in->nrows; i++) {
        unsigned long h = group_hash(&in->rows[i], gidx, s->ngroup);
        size_t b = h & (nb - 1);
        long g = -1;
        for (GNode *n = buckets[b]; n; n = n->next) {
            if (n->h != h) continue;
            int same = 1;                                /* confirm on hash hit */
            for (size_t k = 0; k < s->ngroup; k++) {
                int idx = gidx[k]; if (idx < 0) continue;
                int cmp; Value *a = &in->rows[i].values[idx];
                Value *bb = &groups[n->gi].rows[0]->values[idx];
                if (a->type == TYPE_NULL && bb->type == TYPE_NULL) continue;
                if (!value_compare(a, bb, &cmp) || cmp != 0) { same = 0; break; }
            }
            if (same) { g = (long)n->gi; break; }
        }
        if (g < 0) {   /* new group */
            if (ng == gcap) { gcap = gcap ? gcap * 2 : 16; groups = xrealloc(groups, gcap * sizeof(Group)); }
            groups[ng].rows = NULL; groups[ng].nrows = 0; groups[ng].cap = 0;
            GNode *node = xmalloc(sizeof(GNode));
            node->h = h; node->gi = ng; node->next = buckets[b]; buckets[b] = node;
            g = (long)ng++;
        }
        Group *grp = &groups[g];
        if (grp->nrows == grp->cap) { grp->cap = grp->cap ? grp->cap * 2 : 8;
                                      grp->rows = xrealloc(grp->rows, grp->cap * sizeof(Row *)); }
        grp->rows[grp->nrows++] = &in->rows[i];
    }
    for (size_t b = 0; b < nb; b++)                      /* free the hash index */
        for (GNode *n = buckets[b]; n; ) { GNode *nx = n->next; free(n); n = nx; }
    free(buckets);

    /* Whole-table aggregate with no GROUP BY and no rows: still one group. */
    if (s->ngroup == 0 && ng == 0) {
        groups = xrealloc(groups, sizeof(Group));
        groups[0].rows = NULL; groups[0].nrows = 0; groups[0].cap = 0; ng = 1;
    }

    Rel out = {0};
    out_schema(s, in, &out.cols, &out.ncols);
    for (size_t gi = 0; gi < ng; gi++) {
        Row **grows = groups[gi].rows; size_t gn = groups[gi].nrows;
        if (s->having) { int keep = is_true(eval_group(s->having, in, grows, gn)); arena_reset(); if (!keep) continue; }
        Row row; row.count = out.ncols; row.values = xmalloc(out.ncols * sizeof(Value));
        size_t vi = 0;
        EvalCtx cx = { in, gn ? grows[0] : NULL, grows, gn, 1 };
        for (size_t k = 0; k < s->nitems; k++) {
            SelItem *it = &s->items[k];
            if (it->kind == SEL_STAR) {
                for (size_t j = 0; j < in->ncols; j++) {
                    Value nv; nv.type = TYPE_NULL;
                    row.values[vi++] = gn ? value_dup(&grows[0]->values[j]) : nv;
                }
            } else { Value a = eval(it->expr, &cx); row.values[vi++] = value_dup(&a); }
        }
        arena_reset();
        rel_add_row(&out, row);
    }
    for (size_t gi = 0; gi < ng; gi++) free(groups[gi].rows);
    free(groups); free(gidx);
    return out;
}

/* --- ORDER BY via qsort. qsort has no context arg, so we pass it through
 * file-scope pointers (single-threaded, so this is safe here). */
static const Rel *g_ord_rel; static const OrderItem *g_ord; static size_t g_ord_n;
/* Resolve an ORDER BY key to a column index in the (output) relation: a
 * positional `ORDER BY 2` uses column 2, otherwise resolve the column by name. */
static int order_col_idx(const Rel *r, const OrderItem *o) {
    if (o->pos >= 1) return (o->pos - 1 < (long)r->ncols) ? (int)(o->pos - 1) : -1;
    if (o->expr && o->expr->kind == EXPR_COLUMN) return rel_find_col(r, &o->expr->col);
    return -1;
}
static int row_cmp(const void *pa, const void *pb) {
    const Row *ra = pa, *rb = pb;
    for (size_t k = 0; k < g_ord_n; k++) {
        int idx = order_col_idx(g_ord_rel, &g_ord[k]);
        if (idx < 0) continue;
        Value *a = &ra->values[idx], *b = &rb->values[idx];
        int c;
        if (a->type == TYPE_NULL && b->type == TYPE_NULL) c = 0;
        else if (a->type == TYPE_NULL) c = -1;
        else if (b->type == TYPE_NULL) c = 1;
        else if (!value_compare(a, b, &c)) c = 0;
        if (c != 0) return g_ord[k].desc ? -c : c;
    }
    return 0;
}
static void order_rel(Rel *r, const SelectStmt *s) {
    if (r->nrows == 0) return;                        /* qsort(NULL,0,...) is UB */
    g_ord_rel = r; g_ord = s->order; g_ord_n = s->norder;
    qsort(r->rows, r->nrows, sizeof(Row), row_cmp);
}

/* SELECT DISTINCT: drop duplicate output rows, keeping first occurrence.
 * Rows are keyed by a hash of all their values (O(rows) average). */
static int rows_equal(const Row *a, const Row *b, size_t ncols) {
    for (size_t j = 0; j < ncols; j++) {
        const Value *x = &a->values[j], *y = &b->values[j];
        if (x->type == TYPE_NULL && y->type == TYPE_NULL) continue;
        if (x->type == TYPE_NULL || y->type == TYPE_NULL) return 0;
        int c; if (!value_compare(x, y, &c) || c != 0) return 0;
    }
    return 1;
}
static void distinct_rel(Rel *r) {
    struct RNode { unsigned long h; size_t idx; struct RNode *next; } **buckets;
    size_t nb = 16; while (nb < r->nrows + 1) nb <<= 1;
    buckets = xcalloc(nb, sizeof(void *));
    Row *keep = NULL; size_t nk = 0, kc = 0;
    for (size_t i = 0; i < r->nrows; i++) {
        unsigned long h = 1469598103934665603UL;
        for (size_t j = 0; j < r->ncols; j++) {
            Value *v = &r->rows[i].values[j];
            h ^= (v->type == TYPE_NULL) ? 0 : value_hash(v); h *= 1099511628211UL;
        }
        size_t b = h & (nb - 1); int dup = 0;
        for (struct RNode *nd = buckets[b]; nd; nd = nd->next)
            if (nd->h == h && rows_equal(&r->rows[i], &keep[nd->idx], r->ncols)) { dup = 1; break; }
        if (dup) {
            for (size_t j = 0; j < r->rows[i].count; j++)
                if (r->rows[i].values[j].type == TYPE_STRING) free(r->rows[i].values[j].as.str_val);
            free(r->rows[i].values);
        } else {
            if (nk == kc) { kc = kc ? kc * 2 : 16; keep = xrealloc(keep, kc * sizeof(Row)); }
            struct RNode *nd = xmalloc(sizeof *nd); nd->h = h; nd->idx = nk; nd->next = buckets[b]; buckets[b] = nd;
            keep[nk++] = r->rows[i];
        }
    }
    for (size_t b = 0; b < nb; b++) { struct RNode *nd = buckets[b]; while (nd) { struct RNode *x = nd->next; free(nd); nd = x; } }
    free(buckets); free(r->rows);
    r->rows = keep; r->nrows = nk; r->cap = kc;
}

/* --- the conductor: run a SelectStmt against a set of loaded tables. --- */
typedef struct { char *name; Table table; } NamedTable;
typedef struct { NamedTable *items; size_t count; } Database;

static Table *db_get(Database *db, const char *name) {
    for (size_t i = 0; i < db->count; i++)
        if (strcmp(db->items[i].name, name) == 0) return &db->items[i].table;
    return NULL;
}

/* --- planner helpers: predicate pushdown --------------------------------- */

/* Flatten a WHERE tree into its top-level AND-conjuncts (borrowed pointers). */
static void collect_conjuncts(const Expr *e, const Expr ***arr, size_t *n) {
    if (!e) return;
    if (e->kind == EXPR_BINARY && e->op == TOK_AND) {
        collect_conjuncts(e->left, arr, n);
        collect_conjuncts(e->right, arr, n);
    } else {
        *arr = xrealloc(*arr, (*n + 1) * sizeof(Expr *));
        (*arr)[(*n)++] = e;
    }
}
static void esq_walk(const Expr *e, const char **q, int *ncols, int *bad) {
    if (!e) return;
    if (e->kind == EXPR_COLUMN) {
        (*ncols)++;
        if (!e->col.table) { *bad = 1; return; }
        if (!*q) *q = e->col.table;
        else if (strcmp(*q, e->col.table) != 0) *bad = 1;
        return;
    }
    if (e->kind == EXPR_AGG) { *bad = 1; return; }
    /* recurse into EVERY child — a column hiding inside UPPER()/IN/BETWEEN/NOT
       must be seen, or we'd wrongly push a multi-table predicate down one side */
    esq_walk(e->left, q, ncols, bad); esq_walk(e->right, q, ncols, bad);
    esq_walk(e->lo, q, ncols, bad);   esq_walk(e->hi, q, ncols, bad);
    for (size_t i = 0; i < e->nargs; i++) esq_walk(e->args[i], q, ncols, bad);
}
/* If every column in e is qualified by ONE table, return that qualifier and set
 * *ncols; otherwise return NULL. Used to decide if a predicate can be pushed. */
static const char *expr_single_qualifier(const Expr *e, int *ncols) {
    const char *q = NULL; int bad = 0; *ncols = 0;
    esq_walk(e, &q, ncols, &bad);
    return bad ? NULL : q;
}

/* Collect the distinct table qualifiers referenced by an expression. */
static void expr_quals(const Expr *e, const char **set, int *n, int cap) {
    if (!e) return;
    if (e->kind == EXPR_COLUMN) {
        if (e->col.table) {
            for (int i = 0; i < *n; i++) if (strcmp(set[i], e->col.table) == 0) return;
            if (*n < cap) set[(*n)++] = e->col.table;
        }
        return;
    }
    expr_quals(e->left, set, n, cap); expr_quals(e->right, set, n, cap);
    expr_quals(e->lo, set, n, cap);   expr_quals(e->hi, set, n, cap);
    for (size_t i = 0; i < e->nargs; i++) expr_quals(e->args[i], set, n, cap);
}

/* Cost-based join ordering. Fills order[0..njoins-1] with the sequence in which
 * to apply the joins. For all-INNER queries with 2+ joins we reorder greedily:
 * repeatedly add the smallest table whose join predicate only references tables
 * already joined (so its ON clause still resolves). Otherwise we keep the
 * written order (LEFT joins aren't freely reorderable). */
/* Does any column reference in the tree lack a table qualifier? */
static int expr_has_unqualified_col(const Expr *e) {
    if (!e) return 0;
    if (e->kind == EXPR_COLUMN) return e->col.table == NULL;
    if (expr_has_unqualified_col(e->left) || expr_has_unqualified_col(e->right) ||
        expr_has_unqualified_col(e->lo) || expr_has_unqualified_col(e->hi)) return 1;
    for (size_t i = 0; i < e->nargs; i++) if (expr_has_unqualified_col(e->args[i])) return 1;
    return 0;
}

static void plan_join_order(Database *db, const SelectStmt *s, size_t *order) {
    size_t m = s->njoins;
    /* Reordering is only SAFE when we can determine each join's dependencies:
       all INNER, 2+ joins, small enough for the fixed node arrays, and every ON
       clause fully qualified (an unqualified column could let us join a table
       before its key exists -> silent wrong results). Otherwise keep the order
       as written. */
    int reorderable = (m >= 2) && (m + 1 <= 64);
    for (size_t i = 0; i < m && reorderable; i++)
        if (s->joins[i].type != JOIN_INNER || expr_has_unqualified_col(s->joins[i].on)) reorderable = 0;
    if (!reorderable) { for (size_t i = 0; i < m; i++) order[i] = i; return; }

    /* node 0 = base (FROM); node k+1 = the table of join k */
    const char *nq[64]; nq[0] = s->from_alias ? s->from_alias : s->from_table;
    for (size_t k = 0; k < m && k + 1 < 64; k++)
        nq[k + 1] = s->joins[k].alias ? s->joins[k].alias : s->joins[k].table;

    char inS[64] = {0}; inS[0] = 1;                 /* base is the seed */
    size_t no = 0;
    for (size_t step = 0; step < m; step++) {
        long best = -1; double bestsz = 0;
        for (size_t k = 0; k < m; k++) {
            if (inS[k + 1]) continue;
            const char *qs[16]; int nqn = 0;
            expr_quals(s->joins[k].on, qs, &nqn, 16);
            int ok = 1;                             /* all other endpoints already joined? */
            for (int qi = 0; qi < nqn; qi++) {
                int node = -1;
                for (size_t d = 0; d <= m; d++) if (nq[d] && strcmp(nq[d], qs[qi]) == 0) { node = (int)d; break; }
                if (node >= 0 && node != (int)(k + 1) && !inS[node]) { ok = 0; break; }
            }
            if (!ok) continue;
            Table *t = db_get(db, s->joins[k].table);
            double sz = t ? (double)t->row_count : 1e18;
            if (best < 0 || sz < bestsz) { best = (long)k; bestsz = sz; }
        }
        if (best < 0) for (size_t k = 0; k < m; k++) if (!inS[k + 1]) { best = (long)k; break; }
        order[no++] = (size_t)best; inS[best + 1] = 1;
    }
}

/* If conjunct e is `col OP literal` (or `literal OP col`) on the base table,
 * return the base column index and set *op (normalized so the column is on the
 * left) and *key (borrowed literal). Otherwise return -1. */
static int indexable_pred(const Expr *e, Table *base, const char *bq,
                          TokenType *op, const Value **key) {
    if (!e || e->kind != EXPR_BINARY) return -1;
    TokenType o = e->op;
    if (!(o == TOK_EQ || o == TOK_LT || o == TOK_GT || o == TOK_LE || o == TOK_GE)) return -1;
    const Expr *colE, *litE; int swapped = 0;
    if (e->left->kind == EXPR_COLUMN && e->right->kind == EXPR_LITERAL) { colE = e->left; litE = e->right; }
    else if (e->left->kind == EXPR_LITERAL && e->right->kind == EXPR_COLUMN) { colE = e->right; litE = e->left; swapped = 1; }
    else return -1;
    if (colE->col.table && strcmp(colE->col.table, bq) != 0) return -1;
    int ci = -1;
    for (size_t i = 0; i < base->col_count; i++)
        if (strcmp(base->col_names[i], colE->col.column) == 0) { ci = (int)i; break; }
    if (ci < 0) return -1;
    if (swapped) switch (o) { case TOK_LT: o = TOK_GT; break; case TOK_GT: o = TOK_LT; break;
                              case TOK_LE: o = TOK_GE; break; case TOK_GE: o = TOK_LE; break; default: break; }
    *op = o; *key = &litE->literal; return ci;
}

/* ===========================================================================
 * BINDER (name resolution): before executing, resolve every column reference
 * against the query's schema and ERROR on anything unknown — otherwise a typo'd
 * column silently evaluates to NULL and the query "works" but returns nothing.
 * Scope = all FROM+JOIN columns. SELECT aliases are honored ONLY in ORDER BY
 * (where the executor can resolve them, because output columns carry the alias
 * name) — NOT in WHERE/HAVING/GROUP BY, where the executor can't resolve an
 * alias and would silently produce NULL. So an alias used there errors loudly.
 * ========================================================================= */
typedef struct { const char *qual; const char *name; } BCol;
typedef struct { BCol *cols; size_t n, cap; const char **aliases; size_t na, nacap; int allow_alias; } BindScope;

static void bs_addcol(BindScope *sc, const char *q, const char *nm) {
    if (sc->n == sc->cap) { sc->cap = sc->cap ? sc->cap * 2 : 16; sc->cols = xrealloc(sc->cols, sc->cap * sizeof(BCol)); }
    sc->cols[sc->n].qual = q; sc->cols[sc->n].name = nm; sc->n++;
}
static void bs_addalias(BindScope *sc, const char *a) {
    if (sc->na == sc->nacap) { sc->nacap = sc->nacap ? sc->nacap * 2 : 8; sc->aliases = xrealloc(sc->aliases, sc->nacap * sizeof(char *)); }
    sc->aliases[sc->na++] = a;
}
static int bs_addtable(BindScope *sc, Database *db, const char *tbl, const char *alias) {
    Table *t = db_get(db, tbl);
    if (!t) { fprintf(stderr, "bind error: no such table: %s\n", tbl); return 0; }
    const char *q = alias ? alias : tbl;
    for (size_t i = 0; i < t->col_count; i++) bs_addcol(sc, q, t->col_names[i]);
    return 1;
}
/* returns 1 if the column resolves; prints a specific error and returns 0 if not */
static int scope_resolve(const BindScope *sc, const ColRef *c) {
    if (c->table) {                                   /* qualified: t.col */
        int col = 0, tab = 0;
        for (size_t i = 0; i < sc->n; i++)
            if (strcmp(sc->cols[i].qual, c->table) == 0) { tab = 1; if (strcmp(sc->cols[i].name, c->column) == 0) col++; }
        if (col >= 1) return 1;
        if (tab) fprintf(stderr, "bind error: no such column: %s.%s\n", c->table, c->column);
        else     fprintf(stderr, "bind error: no such table/alias: %s\n", c->table);
        return 0;
    }
    int m = 0;                                        /* unqualified */
    for (size_t i = 0; i < sc->n; i++) if (strcmp(sc->cols[i].name, c->column) == 0) m++;
    if (m == 1) return 1;
    if (m == 0 && sc->allow_alias)                    /* aliases resolve only in ORDER BY */
        for (size_t i = 0; i < sc->na; i++) if (strcmp(sc->aliases[i], c->column) == 0) return 1;
    if (m > 1) fprintf(stderr, "bind error: ambiguous column: %s\n", c->column);
    else       fprintf(stderr, "bind error: no such column: %s\n", c->column);
    return 0;
}
static int bind_expr(const BindScope *sc, const Expr *e) {
    if (!e) return 1;
    if (e->kind == EXPR_COLUMN) return scope_resolve(sc, &e->col);
    if (!bind_expr(sc, e->left) || !bind_expr(sc, e->right) ||
        !bind_expr(sc, e->lo)   || !bind_expr(sc, e->hi)) return 0;
    for (size_t i = 0; i < e->nargs; i++) if (!bind_expr(sc, e->args[i])) return 0;
    return 1;
}
static int bind_select(Database *db, const SelectStmt *s) {
    BindScope sc = {0}; int ok = 1;
    if (!bs_addtable(&sc, db, s->from_table, s->from_alias)) { ok = 0; goto done; }
    for (size_t i = 0; i < s->njoins && ok; i++)
        if (!bs_addtable(&sc, db, s->joins[i].table, s->joins[i].alias)) ok = 0;
    if (!ok) goto done;
    for (size_t i = 0; i < s->nitems; i++) if (s->items[i].alias) bs_addalias(&sc, s->items[i].alias);

    sc.allow_alias = 0;   /* aliases NOT visible in JOIN/WHERE/GROUP/SELECT/HAVING */
    for (size_t i = 0; i < s->njoins && ok; i++) ok = bind_expr(&sc, s->joins[i].on);
    if (ok && s->where) ok = bind_expr(&sc, s->where);
    for (size_t i = 0; i < s->ngroup && ok; i++) ok = scope_resolve(&sc, &s->group[i]);
    for (size_t i = 0; i < s->nitems && ok; i++) if (s->items[i].kind == SEL_EXPR) ok = bind_expr(&sc, s->items[i].expr);
    if (ok && s->having) ok = bind_expr(&sc, s->having);
    sc.allow_alias = 1;   /* ORDER BY may reference SELECT aliases */
    for (size_t i = 0; i < s->norder && ok; i++) if (s->order[i].pos < 0) ok = bind_expr(&sc, s->order[i].expr);
done:
    free(sc.cols); free(sc.aliases);
    return ok;
}

static int execute_select(Database *db, const SelectStmt *s, Rel *result) {
    if (!bind_select(db, s)) return 0;               /* resolve names or fail loudly */
    Table *base = db_get(db, s->from_table);
    if (!base) { fprintf(stderr, "no such table: %s\n", s->from_table); return 0; }

    /* Planner: split WHERE into conjuncts so single-table ones can be pushed
       down to filter a relation BEFORE joining (smaller intermediate results). */
    const Expr **conj = NULL; size_t nconj = 0;
    if (s->where) collect_conjuncts(s->where, &conj, &nconj);
    char *pushed = xcalloc(nconj ? nconj : 1, 1);

    const char *bq = s->from_alias ? s->from_alias : s->from_table;

    /* Index scan: if a base predicate is `col OP literal` and a suitable index
       exists (or auto-index is on), fetch just the matching rows instead of
       copying and scanning the whole table. */
    Rel cur; int built = 0; g_used_index = 0;
    for (size_t c = 0; c < nconj && !built; c++) {
        TokenType op; const Value *key;
        int ci = indexable_pred(conj[c], base, bq, &op, &key);
        if (ci < 0) continue;
        int want_range = (op != TOK_EQ);
        Index *ix = table_find_index(base, ci, want_range);
        /* auto-build only the cheap hash index (equality). Ranges need a sorted
           index (an O(n log n) build) — not worth it unless the user asks via
           .index <t> <c> sorted, so we don't auto-build those. */
        if (!ix && g_auto_index && !want_range) {
            table_add_index(base, ci, IDX_HASH);
            ix = table_find_index(base, ci, 0);
        }
        if (!ix) continue;
        size_t *rows; size_t nr = index_lookup(base, ix, op, key, &rows);
        cur = rel_from_table_rows(base, bq, rows, nr);
        free(rows);
        pushed[c] = 1; built = 1; g_used_index = 1;
    }
    if (!built) cur = rel_from_table(base, bq);

    for (size_t c = 0; c < nconj; c++) {           /* push remaining base predicates as filters */
        if (pushed[c]) continue;
        int n; const char *q = expr_single_qualifier(conj[c], &n);
        if (n > 0 && q && strcmp(q, bq) == 0) {
            Rel f = filter_rel(&cur, conj[c]); rel_free(&cur); cur = f; pushed[c] = 1;
        }
    }

    size_t *order = xmalloc((s->njoins ? s->njoins : 1) * sizeof(size_t));
    plan_join_order(db, s, order);
    for (size_t oi = 0; oi < s->njoins; oi++) {
        size_t i = order[oi];
        Table *rt = db_get(db, s->joins[i].table);
        if (!rt) { fprintf(stderr, "no such table: %s\n", s->joins[i].table);
                   rel_free(&cur); free(pushed); free(conj); free(order); return 0; }
        const char *rq = s->joins[i].alias ? s->joins[i].alias : s->joins[i].table;

        /* Is there a single-table filter targeting this right table? If so we
           can't index-join it (the index is over ALL its rows). */
        int right_has_filter = 0;
        for (size_t c = 0; c < nconj; c++) if (!pushed[c]) {
            int n; const char *q = expr_single_qualifier(conj[c], &n);
            if (n > 0 && q && strcmp(q, rq) == 0) { right_has_filter = 1; break; }
        }

        /* INDEX JOIN: if the right table is unfiltered and has (or auto-gets) a
           hash index on the join key, probe it per left row instead of copying
           the whole right table. */
        int did_index_join = 0;
        if (!right_has_filter && g_join_mode != 1) {   /* respect forced nested-loop */
            Rel rsch = rel_schema_only(rt, rq);
            int lk, rk;
            if (equijoin_keys(s->joins[i].on, &cur, &rsch, &lk, &rk)) {
                Index *ix = table_find_index(rt, rk, 0);
                if (!ix && g_auto_index) { table_add_index(rt, rk, IDX_HASH); ix = table_find_index(rt, rk, 0); }
                if (ix) {
                    Rel joined = join_index(&cur, rt, rq, lk, rk, ix, s->joins[i].type);
                    rel_free(&cur); cur = joined; did_index_join = 1;
                }
            }
            rel_free(&rsch);
        }
        if (did_index_join) continue;

        Rel right = rel_from_table(rt, rq);
        /* A single-table predicate on an INNER-joined table can be pushed too.
           (Not for LEFT joins — that would wrongly drop NULL-extended rows.) */
        if (s->joins[i].type == JOIN_INNER)
            for (size_t c = 0; c < nconj; c++) if (!pushed[c]) {
                int n; const char *q = expr_single_qualifier(conj[c], &n);
                if (n > 0 && q && strcmp(q, rq) == 0) {
                    Rel f = filter_rel(&right, conj[c]); rel_free(&right); right = f; pushed[c] = 1;
                }
            }
        const char *algo;
        Rel joined = join_dispatch(&cur, &right, s->joins[i].on, s->joins[i].type, &algo);
        rel_free(&cur); rel_free(&right); cur = joined;
    }

    free(order);
    /* Residual WHERE: any conjunct not pushed down (spans tables, unqualified). */
    for (size_t c = 0; c < nconj; c++) if (!pushed[c]) {
        Rel f = filter_rel(&cur, conj[c]); rel_free(&cur); cur = f;
    }
    free(pushed); free(conj);

    int has_agg = 0;
    for (size_t i = 0; i < s->nitems; i++) if (s->items[i].kind == SEL_EXPR && expr_has_agg(s->items[i].expr)) has_agg = 1;

    Rel out = (has_agg || s->ngroup > 0) ? aggregate(s, &cur) : project_plain(s, &cur);
    rel_free(&cur);

    if (s->distinct) distinct_rel(&out);
    if (s->norder) {
        /* every ORDER BY term must be an output column/position — we can't sort
           by a column that projection already dropped. Error loudly rather than
           silently ignore it. */
        for (size_t k = 0; k < s->norder; k++)
            if (order_col_idx(&out, &s->order[k]) < 0) {
                fprintf(stderr, "error: ORDER BY term %zu is not a selected column or valid position\n", k + 1);
                rel_free(&out); return 0;
            }
        order_rel(&out, s);
    }
    if (s->limit >= 0 && (size_t)s->limit < out.nrows) {
        for (size_t i = s->limit; i < out.nrows; i++) {
            for (size_t j = 0; j < out.rows[i].count; j++)
                if (out.rows[i].values[j].type == TYPE_STRING) free(out.rows[i].values[j].as.str_val);
            free(out.rows[i].values);
        }
        out.nrows = s->limit;
    }
    *result = out;
    return 1;
}

/* Print a result relation as an aligned grid. */
static void rel_print(const Rel *r) {
    char buf[512];
    size_t *w = xmalloc(r->ncols * sizeof(size_t));
    for (size_t c = 0; c < r->ncols; c++) {
        w[c] = strlen(r->cols[c].name);
        size_t tl = strlen(type_name(r->cols[c].type)); if (tl > w[c]) w[c] = tl;
    }
    for (size_t i = 0; i < r->nrows; i++)
        for (size_t c = 0; c < r->ncols; c++) {
            value_str(&r->rows[i].values[c], buf, sizeof buf);
            size_t len = strlen(buf); if (len > w[c]) w[c] = len;
        }
    for (size_t c = 0; c < r->ncols; c++) printf("%-*s  ", (int)w[c], r->cols[c].name);
    printf("\n");
    for (size_t c = 0; c < r->ncols; c++) printf("%-*s  ", (int)w[c], type_name(r->cols[c].type));
    printf("\n");
    for (size_t c = 0; c < r->ncols; c++) { for (size_t i = 0; i < w[c]; i++) putchar('-'); printf("  "); }
    printf("\n");
    for (size_t i = 0; i < r->nrows; i++) {
        for (size_t c = 0; c < r->ncols; c++) {
            value_str(&r->rows[i].values[c], buf, sizeof buf);
            printf("%-*s  ", (int)w[c], buf);
        }
        printf("\n");
    }
    free(w);
    printf("(%zu row%s)\n", r->nrows, r->nrows == 1 ? "" : "s");
}

/* ===========================================================================
 * STEP 9 — QUERY PLANNER (EXPLAIN): reason about a query WITHOUT executing it,
 * using only table sizes and schemas. Shows join order, the chosen algorithm
 * per join (hash vs nested-loop), pushed-down filters, and a rough cost.
 * ========================================================================= */
static Rel rel_schema_only(const Table *t, const char *qual) {
    Rel r = {0}; r.ncols = t->col_count; r.cols = xmalloc(r.ncols * sizeof(ColMeta));
    for (size_t i = 0; i < r.ncols; i++) {
        r.cols[i].qualifier = qual ? xstrdup(qual) : NULL;
        r.cols[i].name = xstrdup(t->col_names[i]); r.cols[i].type = t->col_types[i];
    }
    return r;   /* no rows: planning only needs the schema */
}
static Rel rel_combine_schema(const Rel *L, const Rel *R) {
    Rel out = {0}; out.ncols = L->ncols + R->ncols; out.cols = xmalloc(out.ncols * sizeof(ColMeta));
    for (size_t i = 0; i < L->ncols; i++) {
        out.cols[i].qualifier = L->cols[i].qualifier ? xstrdup(L->cols[i].qualifier) : NULL;
        out.cols[i].name = xstrdup(L->cols[i].name); out.cols[i].type = L->cols[i].type;
    }
    for (size_t i = 0; i < R->ncols; i++) { size_t o = L->ncols + i;
        out.cols[o].qualifier = R->cols[i].qualifier ? xstrdup(R->cols[i].qualifier) : NULL;
        out.cols[o].name = xstrdup(R->cols[i].name); out.cols[o].type = R->cols[i].type;
    }
    return out;
}
static void explain_select(Database *db, const SelectStmt *s) {
    if (!bind_select(db, s)) return;                 /* validate names first */
    Table *base = db_get(db, s->from_table);
    if (!base) { fprintf(stderr, "no such table: %s\n", s->from_table); return; }
    const Expr **conj = NULL; size_t nconj = 0;
    if (s->where) collect_conjuncts(s->where, &conj, &nconj);
    char *pushed = xcalloc(nconj ? nconj : 1, 1);

    const char *bq = s->from_alias ? s->from_alias : s->from_table;
    Rel cur = rel_schema_only(base, bq);
    double est = (double)base->row_count;
    printf("QUERY PLAN\n");
    printf("  Scan %s (rows=%zu)\n", bq, base->row_count);
    for (size_t c = 0; c < nconj; c++) { int n; const char *q = expr_single_qualifier(conj[c], &n);
        if (n > 0 && q && strcmp(q, bq) == 0) { printf("    +push filter onto %s\n", bq); pushed[c] = 1; est *= 0.5; } }

    size_t *order = xmalloc((s->njoins ? s->njoins : 1) * sizeof(size_t));
    plan_join_order(db, s, order);
    for (size_t oi = 0; oi < s->njoins; oi++) {
        size_t i = order[oi];
        Table *rt = db_get(db, s->joins[i].table);
        if (!rt) { printf("  (no such table: %s)\n", s->joins[i].table); break; }
        const char *rq = s->joins[i].alias ? s->joins[i].alias : s->joins[i].table;
        Rel right = rel_schema_only(rt, rq);
        double rsize = (double)rt->row_count;
        if (s->joins[i].type == JOIN_INNER)
            for (size_t c = 0; c < nconj; c++) if (!pushed[c]) { int n; const char *q = expr_single_qualifier(conj[c], &n);
                if (n > 0 && q && strcmp(q, rq) == 0) { printf("    +push filter onto %s\n", rq); pushed[c] = 1; rsize *= 0.5; } }
        int lk, rk, equi = equijoin_keys(s->joins[i].on, &cur, &right, &lk, &rk);
        const char *algo = (g_join_mode == 1) ? "nested-loop" : (equi ? "hash" : "nested-loop");
        double cost = strcmp(algo, "hash") == 0 ? est + rsize : est * rsize;
        printf("  %s JOIN %s [%s]  (left~%.0f x right~%.0f, cost~%.0f)\n",
               s->joins[i].type == JOIN_LEFT ? "LEFT" : "INNER", rq, algo, est, rsize, cost);
        Rel comb = rel_combine_schema(&cur, &right);
        rel_free(&cur); rel_free(&right); cur = comb;
        est = equi ? (est > rsize ? est : rsize) : est * rsize * 0.1;
    }
    for (size_t c = 0; c < nconj; c++) if (!pushed[c]) { printf("  Filter (residual WHERE)\n"); break; }
    int has_agg = 0; for (size_t i = 0; i < s->nitems; i++) if (s->items[i].kind == SEL_EXPR && expr_has_agg(s->items[i].expr)) has_agg = 1;
    if (has_agg || s->ngroup) printf("  Aggregate (%zu group key%s)\n", s->ngroup, s->ngroup == 1 ? "" : "s");
    if (s->norder) printf("  Sort (%zu key%s)\n", s->norder, s->norder == 1 ? "" : "s");
    if (s->limit >= 0) printf("  Limit %ld\n", s->limit);
    printf("  est. rows out ~ %.0f\n", est);
    rel_free(&cur); free(pushed); free(conj); free(order);
}

/* ===========================================================================
 * STEP 8 — REPL: the user-facing shell. Loads CSVs as tables, runs SQL,
 * pretty-prints results. Meta-commands start with '.'.
 * ========================================================================= */

/* Add a table under `name`, replacing any existing table of that name. */
static void db_add(Database *db, const char *name, Table t) {
    for (size_t i = 0; i < db->count; i++)
        if (strcmp(db->items[i].name, name) == 0) {
            table_free(&db->items[i].table);
            db->items[i].table = t;
            return;
        }
    db->items = xrealloc(db->items, (db->count + 1) * sizeof(NamedTable));
    db->items[db->count].name = xstrdup(name);
    db->items[db->count].table = t;
    db->count++;
}

static void meta_command(Database *db, char *line) {
    char *cmd = strtok(line, " \t");
    if (!cmd) return;

    if (strcmp(cmd, ".quit") == 0 || strcmp(cmd, ".exit") == 0) {
        exit(0);
    } else if (strcmp(cmd, ".help") == 0) {
        printf(".load <file.csv> <name>   load a CSV as a table\n"
               ".tables                   list loaded tables\n"
               ".schema <name>            show a table's columns and types\n"
               ".dump <name>              print an entire table\n"
               ".ast <sql>                show the parse tree for a query\n"
               ".plan <sql>               show the query plan (EXPLAIN)\n"
               ".join auto|nested|hash    choose join algorithm (default auto)\n"
               ".index <tbl> <col> [hash|sorted]  build a secondary index\n"
               ".autoindex on|off         auto-build indexes on filter columns\n"
               ".timer on|off             print per-query execution time\n"
               ".quit                     exit\n"
               "Anything else is run as a SQL SELECT query.\n");
    } else if (strcmp(cmd, ".load") == 0) {
        char *file = strtok(NULL, " \t");
        char *name = strtok(NULL, " \t");
        if (!file || !name) { fprintf(stderr, "usage: .load <file.csv> <name>\n"); return; }
        Table t = load_table(name, file);
        if (t.col_count == 0) { fprintf(stderr, "could not load %s\n", file); table_free(&t); return; }
        db_add(db, name, t);
        printf("loaded %zu row%s into \"%s\"\n", t.row_count, t.row_count == 1 ? "" : "s", name);
    } else if (strcmp(cmd, ".tables") == 0) {
        if (db->count == 0) printf("(no tables loaded)\n");
        for (size_t i = 0; i < db->count; i++)
            printf("  %s (%zu rows)\n", db->items[i].name, db->items[i].table.row_count);
    } else if (strcmp(cmd, ".schema") == 0) {
        char *name = strtok(NULL, " \t");
        Table *t = name ? db_get(db, name) : NULL;
        if (!t) { fprintf(stderr, "no such table: %s\n", name ? name : "(none)"); return; }
        printf("%s (\n", name);
        for (size_t c = 0; c < t->col_count; c++)
            printf("  %-16s %s\n", t->col_names[c], type_name(t->col_types[c]));
        printf(")\n");
    } else if (strcmp(cmd, ".dump") == 0) {
        char *name = strtok(NULL, " \t");
        Table *t = name ? db_get(db, name) : NULL;
        if (!t) { fprintf(stderr, "no such table: %s\n", name ? name : "(none)"); return; }
        print_table(t);
    } else if (strcmp(cmd, ".ast") == 0) {
        char *sql = strtok(NULL, "");   /* rest of the line */
        if (!sql) { fprintf(stderr, "usage: .ast <sql>\n"); return; }
        size_t n = strlen(sql); while (n && (sql[n-1]==';'||sql[n-1]==' ')) sql[--n]='\0';
        SelectStmt *s = parse_sql(sql);
        if (s) { print_ast(s); free_select(s); } else printf("(parse error)\n");
    } else if (strcmp(cmd, ".plan") == 0) {
        char *sql = strtok(NULL, "");
        if (!sql) { fprintf(stderr, "usage: .plan <sql>\n"); return; }
        size_t n = strlen(sql); while (n && (sql[n-1]==';'||sql[n-1]==' ')) sql[--n]='\0';
        SelectStmt *s = parse_sql(sql);
        if (s) { explain_select(db, s); free_select(s); } else printf("(parse error)\n");
    } else if (strcmp(cmd, ".join") == 0) {
        char *mode = strtok(NULL, " \t");
        if (!mode) { printf("join mode: %s\n", g_join_mode == 1 ? "nested" : "auto"); return; }
        if (strcmp(mode, "auto") == 0) g_join_mode = 0;
        else if (strcmp(mode, "nested") == 0) g_join_mode = 1;
        else if (strcmp(mode, "hash") == 0) g_join_mode = 0;   /* hash-when-possible == auto */
        else { fprintf(stderr, "usage: .join auto|nested|hash\n"); return; }
        printf("join mode set to %s\n", mode);
    } else if (strcmp(cmd, ".index") == 0) {
        char *tn = strtok(NULL, " \t"), *cn = strtok(NULL, " \t"), *kn = strtok(NULL, " \t");
        Table *t = tn ? db_get(db, tn) : NULL;
        if (!t || !cn) { fprintf(stderr, "usage: .index <table> <col> [hash|sorted]\n"); return; }
        int ci = -1;
        for (size_t i = 0; i < t->col_count; i++) if (strcmp(t->col_names[i], cn) == 0) ci = (int)i;
        if (ci < 0) { fprintf(stderr, "no such column: %s\n", cn); return; }
        IndexKind kind = IDX_HASH;
        if (kn && ci_equal(kn, "sorted")) kind = IDX_SORTED;
        else if (kn && !ci_equal(kn, "hash")) { fprintf(stderr, "unknown index type: %s (use hash or sorted)\n", kn); return; }
        table_add_index(t, ci, kind);
        printf("built %s index on %s.%s\n", kind == IDX_SORTED ? "sorted" : "hash", tn, cn);
    } else if (strcmp(cmd, ".autoindex") == 0) {
        char *m = strtok(NULL, " \t");
        if (m && strcmp(m, "on") == 0) g_auto_index = 1;
        else if (m && strcmp(m, "off") == 0) g_auto_index = 0;
        else { printf("autoindex is %s\n", g_auto_index ? "on" : "off"); return; }
        printf("autoindex %s\n", g_auto_index ? "on" : "off");
    } else if (strcmp(cmd, ".timer") == 0) {
        char *m = strtok(NULL, " \t");
        g_timer = (m && strcmp(m, "on") == 0);
        printf("timer %s\n", g_timer ? "on" : "off");
    } else {
        fprintf(stderr, "unknown command: %s (try .help)\n", cmd);
    }
}

static void run_sql(Database *db, const char *sql) {
    SelectStmt *s = parse_sql(sql);
    if (!s) return;                      /* tokenizer/parser already reported */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    Rel r;
    int ok = execute_select(db, s, &r);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (ok) { rel_print(&r); rel_free(&r); }
    if (g_timer) {
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr, "time: %.3f ms%s\n", ms, g_used_index ? " (index scan)" : "");
    }
    free_select(s);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);   /* line-buffer so output/errors interleave in order */
    Database db = {0};
    int interactive = isatty(0);

    char *line = NULL; size_t cap = 0; ssize_t len;
    if (interactive) { printf("minisql — .help for commands, .quit to exit\n"); printf("minisql> "); fflush(stdout); }

    while ((len = getline(&line, &cap, stdin)) != -1) {
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        /* trim leading whitespace */
        char *p = line; while (*p == ' ' || *p == '\t') p++;

        if (*p == '\0') { /* blank line */ }
        else if (*p == '.') meta_command(&db, p);
        else {
            /* strip a trailing ';' if present, then run as SQL */
            size_t n = strlen(p);
            while (n > 0 && (p[n-1] == ';' || p[n-1] == ' ' || p[n-1] == '\t')) p[--n] = '\0';
            if (n > 0) run_sql(&db, p);
        }
        if (interactive) { printf("minisql> "); fflush(stdout); }
    }
    if (interactive) printf("\n");

    free(line);
    for (size_t i = 0; i < db.count; i++) { free(db.items[i].name); table_free(&db.items[i].table); }
    free(db.items);
    return 0;
}
