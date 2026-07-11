#!/usr/bin/env bash
# Regression tests: every check here reproduces a bug that once existed.
# Run against a candidate binary with:  BIN=./some_binary tests/regressions.sh
set -u
cd "$(dirname "$0")/.."
BIN="${BIN:-./minisql}"
[ -x "$BIN" ] || { echo "no binary at $BIN"; exit 1; }

pass=0; fail=0
check() { if grep -qF "$2" <<<"$1"; then pass=$((pass+1)); else echo "MISSING [$3]: $2"; fail=$((fail+1)); fi; }
# first data cell after the header separator
cell1() { awk '/^---/{f=1;next} f&&NF{print $1;exit}'; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# --- CRITICAL: join reordering with unqualified ON columns ----------------
# Planner used to reorder a table before its join key existed; the key then
# evaluated to NULL and the join silently returned 0 rows.
printf 'x\n1\n2\n3\n' > "$tmp/a.csv"
printf 'y\n1\n2\n3\n'  > "$tmp/b.csv"
printf 'z\n1\n'        > "$tmp/c.csv"   # smallest table: tempts the greedy planner to join c first
got=$(printf '.load %s a\n.load %s b\n.load %s c\nSELECT COUNT(*) FROM a JOIN b ON x = y JOIN c ON y = z;\n' \
      "$tmp/a.csv" "$tmp/b.csv" "$tmp/c.csv" | "$BIN" 2>&1 | cell1)
if [ "$got" = "1" ]; then pass=$((pass+1)); else echo "WRONG [join-reorder-unqualified]: got '$got' want 1"; fail=$((fail+1)); fi

# --- 70 joins must not smash the fixed 64-entry planner arrays ------------
printf 'id\n1\n2\n' > "$tmp/t.csv"
q="SELECT COUNT(*) FROM t t0"
for i in $(seq 1 70); do q="$q JOIN t t$i ON t$((i-1)).id = t$i.id"; done
got=$(printf '.load %s t\n%s;\n' "$tmp/t.csv" "$q" | "$BIN" 2>&1); rc=$?
n=$(cell1 <<<"$got")
if [ $rc -eq 0 ] && [ "$n" = "2" ]; then pass=$((pass+1)); else echo "WRONG [70-joins]: rc=$rc got '$n' want 2"; fail=$((fail+1)); fi

# --- hash index must agree with a scan on cross-type equality -------------
noidx=$(printf '.load data/simple.csv s\nSELECT COUNT(*) FROM s WHERE id = 2.0;\n' | "$BIN" 2>&1 | cell1)
widx=$(printf '.load data/simple.csv s\n.index s id hash\nSELECT COUNT(*) FROM s WHERE id = 2.0;\n' | "$BIN" 2>&1 | cell1)
if [ "$noidx" = "1" ] && [ "$widx" = "1" ]; then pass=$((pass+1)); else echo "WRONG [hash-crosstype]: scan=$noidx idx=$widx want 1/1"; fail=$((fail+1)); fi

# --- SELECT aliases in WHERE/HAVING must error, not silently NULL ---------
gw=$(printf ".load data/simple.csv s\nSELECT name AS n FROM s WHERE n = 'Alice';\n" | "$BIN" 2>&1)
check "$gw" "no such column: n" alias-in-where
gh=$(printf ".load data/simple.csv s\nSELECT name, COUNT(*) AS cnt FROM s GROUP BY name HAVING cnt >= 1;\n" | "$BIN" 2>&1)
check "$gh" "no such column: cnt" alias-in-having
go=$(printf ".load data/simple.csv s\nSELECT name AS n FROM s ORDER BY n;\n" | "$BIN" 2>&1)
check "$go" "Alice" alias-in-orderby-still-works

# --- integer overflow promotes to double instead of UB --------------------
printf 'k\n1\n' > "$tmp/one.csv"
ld=".load $tmp/one.csv one"
gp=$(printf '%s\nSELECT 9223372036854775807 + 1 FROM one;\n' "$ld" | "$BIN" 2>&1);  check "$gp" "9.22337e+18" overflow-add
gm=$(printf '%s\nSELECT 9223372036854775806 * 3 FROM one;\n' "$ld" | "$BIN" 2>&1);  check "$gm" "2.76701e+19" overflow-mul
gn=$(printf '%s\nSELECT -(-9223372036854775807 - 1) FROM one;\n' "$ld" | "$BIN" 2>&1); check "$gn" "9.22337e+18" overflow-negate
gd=$(printf '%s\nSELECT (-9223372036854775807 - 1) / -1 FROM one;\n' "$ld" | "$BIN" 2>&1); check "$gd" "9.22337e+18" overflow-div

# --- NOT IN with a NULL in the list is UNKNOWN (matches sqlite) ------------
gni=$(printf '.load data/simple.csv s\nSELECT COUNT(*) FROM s WHERE id NOT IN (1, NULL);\n' | "$BIN" 2>&1 | cell1)
if [ "$gni" = "0" ]; then pass=$((pass+1)); else echo "WRONG [not-in-null]: got '$gni' want 0"; fail=$((fail+1)); fi

# --- ORDER BY on a non-output column errors instead of silently ignoring --
gob=$(printf '.load data/simple.csv s\nSELECT name FROM s ORDER BY age;\n' | "$BIN" 2>&1)
check "$gob" "not a selected column" orderby-nonoutput

# --- unterminated string literal is a tokenizer error ----------------------
gus=$(printf "SELECT 'abc\n" | "$BIN" 2>&1)
check "$gus" "unterminated string literal" unterminated-string

# --- deeply nested parens error gracefully (no stack overflow) -------------
deep=$(python3 -c "print('SELECT ' + '('*5000 + '1' + ')'*5000 + ';')")
gdn=$(printf '%s\n' "$deep" | "$BIN" 2>&1); rc=$?
if [ $rc -eq 0 ] && grep -qF "nested too deeply" <<<"$gdn"; then pass=$((pass+1)); else echo "WRONG [deep-nesting]: rc=$rc"; fail=$((fail+1)); fi

# --- typo'd index type errors instead of silently building hash ------------
gt=$(printf '.load data/simple.csv s\n.index s id sroted\n' | "$BIN" 2>&1)
check "$gt" "unknown index type" index-typo

# --- LIKE must not backtrack exponentially ---------------------------------
printf 's\n%s\n' "$(python3 -c "print('a'*45)")" > "$tmp/like.csv"
gl=$(timeout 5 bash -c "printf '.load $tmp/like.csv l\nSELECT COUNT(*) FROM l WHERE s LIKE '\''%%a%%a%%a%%a%%a%%a%%a%%a%%a%%a%%a%%a%%b'\'';\n' | '$BIN' 2>&1" | cell1)
if [ "$gl" = "0" ]; then pass=$((pass+1)); else echo "WRONG [like-backtrack]: got '$gl' want 0 (within 5s)"; fail=$((fail+1)); fi

# --- SUM of big ints stays exact (no silent double rounding) ---------------
printf 'v\n9007199254740993\n2\n' > "$tmp/big.csv"
gs=$(printf '.load %s b\nSELECT SUM(v) FROM b;\n' "$tmp/big.csv" | "$BIN" 2>&1 | cell1)
if [ "$gs" = "9007199254740995" ]; then pass=$((pass+1)); else echo "WRONG [sum-precision]: got '$gs' want 9007199254740995"; fail=$((fail+1)); fi

# --- SUM overflow promotes to double instead of UB --------------------------
printf 'v\n9223372036854775806\n9223372036854775806\n' > "$tmp/big2.csv"
gso=$(printf '.load %s b\nSELECT SUM(v) FROM b;\n' "$tmp/big2.csv" | "$BIN" 2>&1)
check "$gso" "1.84467e+19" sum-overflow

# --- NOT over AND/OR must propagate NULL (three-valued logic) ---------------
# rows: (n=1,f=1) (n=NULL,f=0) (n=NULL,f=NULL)
printf 'i,n,f\n1,1,1\n2,,0\n3,,\n' > "$tmp/tvl.csv"
g3o=$(printf '.load %s t\nSELECT COUNT(*) FROM t WHERE NOT (n = 1 OR f = 1);\n' "$tmp/tvl.csv" | "$BIN" 2>&1 | cell1)
if [ "$g3o" = "0" ]; then pass=$((pass+1)); else echo "WRONG [3vl-not-or]: got '$g3o' want 0"; fail=$((fail+1)); fi
g3a=$(printf '.load %s t\nSELECT COUNT(*) FROM t WHERE NOT (n = 1 AND f = 1);\n' "$tmp/tvl.csv" | "$BIN" 2>&1 | cell1)
if [ "$g3a" = "1" ]; then pass=$((pass+1)); else echo "WRONG [3vl-not-and]: got '$g3a' want 1"; fail=$((fail+1)); fi

# --- ABS(LONG_MIN) promotes to double instead of UB --------------------------
printf 'v\n-9223372036854775808\n' > "$tmp/lmin.csv"
gab=$(printf '.load %s m\nSELECT ABS(v) FROM m;\n' "$tmp/lmin.csv" | "$BIN" 2>&1)
check "$gab" "9.22337e+18" abs-longmin

# --- ROUND: huge precision must not hang, big values must not wrap ----------
grh=$(timeout 5 bash -c "printf '.load $tmp/lmin.csv m\nSELECT ROUND(1.5, 2000000000) FROM m;\n' | '$BIN' 2>&1")
check "$grh" "1.5" round-huge-precision
gro=$(printf '.load %s m\nSELECT ROUND(1000000000000000000.0, 5) FROM m;\n' "$tmp/lmin.csv" | "$BIN" 2>&1)
check "$gro" "1e+18" round-big-value
grt=$(printf '.load %s m\nSELECT ROUND(2.675, 2) FROM m;\n' "$tmp/lmin.csv" | "$BIN" 2>&1)
check "$grt" "2.67" round-decimal-tie   # 2.675 is really 2.67499… — sqlite says 2.67
grf=$(printf '.load %s m\nSELECT ROUND(2.5) FROM m;\n' "$tmp/lmin.csv" | "$BIN" 2>&1 | cell1)
if [ "$grf" = "3" ]; then pass=$((pass+1)); else echo "WRONG [round-half-up]: got '$grf' want 3"; fail=$((fail+1)); fi

# --- depth guard must also cover function args and IN-list elements ---------
gfa=$(python3 -c "print('.load $tmp/one.csv one'); print('SELECT ' + 'abs('*50000 + '1' + ')'*50000 + ' FROM one;')" | "$BIN" 2>&1); rc=$?
if [ $rc -eq 0 ] && grep -qF "nested too deeply" <<<"$gfa"; then pass=$((pass+1)); else echo "WRONG [deep-func-args]: rc=$rc"; fail=$((fail+1)); fi
gin=$(python3 -c "print('.load $tmp/one.csv one'); print('SELECT ' + '1 IN ('*20000 + '1' + ')'*20000 + ' FROM one;')" | "$BIN" 2>&1); rc=$?
if [ $rc -eq 0 ] && grep -qF "nested too deeply" <<<"$gin"; then pass=$((pass+1)); else echo "WRONG [deep-in-list]: rc=$rc"; fail=$((fail+1)); fi

# --- .index kind is case-insensitive like every other keyword ---------------
gik=$(printf '.load data/simple.csv s\n.index s id HASH\n' | "$BIN" 2>&1)
if grep -qF "unknown index type" <<<"$gik"; then echo "WRONG [index-kind-case]: HASH rejected"; fail=$((fail+1)); else pass=$((pass+1)); fi

# --- extra CSV fields warn instead of vanishing -----------------------------
printf 'id,name\n1,Alice\n2,Bob,EXTRA\n' > "$tmp/ragged.csv"
gr=$(printf '.load %s r\n' "$tmp/ragged.csv" | "$BIN" 2>&1)
check "$gr" "expected 2" csv-extra-fields

echo "---"
if [ "$fail" -eq 0 ]; then echo "REGRESSIONS OK: $pass checks passed"; else echo "REGRESSIONS FAILED: $fail failed, $pass passed"; exit 1; fi
