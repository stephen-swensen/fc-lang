#!/bin/bash
set -e
ulimit -c 0

# Per-OS build subdirectory: build/linux/, build/windows/, ... — ask Make
# rather than replicate the OS-detection logic here.
FCC="$(make -s print-bin)"
CC="${CC:-cc}"
TESTDIR="tests/cases"
TMPDIR=$(mktemp -d)
trap "rm -rf $TMPDIR" EXIT

# On Windows (MSYS2/MinGW), net.fc uses Winsock and needs -lws2_32. Adding it
# unconditionally is harmless on tests that don't pull in winsock symbols.
EXTRA_LIBS=""
IS_WINDOWS=""
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*) EXTRA_LIBS="-lws2_32"; IS_WINDOWS=1 ;;
esac

# Optimized-C run detection. The -O2 test targets (test-*-O2) set CC_OPT=-O2;
# the default runs leave it empty (the transpiled C is built unoptimized). This
# drives the skip_o2 / only_o2 markers below.
IS_O2=""
case "${CC_OPT:-}" in *-O2*) IS_O2=1 ;; esac

# UCRT's abort() exits with status 3 (it calls _exit(3) after raising SIGABRT),
# whereas POSIX reports 128+SIGABRT=134. Existing .expected_exit files hardcode
# the POSIX value; treat 3 as equivalent on Windows rather than duplicating
# every exit file per platform.
exit_matches() {
    local expected="$1"
    local actual="$2"
    if [ "$expected" = "$actual" ]; then return 0; fi
    if [ -n "$IS_WINDOWS" ] && [ "$expected" = "134" ] && [ "$actual" = "3" ]; then return 0; fi
    return 1
}
export -f exit_matches
export IS_WINDOWS

start_time=$(date +%s%N)
JOBS="${JOBS:-$(nproc)}"

# Platform skips accumulate here (see the skip_windows marker check below).
skipped=0
skip_names=""

# run_one_test outputs a single line: PASS or FAIL with details.
# Each test writes to its own temp files keyed by slug, so no conflicts.
run_one_test() {
    local test_display="$1"
    local fc_files="$2"
    local error_file="$3"
    local expected_exit_file="$4"
    local expected_file="$5"
    local fc_flags="$6"
    local stderr_contains_file="$7"

    local slug="${test_display//\//_}"
    local c_file="$TMPDIR/${slug}.c"
    local bin_file="$TMPDIR/${slug}"

    # Cap each compile's virtual memory so a runaway fcc/cc (e.g. an exponential
    # monomorphization) dies as a single failed test instead of exhausting RAM
    # and letting the OOM killer take down the whole session/VM. Normal compiles
    # use a few MB, so the default 3 GiB is generous. Override via
    # FC_TEST_MEM_CAP_KB (0 disables). Not applied on Windows (ulimit -v is a
    # no-op under MSYS2/UCRT).
    if [ -z "$IS_WINDOWS" ]; then
        ulimit -v "${FC_TEST_MEM_CAP_KB:-3145728}" 2>/dev/null || true
    fi

    # Compile FC -> C
    if ! $FCC $fc_files $fc_flags -o "$c_file" 2>"$TMPDIR/${slug}.stderr"; then
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

    # Compile C -> binary (include source dir for local .h files)
    local src_dir
    src_dir="$(dirname "$(echo $fc_files | awk '{print $1}')")"
    if ! "$CC" -std=c11 -Wall -Werror ${CC_OPT:-} -I "$src_dir" -o "$bin_file" "$c_file" -lm $EXTRA_LIBS 2>"$TMPDIR/${slug}.cc_stderr"; then
        echo "FAIL  $test_display (C compilation failed)"
        cat "$TMPDIR/${slug}.cc_stderr" >&2
        return
    fi

    # Run the binary once, capturing stdout and stderr separately so we can
    # match .expected (combined stdout+stderr, exact diff) and
    # .expected_stderr_contains (stderr only, substring lines). Mirrors the
    # run_bin helper in tests/run_tests.sh.
    local stdout_file="$TMPDIR/${slug}.stdout"
    local stderr_file="$TMPDIR/${slug}.run_stderr"
    local combined_file="$TMPDIR/${slug}.combined"
    local actual_exit
    local ran=0
    run_bin() {
        set +e
        "$bin_file" > "$stdout_file" 2>"$stderr_file"
        actual_exit=$?
        set -e
        cat "$stdout_file" "$stderr_file" > "$combined_file" 2>/dev/null || true
        ran=1
    }

    # Check expected exit code
    if [ -n "$expected_exit_file" ] && [ -f "$expected_exit_file" ]; then
        local expected_exit=$(cat "$expected_exit_file" | tr -d '[:space:]')
        run_bin
        if ! exit_matches "$expected_exit" "$actual_exit"; then
            echo "FAIL  $test_display (exit code: expected $expected_exit, got $actual_exit)"
            return
        fi
    fi

    # Check expected stdout (matches combined stdout+stderr for back-compat)
    if [ -n "$expected_file" ] && [ -f "$expected_file" ]; then
        [ "$ran" = "1" ] || run_bin
        if ! diff -u "$expected_file" "$combined_file" > "$TMPDIR/${slug}.diff" 2>&1; then
            echo "FAIL  $test_display (output mismatch)"
            cat "$TMPDIR/${slug}.diff" >&2
            return
        fi
    fi

    # If no expected_exit and no expected stdout, run and expect exit code 0
    if { [ -z "$expected_exit_file" ] || [ ! -f "$expected_exit_file" ]; } && \
       { [ -z "$expected_file" ] || [ ! -f "$expected_file" ]; }; then
        [ "$ran" = "1" ] || run_bin
        if [ "$actual_exit" != "0" ]; then
            echo "FAIL  $test_display (exit code: expected 0, got $actual_exit)"
            cat "$stderr_file" >&2
            return
        fi
    fi

    # Substring assertions against stderr.  Each non-empty, non-comment line of
    # expected_stderr_contains must appear (fixed-string) somewhere in stderr.
    # Used by --backtraces tests where the exact frame layout is variable but
    # key tokens are stable.
    if [ -n "$stderr_contains_file" ] && [ -f "$stderr_contains_file" ]; then
        [ "$ran" = "1" ] || run_bin
        local missing=""
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            case "$line" in \#*) continue ;; esac
            if ! grep -qF -- "$line" "$stderr_file"; then
                missing="${missing}    missing: ${line}\n"
            fi
        done < "$stderr_contains_file"
        if [ -n "$missing" ]; then
            echo "FAIL  $test_display (stderr missing expected substrings)"
            printf "$missing" >&2
            echo "  ---- actual stderr ----" >&2
            cat "$stderr_file" >&2
            return
        fi
    fi

    echo "PASS  $test_display"
}

export -f run_one_test
export FCC CC TMPDIR FILTER

# Build the list of tests (one per line: display|fc_files|error|exit|expected|flags)
test_list="$TMPDIR/test_list"

for milestone_dir in "$TESTDIR"/*/; do
    milestone=$(basename "$milestone_dir")

    for fc_file in "$milestone_dir"*.fc; do
        [ -f "$fc_file" ] || continue
        test_name=$(basename "$fc_file" .fc)
        echo "$milestone/$test_name|$fc_file|${milestone_dir}${test_name}.error|${milestone_dir}${test_name}.expected_exit|${milestone_dir}${test_name}.expected||"
    done

    for test_subdir in "$milestone_dir"*/; do
        [ -d "$test_subdir" ] || continue
        test_name=$(basename "$test_subdir")
        fc_files=$(find "$test_subdir" -name "*.fc" | sort | tr '\n' ' ')
        [ -n "$fc_files" ] || continue

        # Platform skips: a skip_windows marker opts a test out on Windows —
        # e.g. the --backtraces tests, whose frames rely on execinfo backtrace()
        # (glibc/macOS only; a no-op stub under MSYS2/UCRT).
        if [ -n "$IS_WINDOWS" ] && [ -f "${test_subdir}skip_windows" ]; then
            if [ -z "$FILTER" ] || printf '%s' "$milestone/$test_name" | grep -q "$FILTER"; then
                skipped=$((skipped + 1))
                skip_names="${skip_names}  SKIP  $milestone/$test_name (windows)\n"
            fi
            continue
        fi

        # Optimization-level skips (see IS_O2 above). skip_o2 opts a test out of
        # the -O2 runs (e.g. --backtraces tests asserting full frames that TCO
        # legitimately elides at -O2); only_o2 opts a test out of the default
        # unoptimized runs (e.g. the -O2 backtrace variants asserting the
        # degraded frame set / cold-split handling, which only holds at -O2).
        if { [ -n "$IS_O2" ] && [ -f "${test_subdir}skip_o2" ]; } || \
           { [ -z "$IS_O2" ] && [ -f "${test_subdir}only_o2" ]; }; then
            if [ -z "$FILTER" ] || printf '%s' "$milestone/$test_name" | grep -q "$FILTER"; then
                skipped=$((skipped + 1))
                skip_names="${skip_names}  SKIP  $milestone/$test_name (opt)\n"
            fi
            continue
        fi

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

        # Literal extra fcc args (one per line; `#` comments skipped) — e.g.
        # --backtraces or a @response.rsp file. Matches tests/run_tests.sh.
        if [ -f "${test_subdir}fcc_args" ]; then
            while IFS= read -r arg; do
                [ -n "$arg" ] || continue
                case "$arg" in \#*) continue ;; esac
                fc_flags="$fc_flags $arg"
            done < "${test_subdir}fcc_args"
        fi

        echo "$milestone/$test_name|$fc_files|${test_subdir}error|${test_subdir}expected_exit|${test_subdir}expected|$fc_flags|${test_subdir}expected_stderr_contains"
    done
done > "$test_list"

# Apply filter if set
if [ -n "$FILTER" ]; then
    filtered="$TMPDIR/test_list_filtered"
    grep "$FILTER" "$test_list" > "$filtered" || true
    test_list="$filtered"
fi

# Run tests in parallel, collect output
results=$(cat "$test_list" | xargs -P "$JOBS" -I {} bash -c '
    IFS="|" read -r display files err exit exp flags scont <<< "{}"
    run_one_test "$display" "$files" "$err" "$exit" "$exp" "$flags" "$scont"
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

if [ -n "$skip_names" ]; then
    echo -en "$skip_names"
fi

elapsed_ms=$(( ($(date +%s%N) - start_time) / 1000000 ))
elapsed_s=$(( elapsed_ms / 1000 ))
elapsed_frac=$(( elapsed_ms % 1000 ))

echo ""
if [ "$skipped" -gt 0 ]; then
    printf "%d passed, %d failed, %d skipped in %d.%03ds (%s)\n" "$passed" "$failed" "$skipped" "$elapsed_s" "$elapsed_frac" "${CC:-cc}"
else
    printf "%d passed, %d failed in %d.%03ds (%s)\n" "$passed" "$failed" "$elapsed_s" "$elapsed_frac" "${CC:-cc}"
fi

if [ $failed -gt 0 ]; then
    echo -e "Failed tests:\n$fail_lines"
    exit 1
fi
