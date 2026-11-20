#!/usr/bin/env bash

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../../.." && pwd)"
TOOLS="$REPO/tools"
RUNTIME_SPU="$REPO/runtime/spu"

GCC="${1:-${GCC:-gcc}}"

pass=0
fail=0
failed_tests=()

for gen in "$HERE"/gen_test_*.py; do
    name=$(basename "$gen" .py)
    name=${name#gen_test_}
    main="$HERE/test_${name}_main.c"
    [ -f "$main" ] || { echo "[skip] $name: no test_${name}_main.c"; continue; }

    elf="$HERE/test_${name}.elf"
    gen_out="$HERE/out_${name}"
    exe="$HERE/test_${name}.exe"

    rm -rf "$gen_out"
    python "$gen" > /dev/null
    python "$TOOLS/spu_lifter.py" --auto-functions "$elf" \
        --output "$gen_out" > /dev/null

    EXTRA=""
    [ -f "$HERE/test_${name}.deps" ] && . "$HERE/test_${name}.deps"

    if "$GCC" -std=c11 -O2 -I "$gen_out" -I "$RUNTIME_SPU" \
        "$gen_out/spu_recomp.c" "$main" $EXTRA -o "$exe" \
        2> "$HERE/build_${name}.log"; then
        out=$("$exe" 2>&1) && rc=$? || rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "[PASS] $name -- $out"
            pass=$((pass+1))
        else
            echo "[FAIL] $name (exit $rc)"
            echo "$out" | sed 's/^/   /'
            failed_tests+=("$name")
            fail=$((fail+1))
        fi
    else
        echo "[FAIL] $name (build error -- see build_${name}.log)"
        failed_tests+=("$name")
        fail=$((fail+1))
    fi
done

echo
echo "==========================================="
echo "Results: $pass passed, $fail failed"
[ $fail -eq 0 ] || { echo "Failed: ${failed_tests[*]}"; exit 1; }
