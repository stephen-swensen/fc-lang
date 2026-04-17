#!/bin/bash
set -e
ulimit -c 0

FC="./fc"
CC="${CC:-cc}"
TESTDIR="tests/cases"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# On Windows (MSYS2/MinGW), net.fc uses Winsock and needs -lws2_32. Adding it
# unconditionally is harmless on tests that don't pull in winsock symbols.
EXTRA_LIBS=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXTRA_LIBS="-lws2_32" ;;
esac

start_time=$(date +%s%N)
passed=0
failed=0
errors=""

run_test() {
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
                echo "  PASS  $test_display (expected error)"
                passed=$((passed + 1))
                return
            else
                echo "  FAIL  $test_display (wrong error)"
                echo "    expected: $expected_error"
                echo "    got: $actual_error"
                failed=$((failed + 1))
                errors="$errors  $test_display\n"
                return
            fi
        fi
        echo "  FAIL  $test_display (fc compilation failed)"
        cat "$TMPDIR/${slug}.stderr"
        failed=$((failed + 1))
        errors="$errors  $test_display\n"
        return
    fi

    # If an .error file exists but compilation succeeded, that's a failure
    if [ -n "$error_file" ] && [ -f "$error_file" ]; then
        echo "  FAIL  $test_display (expected error but compilation succeeded)"
        failed=$((failed + 1))
        errors="$errors  $test_display\n"
        return
    fi

    # Compile C -> binary (include source dir for local .h files)
    local src_dir
    src_dir="$(dirname "$(echo $fc_files | awk '{print $1}')")"
    if ! "$CC" -std=c11 -Wall -Werror -Wno-unused-function -I "$src_dir" -o "$bin_file" "$c_file" -lm $EXTRA_LIBS 2>"$TMPDIR/${slug}.cc_stderr"; then
        echo "  FAIL  $test_display (C compilation failed)"
        cat "$TMPDIR/${slug}.cc_stderr"
        failed=$((failed + 1))
        errors="$errors  $test_display\n"
        return
    fi

    # Check expected exit code
    if [ -n "$expected_exit_file" ] && [ -f "$expected_exit_file" ]; then
        local expected_exit=$(cat "$expected_exit_file" | tr -d '[:space:]')
        set +e
        { "$bin_file" > "$TMPDIR/${slug}.stdout" 2>&1; } 2>/dev/null
        local actual_exit=$?
        set -e
        if [ "$actual_exit" != "$expected_exit" ]; then
            echo "  FAIL  $test_display (exit code: expected $expected_exit, got $actual_exit)"
            failed=$((failed + 1))
            errors="$errors  $test_display\n"
            return
        fi
    fi

    # Check expected stdout
    if [ -n "$expected_file" ] && [ -f "$expected_file" ]; then
        set +e
        { "$bin_file" > "$TMPDIR/${slug}.stdout" 2>&1; } 2>/dev/null
        set -e
        if ! diff -u "$expected_file" "$TMPDIR/${slug}.stdout" > "$TMPDIR/${slug}.diff" 2>&1; then
            echo "  FAIL  $test_display (output mismatch)"
            cat "$TMPDIR/${slug}.diff"
            failed=$((failed + 1))
            errors="$errors  $test_display\n"
            return
        fi
    fi

    # If no expected_exit and no expected stdout, run and expect exit code 0
    if { [ -z "$expected_exit_file" ] || [ ! -f "$expected_exit_file" ]; } && \
       { [ -z "$expected_file" ] || [ ! -f "$expected_file" ]; }; then
        set +e
        { "$bin_file" > "$TMPDIR/${slug}.stdout" 2>"$TMPDIR/${slug}.run_stderr"; } 2>/dev/null
        local actual_exit=$?
        set -e
        if [ "$actual_exit" != "0" ]; then
            echo "  FAIL  $test_display (exit code: expected 0, got $actual_exit)"
            cat "$TMPDIR/${slug}.run_stderr"
            failed=$((failed + 1))
            errors="$errors  $test_display\n"
            return
        fi
    fi

    echo "  PASS  $test_display"
    passed=$((passed + 1))
}

# Discover and run tests
# Two kinds:
#   1. Single-file: a .fc file directly in a category dir (expressions/foo.fc)
#   2. Multi-file:  a subdirectory containing *.fc files + expected_exit or error
#      (modules/cross_ns_import/{main.fc, lib.fc, expected_exit})

for milestone_dir in "$TESTDIR"/*/; do
    milestone=$(basename "$milestone_dir")

    # Single-file tests: .fc files directly in this directory
    for fc_file in "$milestone_dir"*.fc; do
        [ -f "$fc_file" ] || continue
        test_name=$(basename "$fc_file" .fc)
        test_display="$milestone/$test_name"

        run_test "$test_display" \
            "$fc_file" \
            "$milestone_dir${test_name}.error" \
            "$milestone_dir${test_name}.expected_exit" \
            "$milestone_dir${test_name}.expected"
    done

    # Multi-file tests: subdirectories containing .fc files
    for test_subdir in "$milestone_dir"*/; do
        [ -d "$test_subdir" ] || continue
        test_name=$(basename "$test_subdir")
        test_display="$milestone/$test_name"

        # Collect all .fc files in the subdirectory
        fc_files=$(find "$test_subdir" -name "*.fc" | sort | tr '\n' ' ')
        [ -n "$fc_files" ] || continue

        # Append dependency files listed in deps (one path per line, relative to project root)
        if [ -f "${test_subdir}deps" ]; then
            while IFS= read -r dep; do
                [ -n "$dep" ] || continue
                fc_files="$fc_files $dep"
            done < "${test_subdir}deps"
        fi

        # Collect --flag arguments from flags file
        fc_flags=""
        if [ -f "${test_subdir}flags" ]; then
            while IFS= read -r flag; do
                [ -n "$flag" ] || continue
                fc_flags="$fc_flags --flag $flag"
            done < "${test_subdir}flags"
        fi

        run_test "$test_display" \
            "$fc_files" \
            "${test_subdir}error" \
            "${test_subdir}expected_exit" \
            "${test_subdir}expected" \
            "$fc_flags"
    done
done

elapsed_ms=$(( ($(date +%s%N) - start_time) / 1000000 ))
elapsed_s=$(( elapsed_ms / 1000 ))
elapsed_frac=$(( elapsed_ms % 1000 ))

echo ""
printf "%d passed, %d failed in %d.%03ds\n" "$passed" "$failed" "$elapsed_s" "$elapsed_frac"

if [ $failed -gt 0 ]; then
    echo -e "Failed tests:\n$errors"
    exit 1
fi
