#!/usr/bin/env bash
# Smoke test for minisql: feed scripts of commands, assert on the output.
set -u
cd "$(dirname "$0")/.."

make --no-print-directory minisql >/dev/null || { echo "BUILD FAILED"; exit 1; }

pass=0; fail=0
check() { if grep -qF "$2" <<<"$1"; then pass=$((pass+1)); else echo "MISSING: $2"; fail=$((fail+1)); fi; }

# --- core features on the small datasets ---------------------------------
got=$(./minisql <<'SQL'
.load data/artists.csv artists
.load data/albums.csv albums
.tables
SELECT a.name, COUNT(*) AS n FROM artists a JOIN albums b ON a.id = b.artist_id WHERE b.genre = 'Jazz' GROUP BY a.name ORDER BY n DESC, a.name;
SELECT b.album_id, b.title, a.name FROM albums b LEFT JOIN artists a ON b.artist_id = a.id ORDER BY b.album_id;
SELECT genre, COUNT(*) FROM albums GROUP BY genre HAVING COUNT(*) >= 2;
SELECT COUNT(*), MIN(album_id), MAX(album_id) FROM albums;
.quit
SQL
)
check "$got" 'loaded 3 rows into "artists"'
check "$got" "Miles Davis    2"
check "$got" "John Coltrane  2"
check "$got" "Bill Evans     1"
check "$got" "Orphan Album     NULL"
check "$got" "Jazz    5"
check "$got" "6      10   15"

# --- quoted CSV round-trips through the engine ---------------------------
gq=$(./minisql <<'SQL'
.load data/quoted.csv q
SELECT name, note FROM q WHERE id >= 2 ORDER BY name DESC;
SQL
)
check "$gq" 'Davis, Miles  two "quoted" words'

# --- hash and nested-loop joins must produce identical results -----------
jq="SELECT a.name, b.title FROM artists a JOIN albums b ON a.id = b.artist_id ORDER BY a.name, b.title;"
hj=$(printf '.load data/artists.csv artists\n.load data/albums.csv albums\n.join auto\n%s\n'   "$jq" | ./minisql 2>/dev/null | grep -v "join mode")
nj=$(printf '.load data/artists.csv artists\n.load data/albums.csv albums\n.join nested\n%s\n' "$jq" | ./minisql 2>/dev/null | grep -v "join mode")
if [ "$hj" = "$nj" ]; then pass=$((pass+1)); else echo "MISMATCH: hash vs nested join differ"; fail=$((fail+1)); fi

# --- EXPLAIN shows a hash join for an equijoin ---------------------------
pl=$(./minisql <<'SQL'
.load data/artists.csv artists
.load data/albums.csv albums
.plan SELECT a.name FROM artists a JOIN albums b ON a.id = b.artist_id
SQL
)
check "$pl" "[hash]"

# --- indexes must return the same answers as a full scan -----------------
scan_eq=$(printf ".load data/albums.csv a\nSELECT COUNT(*) FROM a WHERE genre = 'Jazz';\n" | ./minisql 2>/dev/null | awk '/^---/{f=1;next} f&&NF{print $1;exit}')
hash_eq=$(printf ".load data/albums.csv a\n.index a genre hash\nSELECT COUNT(*) FROM a WHERE genre = 'Jazz';\n" | ./minisql 2>/dev/null | awk '/^---/{f=1;next} f&&NF{print $1;exit}')
scan_rg=$(printf ".load data/albums.csv a\nSELECT COUNT(*) FROM a WHERE album_id >= 12;\n" | ./minisql 2>/dev/null | awk '/^---/{f=1;next} f&&NF{print $1;exit}')
sort_rg=$(printf ".load data/albums.csv a\n.index a album_id sorted\nSELECT COUNT(*) FROM a WHERE album_id >= 12;\n" | ./minisql 2>/dev/null | awk '/^---/{f=1;next} f&&NF{print $1;exit}')
if [ "$hash_eq" = "$scan_eq" ] && [ "$scan_eq" = "5" ]; then pass=$((pass+1)); else echo "MISMATCH: hash index eq ($hash_eq vs $scan_eq)"; fail=$((fail+1)); fi
if [ "$sort_rg" = "$scan_rg" ] && [ "$scan_rg" = "4" ]; then pass=$((pass+1)); else echo "MISMATCH: sorted index range ($sort_rg vs $scan_rg)"; fail=$((fail+1)); fi

# --- index join must match a plain join ----------------------------------
jj="SELECT COUNT(*) FROM artists a JOIN albums b ON a.id = b.artist_id WHERE a.id < 3;"
ij_no=$(printf ".load data/artists.csv artists\n.load data/albums.csv albums\n%s\n"                       "$jj" | ./minisql 2>/dev/null | awk '/^---/{f=1;next} f&&NF{print $1;exit}')
ij_ix=$(printf ".load data/artists.csv artists\n.load data/albums.csv albums\n.index albums artist_id hash\n%s\n" "$jj" | ./minisql 2>/dev/null | awk '/^---/{f=1;next} f&&NF{print $1;exit}')
if [ "$ij_no" = "$ij_ix" ] && [ "$ij_no" = "4" ]; then pass=$((pass+1)); else echo "MISMATCH: index join ($ij_ix vs $ij_no)"; fail=$((fail+1)); fi

echo "---"
if [ "$fail" -eq 0 ]; then echo "OK: $pass checks passed"; else echo "FAILED: $fail failed, $pass passed"; exit 1; fi
