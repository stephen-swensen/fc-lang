#!/bin/bash
set -e

FC="./fc"
CC="${CC:-cc}"
TESTDIR="tests/cases"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

start_time=$(date +%s%N)
JOBS="${JOBS:-$(nproc)}"

# run_one_test outputs a single line: PASS or FAIL with details.
# Each test writes to its own temp files keyed by slug, so no conflicts.
run_one_test() {
    local test_display="$1"
    local fc_files="$2"
    local error_file="$3"
    local expected_exit_file="$4"
    local expected_file="$5"
    local fc_flags="$6"

    local slug="${test_display//\//_}"
    local c_file="$TMPDIR/${slug}.c"
    local bin_file="$TMPDIR/${slug}"

    # Compile FC -> C
    if ! $FC $fc_files $fc_flags -o "$c_file" 2>"$TMPDIR/${slug}.stderr"; then
        if [ -n "$error_file" ] && [ -f "$error_file" ]; then
            local expected_error=$(cat "$error_file")
            local actual_error=$(cat "$TMPDIR/${slug}.stderr")
            if echo "$actual_error" | grep -qF "$expected_error"; then
                echo "PASS  $test_display (expected error)"
                return
            else
                echo "FAIL  $test_display (wrong error)"
                echo "    expected: $expected_error"
                echo "    got: $actual_error"
                return
            fi
        fi
        echo "FAIL  $test_display (fc compilation failed)"
        cat "$TMPDIR/${slug}.stderr" >&2
        return
    fi

    # If an .error file exists but compilation succeeded, that's a failure
    if [ -n "$error_file" ] && [ -f "$error_file" ]; then
        echo "FAIL  $test_display (expected error but compilation succeeded)"
        return
    fi

    # Compile C -> binary
    if ! "$CC" -std=c11 -Wall -Werror -o "$bin_file" "$c_file" 2>"$TMPDIR/${slug}.cc_stderr"; then
        echo "FAIL  $test_display (C compilation failed)"
        cat "$TMPDIR/${slug}.cc_stderr" >&2
        return
    fi

    # Check expected exit code
    if [ -n "$expected_exit_file" ] && [ -f "$expected_exit_file" ]; then
        local expected_exit=$(cat "$expected_exit_file" | tr -d '[:space:]')
        set +e
        "$bin_file" > "$TMPDIR/${slug}.stdout" 2>&1
        local actual_exit=$?
        set -e
        if [ "$actual_exit" != "$expected_exit" ]; then
            echo "FAIL  $test_display (exit code: expected $expected_exit, got $actual_exit)"
            return
        fi
    fi

    # Check expected stdout
    if [ -n "$expected_file" ] && [ -f "$expected_file" ]; then
        set +e
        "$bin_file" > "$TMPDIR/${slug}.stdout" 2>&1
        set -e
        if ! diff -u "$expected_file" "$TMPDIR/${slug}.stdout" > "$TMPDIR/${slug}.diff" 2>&1; then
            echo "FAIL  $test_display (output mismatch)"
            cat "$TMPDIR/${slug}.diff" >&2
            return
        fi
    fi

    echo "PASS  $test_display"
}

export -f run_one_test
export FC CC TMPDIR

# Build the list of tests (one per line: display|fc_files|error|exit|expected|flags)
test_list="$TMPDIR/test_list"

for milestone_dir in "$TESTDIR"/*/; do
    milestone=$(basename "$milestone_dir")

    for fc_file in "$milestone_dir"*.fc; do
        [ -f "$fc_file" ] || continue
        test_name=$(basename "$fc_file" .fc)
        echo "$milestone/$test_name|$fc_file|${milestone_dir}${test_name}.error|${milestone_dir}${test_name}.expected_exit|${milestone_dir}${test_name}.expected|"
    done

    for test_subdir in "$milestone_dir"*/; do
        [ -d "$test_subdir" ] || continue
        test_name=$(basename "$test_subdir")
        fc_files=$(find "$test_subdir" -name "*.fc" | sort | tr '\n' ' ')
        [ -n "$fc_files" ] || continue

        if [ -f "${test_subdir}deps" ]; then
            while IFS= read -r dep; do
                [ -n "$dep" ] || continue
                fc_files="$fc_files $dep"
            done < "${test_subdir}deps"
        fi

        fc_flags=""
        if [ -f "${test_subdir}flags" ]; then
            while IFS= read -r flag; do
                [ -n "$flag" ] || continue
                fc_flags="$fc_flags --flag $flag"
            done < "${test_subdir}flags"
        fi

        echo "$milestone/$test_name|$fc_files|${test_subdir}error|${test_subdir}expected_exit|${test_subdir}expected|$fc_flags"
    done
done > "$test_list"

# Run tests in parallel, collect output
results=$(cat "$test_list" | xargs -P "$JOBS" -I {} bash -c '
    IFS="|" read -r display files err exit exp flags <<< "{}"
    run_one_test "$display" "$files" "$err" "$exit" "$exp" "$flags"
')

# Tally results
passed=0
failed=0
fail_lines=""

while IFS= read -r line; do
    [ -n "$line" ] || continue
    if [[ "$line" == PASS* ]]; then
        echo "  $line"
        passed=$((passed + 1))
    elif [[ "$line" == FAIL* ]]; then
        echo "  $line"
        failed=$((failed + 1))
        fail_lines="$fail_lines  $line\n"
    else
        # Detail lines (indented error output)
        echo "$line"
    fi
done <<< "$results"

elapsed_ms=$(( ($(date +%s%N) - start_time) / 1000000 ))
elapsed_s=$(( elapsed_ms / 1000 ))
elapsed_frac=$(( elapsed_ms % 1000 ))

echo ""
printf "%d passed, %d failed in %d.%03ds\n" "$passed" "$failed" "$elapsed_s" "$elapsed_frac"

if [ $failed -gt 0 ]; then
    echo -e "Failed tests:\n$fail_lines"
    exit 1
fi
