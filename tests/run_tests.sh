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

for fc_file in $(find "$TESTDIR" -name "*.fc" ! -name "*_part2.fc" | sort); do
    test_name=$(basename "$fc_file" .fc)
    test_dir=$(dirname "$fc_file")

    # Display name: path relative to TESTDIR, without .fc extension
    test_display=$(realpath --relative-to="$TESTDIR" "$fc_file" | sed 's/\.fc$//')

    c_file="$TMPDIR/${test_display//\//_}.c"
    bin_file="$TMPDIR/${test_display//\//_}"

    # Check for companion files (multi-file test)
    fc_files="$fc_file"
    if [ -f "$test_dir/${test_name}_part2.fc" ]; then
        fc_files="$fc_file $test_dir/${test_name}_part2.fc"
    fi

    # Compile FC -> C
    if ! $FC $fc_files -o "$c_file" 2>"$TMPDIR/${test_display//\//_}.stderr"; then
        # Check if this is an expected error test
        if [ -f "$test_dir/${test_name}.error" ]; then
            expected_error=$(cat "$test_dir/${test_name}.error")
            actual_error=$(cat "$TMPDIR/${test_display//\//_}.stderr")
            if echo "$actual_error" | grep -qF "$expected_error"; then
                echo "  PASS  $test_display (expected error)"
                passed=$((passed + 1))
                continue
            else
                echo "  FAIL  $test_display (wrong error)"
                echo "    expected: $expected_error"
                echo "    got: $actual_error"
                failed=$((failed + 1))
                errors="$errors  $test_display\n"
                continue
            fi
        fi
        echo "  FAIL  $test_display (fc compilation failed)"
        cat "$TMPDIR/${test_display//\//_}.stderr"
        failed=$((failed + 1))
        errors="$errors  $test_display\n"
        continue
    fi

    # Compile C -> binary
    if ! "$CC" -std=c11 -Wall -Werror -o "$bin_file" "$c_file" 2>"$TMPDIR/${test_display//\//_}.cc_stderr"; then
        echo "  FAIL  $test_display (C compilation failed)"
        cat "$TMPDIR/${test_display//\//_}.cc_stderr"
        failed=$((failed + 1))
        errors="$errors  $test_display\n"
        continue
    fi

    # Check expected exit code
    if [ -f "$test_dir/${test_name}.expected_exit" ]; then
        expected_exit=$(cat "$test_dir/${test_name}.expected_exit" | tr -d '[:space:]')
        set +e
        "$bin_file" > "$TMPDIR/${test_display//\//_}.stdout" 2>&1
        actual_exit=$?
        set -e
        if [ "$actual_exit" != "$expected_exit" ]; then
            echo "  FAIL  $test_display (exit code: expected $expected_exit, got $actual_exit)"
            failed=$((failed + 1))
            errors="$errors  $test_display\n"
            continue
        fi
    fi

    # Check expected stdout
    if [ -f "$test_dir/${test_name}.expected" ]; then
        set +e
        "$bin_file" > "$TMPDIR/${test_display//\//_}.stdout" 2>&1
        set -e
        if ! diff -u "$test_dir/${test_name}.expected" "$TMPDIR/${test_display//\//_}.stdout" > "$TMPDIR/${test_display//\//_}.diff" 2>&1; then
            echo "  FAIL  $test_display (output mismatch)"
            cat "$TMPDIR/${test_display//\//_}.diff"
            failed=$((failed + 1))
            errors="$errors  $test_display\n"
            continue
        fi
    fi

    echo "  PASS  $test_display"
    passed=$((passed + 1))
done

echo ""
echo "$passed passed, $failed failed"

if [ $failed -gt 0 ]; then
    echo -e "Failed tests:\n$errors"
    exit 1
fi
