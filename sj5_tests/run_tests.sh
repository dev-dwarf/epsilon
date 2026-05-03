#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TESTS_DIR="$SCRIPT_DIR/json5-tests"
CHECK="$SCRIPT_DIR/sj5_check"

# Build the checker
gcc -I/home/lf/lf -I/home/lf/epsilon \
    -o "$CHECK" "$SCRIPT_DIR/sj5_check.c" \
    -std=c11 -w 2>&1

pass=0
fail=0
unexpected_pass=0   # should-fail but succeeded
unexpected_fail=0   # should-pass but failed

declare -a unexpected_pass_list=()
declare -a unexpected_fail_list=()

run_test() {
    local file="$1"
    local expect="$2"   # "pass" or "fail"
    local rel="${file#$TESTS_DIR/}"

    local output rc=0
    output=$("$CHECK" "$file" 2>&1) || rc=$?

    if [ "$expect" = "pass" ]; then
        if [ $rc -eq 0 ]; then
            pass=$((pass + 1))
        else
            unexpected_fail=$((unexpected_fail + 1))
            unexpected_fail_list+=("$rel: $output")
        fi
    else
        if [ $rc -ne 0 ]; then
            fail=$((fail + 1))
        else
            unexpected_pass=$((unexpected_pass + 1))
            unexpected_pass_list+=("$rel")
        fi
    fi
}

while IFS= read -r -d '' f; do
    ext="${f##*.}"
    case "$ext" in
        json|json5)  run_test "$f" pass ;;
        js|txt)      run_test "$f" fail ;;
        # skip non-test files
    esac
done < <(find "$TESTS_DIR" -type f ! -path '*/.git/*' -print0 | sort -z)

total=$((pass + fail + unexpected_pass + unexpected_fail))

echo "=== sj5 JSON5 compliance report ==="
echo "Total tests: $total"
echo "Expected pass, passed:  $pass"
echo "Expected fail, failed:  $fail"
echo ""

if [ ${#unexpected_fail_list[@]} -gt 0 ]; then
    echo "--- SHOULD PASS but FAILED ($unexpected_fail) ---"
    for s in "${unexpected_fail_list[@]}"; do
        echo "  FAIL: $s"
    done
    echo ""
fi

if [ ${#unexpected_pass_list[@]} -gt 0 ]; then
    echo "--- SHOULD FAIL but PASSED ($unexpected_pass) ---"
    for s in "${unexpected_pass_list[@]}"; do
        echo "  PASS: $s"
    done
    echo ""
fi

if [ $unexpected_fail -eq 0 ] && [ $unexpected_pass -eq 0 ]; then
    echo "Fully compliant!"
fi
