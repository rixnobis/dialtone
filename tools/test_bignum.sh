#!/bin/sh
# host unit test for src/bignum.cpp; prints "BIGNUM <n> checks, 0 failed" and exits 0 on success
set -e
ROOT=$(cd "$(dirname "$0")/.." && pwd)
OUT=${TMPDIR:-/tmp}/dialtone-bignum-test
g++ -std=c++20 -O2 -Wall -o "$OUT" "$ROOT/tests/bignum_test.cpp" "$ROOT/src/bignum.cpp"
"$OUT"
