# minisql

A tiny SQL engine in C — one file, no dependencies. It loads CSV files as tables
and runs `SELECT` queries over them, using the same `parse → plan → execute`
pipeline a real database uses, small enough to read start to finish.

Built to understand how databases actually work. It's a learning project, not a
replacement for sqlite/DuckDB.

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

## How it's built

```
CSV → tokenizer → parser (AST) → binder → planner → executor
```

Everything lives in `main.c` (~2,500 lines), in top-to-bottom order.

## Tests

```sh
make test      # built-in checks
make stress    # real queries, cross-checked against sqlite3 (needs sqlite3)
```

## Similar projects

- [cstack/db_tutorial](https://cstack.github.io/db_tutorial/) — build a SQLite clone in C, step by step
- [csvdb](https://github.com/TicklishHoneyBee/csvdb) — SQL over CSV files in C
- [dynajoe/tinydb](https://github.com/dynajoe/tinydb) — a relational engine written as a learning exercise
