# minisql

A tiny SQL engine in C — one file, no dependencies. It loads CSV files as tables
and runs `SELECT` queries over them, using the same `parse → plan → execute`
pipeline a real database uses, small enough to read start to finish.

## Build & run

```sh
make
./minisql          # interactive prompt; type .help for commands
```

## Example

```sh
./minisql <<'SQL'
.load data/albums.csv albums
SELECT genre, COUNT(*) FROM albums GROUP BY genre ORDER BY 2 DESC;
SQL
```

## What it supports

- `SELECT` with `WHERE`, `INNER`/`LEFT JOIN`, `GROUP BY`/`HAVING`, `ORDER BY`,
  `LIMIT`, `DISTINCT`
- Expressions (`+ - * /`, `AND`/`OR`/`NOT`, `LIKE`, `IN`, `BETWEEN`, `IS NULL`),
  functions (`UPPER`, `LENGTH`, `ROUND`, `COALESCE`, …), and aggregates
  (`COUNT`/`SUM`/`AVG`/`MIN`/`MAX`, `COUNT(DISTINCT)`)
- A small query planner: hash joins, join reordering, predicate pushdown, and
  optional hash/sorted indexes (`.index`)
- Errors on unknown columns instead of quietly returning nothing

## Semantics worth knowing

Where dialects disagree, minisql errs on the side of failing loudly:

- `SELECT` aliases are visible only in `ORDER BY` (like PostgreSQL / standard
  SQL; sqlite also allows them in `WHERE`/`HAVING`). Using one elsewhere is a
  bind error, never a silent `NULL`.
- `ORDER BY` must name a selected column or position — sorting happens after
  projection, so anything else errors instead of being ignored.
- Integer arithmetic (`+ - *`, `SUM`) that overflows 64 bits promotes to
  double instead of wrapping (sqlite raises an error here).
- Ragged CSV rows: missing fields become `NULL`; extra fields are dropped
  with a warning.
- The REPL is line-based: each statement must fit on one line (no `;`
  continuation across lines).
- Types are strict: comparing a string column to a number matches nothing
  (sqlite would coerce via column affinity — minisql never converts silently).
- Strings are bytes: `LENGTH` and `LIKE`'s `_` count bytes, and `UPPER`/`LOWER`
  fold ASCII only, so multi-byte UTF-8 characters count per byte.

## How it's built

```
CSV → tokenizer → parser (AST) → binder → planner → executor
```

Everything lives in `main.c` (~2,700 lines), in top-to-bottom order.

## Tests

```sh
make test      # built-in checks + regression suite (every past bug stays fixed)
make stress    # real queries, cross-checked against sqlite3 (needs sqlite3)
```
