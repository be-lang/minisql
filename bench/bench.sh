#!/usr/bin/env bash
# Reproducible benchmark: hash vs nested-loop scaling, and minisql vs sqlite3.
# Requires python3 and sqlite3. Run from the minisql/ directory: ./bench/bench.sh
set -u
cd "$(dirname "$0")/.."
make --no-print-directory minisql >/dev/null || exit 1

timeit() { local s e; s=$(date +%s.%N); eval "$1" >/dev/null 2>&1; e=$(date +%s.%N);
           awk -v s=$s -v e=$e -v l="$2" 'BEGIN{printf "  %-44s %8.3f s\n", l, e-s}'; }

echo "### 1. Hash vs nested-loop join scaling (minisql)"
for N in 1000 2000 4000; do
  mkdir -p bench/sq$N; python3 bench/gen.py $N $N bench/sq$N >/dev/null
  q="SELECT COUNT(*) FROM customers c JOIN orders o ON c.id = o.customer_id;"
  printf ".load bench/sq%s/customers.csv customers\n.load bench/sq%s/orders.csv orders\n.join nested\n%s\n" $N $N "$q" > /tmp/n.sql
  printf ".load bench/sq%s/customers.csv customers\n.load bench/sq%s/orders.csv orders\n%s\n" $N $N "$q" > /tmp/h.sql
  echo "N=$N ($((N*N)) candidate pairs):"
  timeit "./minisql < /tmp/n.sql" "nested-loop"
  timeit "./minisql < /tmp/h.sql" "hash"
done

echo
echo "### 2. minisql vs sqlite3 on a 20k x 100k join+group+sum"
python3 bench/gen.py 20000 100000 bench >/dev/null
rm -f bench/bench.db
sqlite3 bench/bench.db >/dev/null <<'SQL'
CREATE TABLE customers(id INTEGER, name TEXT, city TEXT);
CREATE TABLE orders(order_id INTEGER, customer_id INTEGER, amount INTEGER);
.mode csv
.import --skip 1 bench/customers.csv customers
.import --skip 1 bench/orders.csv orders
SQL
Q="SELECT c.city, COUNT(*), SUM(o.amount) FROM customers c JOIN orders o ON c.id = o.customer_id GROUP BY c.city ORDER BY c.city;"
printf ".load bench/customers.csv customers\n.load bench/orders.csv orders\n%s\n" "$Q" > /tmp/m.sql
printf ".load bench/customers.csv customers\n.load bench/orders.csv orders\n" > /tmp/load.sql
timeit "./minisql < /tmp/m.sql"          "minisql (parse CSV + join + group)"
timeit "./minisql < /tmp/load.sql"       "  of which: CSV parsing only"
timeit "sqlite3 bench/bench.db \"$Q\""    "sqlite3 warm (pre-parsed db)"

echo
echo "### 3. Selective point lookup: WHERE customer_id = 12345"
printf ".load bench/customers.csv customers\n.load bench/orders.csv orders\nSELECT COUNT(*) FROM orders WHERE customer_id = 12345;\n" > /tmp/sel.sql
sqlite3 bench/bench.db "CREATE INDEX IF NOT EXISTS idx_o ON orders(customer_id);" >/dev/null
timeit "./minisql < /tmp/sel.sql"                                             "minisql (full scan)"
timeit "sqlite3 bench/bench.db 'SELECT COUNT(*) FROM orders WHERE customer_id=12345;'" "sqlite3 (indexed lookup)"
