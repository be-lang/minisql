#!/usr/bin/env bash
# Stress test: real-world SQL that must produce the SAME answer as sqlite3.
# This is the anti-cheating harness — every query is checked against sqlite's
# result on the same data, not against a hardcoded string.
set -u
cd "$(dirname "$0")/.."
make --no-print-directory minisql >/dev/null || { echo BUILD FAILED; exit 1; }
[ -f bench/bench.db ] || { echo "run bench/bench.sh first to build bench.db"; exit 1; }

BIN=./minisql
# extract the first data value (scalar queries) from minisql's grid output
mscalar(){ printf ".load bench/customers.csv customers\n.load bench/orders.csv orders\n%s\n" "$1" | $BIN 2>/dev/null | awk '/^---/{f=1;next} f&&NF{print $1;exit}'; }
# full result as normalized rows: start after the '---' separator, stop at '(N rows)'
mrows(){ printf ".load bench/customers.csv customers\n.load bench/orders.csv orders\n%s\n" "$1" | $BIN 2>/dev/null \
         | awk 'seen{ if(/^\(/)exit; gsub(/  +/,","); sub(/,+$/,""); if(NF)print } /^---/{seen=1}'; }
srows(){ sqlite3 -separator , bench/bench.db "$1"; }

pass=0; fail=0
# scalar check
ck(){ local m s; m=$(mscalar "$1"); s=$(sqlite3 bench/bench.db "$2");
  if [ "$m" = "$s" ]; then pass=$((pass+1)); printf "OK    %-46s = %s\n" "$3" "$m";
  else fail=$((fail+1)); printf "FAIL  %-46s  minisql=%s sqlite=%s\n" "$3" "$m" "$s"; fi; }
# multi-row check (order-sensitive)
ckrows(){ local m s; m=$(mrows "$1"); s=$(srows "$2");
  if [ "$m" = "$s" ]; then pass=$((pass+1)); printf "OK    %-46s (%d rows)\n" "$3" "$(wc -l<<<"$s")";
  else fail=$((fail+1)); printf "FAIL  %-46s\n--- minisql:\n%s\n--- sqlite:\n%s\n" "$3" "$m" "$s"; fi; }

ck    "SELECT COUNT(DISTINCT city) FROM customers;"                  "SELECT COUNT(DISTINCT city) FROM customers;"          "COUNT(DISTINCT)"
ckrows "SELECT DISTINCT city FROM customers ORDER BY city;"          "SELECT DISTINCT city FROM customers ORDER BY city;"   "DISTINCT + ORDER BY"
ck    "SELECT COUNT(*) FROM customers WHERE city LIKE 'B%';"         "SELECT COUNT(*) FROM customers WHERE city LIKE 'B%';" "LIKE prefix"
ck    "SELECT COUNT(*) FROM customers WHERE city LIKE '%n';"         "SELECT COUNT(*) FROM customers WHERE city LIKE '%n';" "LIKE suffix"
ck    "SELECT COUNT(*) FROM customers WHERE city IN ('Bern','Basel');" "SELECT COUNT(*) FROM customers WHERE city IN ('Bern','Basel');" "IN list"
ck    "SELECT COUNT(*) FROM orders WHERE amount * 2 > 1900;"         "SELECT COUNT(*) FROM orders WHERE amount * 2 > 1900;" "arithmetic in WHERE"
ck    "SELECT COUNT(*) FROM orders WHERE amount BETWEEN 100 AND 200;" "SELECT COUNT(*) FROM orders WHERE amount BETWEEN 100 AND 200;" "BETWEEN"
ck    "SELECT COUNT(*) FROM customers WHERE UPPER(city) = 'BERN';"   "SELECT COUNT(*) FROM customers WHERE UPPER(city) = 'BERN';" "UPPER()"
ck    "SELECT COUNT(*) FROM customers WHERE LENGTH(city) = 4;"       "SELECT COUNT(*) FROM customers WHERE LENGTH(city) = 4;" "LENGTH()"
ck    "SELECT COUNT(*) FROM customers WHERE city IS NOT NULL;"       "SELECT COUNT(*) FROM customers WHERE city IS NOT NULL;" "IS NOT NULL"
ckrows "SELECT city, COUNT(*) FROM customers GROUP BY city ORDER BY 2 DESC, 1 LIMIT 3;" \
       "SELECT city, COUNT(*) FROM customers GROUP BY city ORDER BY 2 DESC, 1 LIMIT 3;" "positional ORDER BY"
ck    "SELECT SUM(amount + 1) FROM orders WHERE customer_id < 100;"  "SELECT SUM(amount + 1) FROM orders WHERE customer_id < 100;" "arithmetic in agg"
ck    "SELECT COUNT(*) FROM orders WHERE NOT amount > 500;"          "SELECT COUNT(*) FROM orders WHERE NOT amount > 500;"  "unary NOT"
ck    "SELECT COUNT(*) FROM customers WHERE city NOT IN ('Bern','Basel');" "SELECT COUNT(*) FROM customers WHERE city NOT IN ('Bern','Basel');" "NOT IN"
ck    "SELECT COUNT(*) FROM customers WHERE city NOT LIKE 'B%';"     "SELECT COUNT(*) FROM customers WHERE city NOT LIKE 'B%';" "NOT LIKE"
ck    "SELECT COUNT(*) FROM orders WHERE amount NOT BETWEEN 100 AND 900;" "SELECT COUNT(*) FROM orders WHERE amount NOT BETWEEN 100 AND 900;" "NOT BETWEEN"
ck    "SELECT COUNT(*) FROM customers WHERE LENGTH(UPPER(city)) = 6;" "SELECT COUNT(*) FROM customers WHERE LENGTH(UPPER(city)) = 6;" "nested functions"
ck    "SELECT COUNT(*) FROM orders WHERE amount + customer_id > 20000;" "SELECT COUNT(*) FROM orders WHERE amount + customer_id > 20000;" "column arithmetic"
ck    "SELECT COUNT(DISTINCT customer_id) FROM orders WHERE amount > 995;" "SELECT COUNT(DISTINCT customer_id) FROM orders WHERE amount > 995;" "COUNT(DISTINCT) + WHERE"

echo "---"
if [ "$fail" -eq 0 ]; then echo "STRESS OK: $pass/$((pass+fail)) real queries match sqlite"; else echo "STRESS FAILED: $fail of $((pass+fail)) differ"; exit 1; fi
