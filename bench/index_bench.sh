#!/usr/bin/env bash
# Index experiment: hash vs sorted vs none vs auto, on 2000-query workloads.
# Run from minisql/:  ./bench/index_bench.sh   (needs python3, sqlite3)
set -u
cd "$(dirname "$0")/.."
make --no-print-directory minisql >/dev/null || exit 1
[ -f bench/orders.csv ] || python3 bench/gen.py 20000 100000 bench >/dev/null

python3 - <<'PY'
import random; random.seed(1)
open('/tmp/pt.sql','w').write("".join(f"SELECT COUNT(*) FROM orders WHERE customer_id = {random.randint(1,20000)};\n" for _ in range(2000)))
open('/tmp/rg.sql','w').write("".join(f"SELECT COUNT(*) FROM orders WHERE amount > {random.randint(1,1000)};\n" for _ in range(2000)))
open('/tmp/rs.sql','w').write("".join(f"SELECT COUNT(*) FROM orders WHERE amount > {random.randint(990,999)};\n" for _ in range(2000)))
PY
L=".load bench/orders.csv orders"
t(){ local s e; s=$(date +%s.%N); eval "$1" >/dev/null 2>&1; e=$(date +%s.%N); awk -v s=$s -v e=$e -v l="$2" 'BEGIN{printf "  %-40s %8.3f s\n",l,e-s}'; }

echo "### A. Point lookup  (2000x  customer_id = ?)  -- hash vs sorted vs none vs auto"
t "{ echo '$L'; cat /tmp/pt.sql; } | ./minisql"                                        "no index (full scans)"
t "{ echo '$L'; echo '.index orders customer_id hash';   cat /tmp/pt.sql; } | ./minisql" "hash index"
t "{ echo '$L'; echo '.index orders customer_id sorted'; cat /tmp/pt.sql; } | ./minisql" "sorted index"
t "{ echo '$L'; echo '.autoindex on';                    cat /tmp/pt.sql; } | ./minisql" "auto index"

echo "### B. Non-selective range  (2000x  amount > ~500, ~half the rows)"
t "{ echo '$L'; cat /tmp/rg.sql; } | ./minisql"                                        "no index"
t "{ echo '$L'; echo '.index orders amount sorted'; cat /tmp/rg.sql; } | ./minisql"    "sorted index"

echo "### C. Selective range  (2000x  amount > ~995, ~1% of rows)"
t "{ echo '$L'; cat /tmp/rs.sql; } | ./minisql"                                        "no index"
t "{ echo '$L'; echo '.index orders amount sorted'; cat /tmp/rs.sql; } | ./minisql"    "sorted index"

echo "### D. One-time build cost (100k rows)"
t "{ echo '$L'; } | ./minisql"                                                         "load only"
t "{ echo '$L'; echo '.index orders customer_id hash';   } | ./minisql"                "+ build hash"
t "{ echo '$L'; echo '.index orders customer_id sorted'; } | ./minisql"                "+ build sorted"

echo "### E. INDEX JOIN: 200x selective join (small filtered left x big right)"
python3 - <<'PY'
open('/tmp/sj.sql','w').write("SELECT COUNT(*) FROM customers c JOIN orders o ON c.id=o.customer_id WHERE c.id < 100;\n"*200)
PY
LC=".load bench/customers.csv customers"
t "{ echo '$LC'; echo '$L'; cat /tmp/sj.sql; } | ./minisql"                    "hash join (copies 100k right)"
t "{ echo '$LC'; echo '$L'; echo '.autoindex on'; cat /tmp/sj.sql; } | ./minisql" "index join (probe, no copy)"
