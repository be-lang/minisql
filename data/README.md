# Test CSVs for the loader

A ladder of example files, easiest first. Make your loader climb it one rung at a
time. Each file isolates ONE thing the loader must handle.

## Step 1 — handle these now

### simple.csv
The "does anything work at all" file. 3 columns, 3 rows, no surprises.
- Expected schema: `id` (int), `name` (string), `age` (int)
- Expected: 3 rows loaded, prints back as a clean grid.

### mixed.csv
Forces real type inference: int, string, float, and a boolean-ish column.
- Expected: `id` (int), `name` (string), `height` (float), `active` (string —
  unless you decide to add a bool type; not required).
- Watch: `1.72` must infer as float, `1` as int. Decide your tie rule.

### empty_fields.csv
The empty-cell / NULL case. Note row 4 (`4,,`) has TWO empty fields.
- Expected: empty string between commas becomes a NULL value, NOT the number 0
  and NOT a crash. Row count is still 4, every row still has 3 fields.

## Step 1, second pass — RFC 4180 quoting (DONE)

### quoted.csv
Forced the rewrite of `split_line` from a naive comma-split into a small state
machine. Covers:
- quoted field containing a comma:  `"Lennon, John"`  — comma must NOT split.
- escaped quotes inside a quoted field: `"says ""hello"""` -> `says "hello"`.
- an empty quoted/plain field still becomes NULL.

Not yet supported (deferred, needs record-based reading instead of line-based):
- a quoted field containing an actual newline, spanning two physical lines.
