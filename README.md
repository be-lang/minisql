# minisql

A tiny, dependency-free SQL engine in C that loads CSV files as in-memory
tables and runs `SELECT` queries over them. Built from scratch as the Week 1
deep-dive: it makes relational algebra concrete — selection, projection, join,
grouping, and aggregation are C functions you can read.

## Build & run

```sh
make            # builds ./minisql
./minisql       # interactive REPL
make test       # run the smoke tests
```

You can also pipe a script of commands:

```sh
./minisql <<'SQL'
.load data/artists.csv artists
.load data/albums.csv albums
SELECT a.name, COUNT(*) AS n FROM artists a
  JOIN albums b ON a.id = b.artist_id
  WHERE b.genre = 'Jazz'
  GROUP BY a.name ORDER BY n DESC;
SQL
```

## REPL meta-commands

| Command | Purpose |
|---|---|
| `.load <file.csv> <name>` | Load a CSV file as a table |
| `.tables` | List loaded tables |
| `.schema <name>` | Show a table's columns and inferred types |
| `.dump <name>` | Print an entire table |
| `.ast <sql>` | Show the parse tree for a query (debug) |
| `.plan <sql>` | Show the query plan / EXPLAIN (join order, algorithms, cost) |
| `.join auto\|nested\|hash` | Force a join algorithm (default `auto`) |
| `.index <tbl> <col> [hash\|sorted]` | Build a secondary index on a column |
| `.autoindex on\|off` | Auto-build an index on a filter column on first use |
| `.timer on\|off` | Print per-query execution time |
| `.help` | List commands |
| `.quit` / `.exit` | Exit |

Anything not starting with `.` is run as a SQL query (a trailing `;` is optional).

## Supported SQL

```
SELECT  <* | col | table.col | agg(col|*) [AS alias]> , ...
FROM    table [alias]
[ [INNER|LEFT] JOIN table [alias] ON <expr> ]*
[ WHERE <expr> ]
[ GROUP BY col , ... [ HAVING <expr> ] ]
[ ORDER BY col [ASC|DESC] , ... ]
[ LIMIT n ]
```

- Aggregates: `COUNT`, `SUM`, `AVG`, `MIN`, `MAX` (and `COUNT(*)`).
- Expressions: comparisons `= != < > <= >=`, boolean `AND`/`OR`, parentheses,
  integer/float/string/`NULL` literals, and (in `HAVING`) aggregates.
- Types are inferred per column: `int`, `float`, or `string`; empty cells → `NULL`.
- CSV: handles quoted fields, commas inside quotes, and `""` escaped quotes
  (RFC 4180), except a quoted field containing a real newline.

## How it works (the pipeline)

```
CSV file ─▶ load_table ─▶ Table          (storage layer)
SQL text ─▶ tokenize  ─▶ parse_select ─▶ SelectStmt (AST)
AST + tables ─▶ execute_select ─▶ Rel ─▶ rel_print   (executor)
```

Execution order inside `execute_select`: FROM → JOIN → WHERE → GROUP/aggregate
→ HAVING → project (SELECT) → ORDER BY → LIMIT.

### Default settings (already optimized)

The defaults are chosen to be fast without surprises, and match how a real
database behaves:

- **Joins**: `auto` — hash join for equijoins, nested-loop otherwise, with
  cost-based join reordering. Index join is used automatically when the right
  table has an index on the join key.
- **Grouping**: hash-based (handles high-cardinality `GROUP BY` in ~O(rows)).
- **Indexes**: **off by default** — like a real database, you create them
  explicitly (`.index`), because auto-indexing silently spends memory and only
  pays off when reused. For repeated/interactive point-lookup or selective-join
  workloads, turn on `.autoindex on` (it builds cheap hash indexes on filter and
  join keys on first use; it never auto-builds the costlier sorted index).

So out of the box you already get hash joins + reordering + hash aggregation;
add `.index` (or `.autoindex on`) to also get index scans and index joins.

### Query planner

A small cost-based planner runs before execution:
- **Join algorithm selection** — an equijoin (`a.x = b.y`, matching types) uses a
  **hash join** (O(n+m)); anything else falls back to **nested-loop** (O(n·m)).
- **Predicate pushdown** — single-table `WHERE` conjuncts are applied *before*
  joining (to the base table, or to an INNER-joined table), shrinking
  intermediate results.
- **Join reordering** — for all-INNER queries with 2+ joins, tables are joined
  smallest-first (subject to each join predicate's dependencies), keeping
  intermediate relations small.

Use `.plan <sql>` to see the chosen plan, and `.join nested` to force nested-loop
(useful for the benchmark below).

### Secondary indexes

Two index designs on a single column, built on demand with `.index` (or
automatically with `.autoindex on`). When a base-table predicate is
`col OP literal`, the planner uses a suitable index to fetch only the matching
rows instead of scanning the whole table.

- **Hash index** — value → bucket of row ids. O(1) average **equality** lookup.
  Cheapest to build; cannot answer ranges.
- **Sorted index** — row ids sorted by the column value; binary search handles
  **equality *and* ranges** (`<`, `>`, `<=`, `>=`). Costs a sort to build.

An index on a join key also enables an **index join**: instead of copying the
whole right table and hash-joining, the engine probes the right table's index
once per left row, so the right table is never materialized. On a repeated
selective-join workload (small filtered left × 100k-row right) this runs ~2.6×
faster than the hash join — the remaining cost is scanning the *left* table each
query, which a further index on the left key would cut.

Run `./bench/index_bench.sh`. On 2000 queries over 100k rows:

| workload | no index | hash | sorted |
|---|---|---|---|
| point lookup (`= ?`) | 11.5 s | **0.07 s** (~175×) | 0.09 s |
| selective range (`> ~995`, ~1%) | 31 s | n/a | **0.6 s** (~50×) |
| non-selective range (`> ~500`, ~50%) | 20 s | n/a | 9 s (~2×) |

Findings: **hash wins for equality** (faster and cheaper to build); **sorted is
required for ranges** and shines when the range is *selective* (a non-selective
range still materializes half the table, so the win shrinks). One-time build cost
is ~0 for hash and ~25 ms for sorted (the qsort) on 100k rows; memory overhead is
negligible next to the row data. **auto-index** matches explicit hash — it builds
the right index type on first use and reuses it — trading a little control for
zero ceremony. Indexes only pay off when **reused across queries**: building one
is itself an O(n) pass, so for a single query it's no faster than scanning.

## Benchmarks

Run `./bench/bench.sh` (needs `python3` + `sqlite3`). Representative numbers:

- **Hash vs nested-loop**, joining N×N rows: nested-loop grows ~4× per doubling
  of N (O(N²)); hash grows ~2× (O(N)). At N=4000, hash is ~120× faster.
- **vs sqlite3** on a 20k×100k join + group + sum: minisql ≈ 0.11 s (including
  CSV parsing) vs sqlite3 warm ≈ 0.10 s on a *pre-parsed* db — effectively tied,
  because the query touches every row (full hash-join + hash-aggregate) where an
  index can't help.
- **High-cardinality GROUP BY** (20k distinct keys over 100k rows): ~0.08 s,
  thanks to hash-based grouping. (A naive linear group search here would be
  O(rows × groups) — ~70× slower.)
- **Selective lookup** (`WHERE customer_id = 12345`): with `.index` built,
  minisql does an index seek instead of a full scan and becomes competitive (see
  the index table above). Without an index it does an O(n) scan. The remaining
  structural gap from a real database is **no persistence** — minisql re-parses
  the CSV every run and rebuilds indexes per session.

All of it lives in one file, `main.c`, in top-to-bottom dependency order:
`StringList` → CSV loader → data model (`Value`/`Row`/`Table`) → type inference
→ tokenizer → parser/AST → executor (`Rel`) → REPL.

## Known limitations (deliberately out of scope)

- Read-only: no `INSERT`/`UPDATE`/`DELETE`, no persistence (indexes are
  in-memory per session; a data reload drops them).
- No `IS NULL` (use of `= NULL` is always unknown, per SQL — so it matches nothing).
- No subqueries, `DISTINCT`, `LIKE`, arithmetic in expressions, `OFFSET`, or
  positional `ORDER BY 1`.
- The planner reorders only all-INNER join chains (greedy, not exhaustive) and
  never reorders across a `LEFT JOIN`.

These are natural next extensions if you want to keep going.

## Correctness & memory

- `make test` runs 10 assertions (joins, LEFT join NULLs, GROUP/HAVING, quoted
  CSV, hash≡nested-loop equivalence, EXPLAIN).
- Cross-checked against `sqlite3` on 20k/100k-row datasets: identical results for
  aggregates, GROUP BY, INNER/LEFT joins, and 2/3-table joins.
- Clean under `-fsanitize=address,undefined` (no leaks or UB) on the large runs.
