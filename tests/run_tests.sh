#!/bin/bash
set -e

FC="./fc"
CC="${CC:-cc}"
TESTDIR="tests/cases"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

passed=0
failed=0
errors=""

for fc_file in "$TESTDIR"/*.fc; do
    test_name=$(basename "$fc_file" .fc)

    # Skip _part2 files — they're companion files for multi-file tests
    if [[ "$test_name" == *_part2 ]]; then
        continue
    fi

    c_file="$TMPDIR/${test_name}.c"
    bin_file="$TMPDIR/${test_name}"

    # Check for companion files (multi-file test)
    fc_files="$fc_file"
    if [ -f "$TESTDIR/${test_name}_part2.fc" ]; then
        fc_files="$fc_file $TESTDIR/${test_name}_part2.fc"
    fi

    # Compile FC -> C
    if ! $FC $fc_files -o "$c_file" 2>"$TMPDIR/${test_name}.stderr"; then
        # Check if this is an expected error test
        if [ -f "$TESTDIR/${test_name}.error" ]; then
            expected_error=$(cat "$TESTDIR/${test_name}.error")
            actual_error=$(cat "$TMPDIR/${test_name}.stderr")
            if echo "$actual_error" | grep -qF "$expected_error"; then
                echo "  PASS  $test_name (expected error)"
                passed=$((passed + 1))
                continue
            else
                echo "  FAIL  $test_name (wrong error)"
                echo "    expected: $expected_error"
                echo "    got: $actual_error"
                failed=$((failed + 1))
                errors="$errors  $test_name\n"
                continue
            fi
        fi
        echo "  FAIL  $test_name (fc compilation failed)"
        cat "$TMPDIR/${test_name}.stderr"
        failed=$((failed + 1))
        errors="$errors  $test_name\n"
        continue
    fi

    # Compile C -> binary
    if ! "$CC" -std=c11 -Wall -Werror -o "$bin_file" "$c_file" 2>"$TMPDIR/${test_name}.cc_stderr"; then
        echo "  FAIL  $test_name (C compilation failed)"
        cat "$TMPDIR/${test_name}.cc_stderr"
        failed=$((failed + 1))
        errors="$errors  $test_name\n"
        continue
    fi

    # Check expected exit code
    if [ -f "$TESTDIR/${test_name}.expected_exit" ]; then
        expected_exit=$(cat "$TESTDIR/${test_name}.expected_exit" | tr -d '[:space:]')
        set +e
        "$bin_file" > "$TMPDIR/${test_name}.stdout" 2>&1
        actual_exit=$?
        set -e
        if [ "$actual_exit" != "$expected_exit" ]; then
            echo "  FAIL  $test_name (exit code: expected $expected_exit, got $actual_exit)"
            failed=$((failed + 1))
            errors="$errors  $test_name\n"
            continue
        fi
    fi

    # Check expected stdout
    if [ -f "$TESTDIR/${test_name}.expected" ]; then
        set +e
        "$bin_file" > "$TMPDIR/${test_name}.stdout" 2>&1
        set -e
        if ! diff -u "$TESTDIR/${test_name}.expected" "$TMPDIR/${test_name}.stdout" > "$TMPDIR/${test_name}.diff" 2>&1; then
            echo "  FAIL  $test_name (output mismatch)"
            cat "$TMPDIR/${test_name}.diff"
            failed=$((failed + 1))
            errors="$errors  $test_name\n"
            continue
        fi
    fi

    echo "  PASS  $test_name"
    passed=$((passed + 1))
done

echo ""
echo "$passed passed, $failed failed"

if [ $failed -gt 0 ]; then
    echo -e "Failed tests:\n$errors"
    exit 1
fi
