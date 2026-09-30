#!/bin/bash
# Run the compiler test suite: compile each test to C with fcc, compile the C
# with $CC -std=c11 -Wall -Werror, run it, and check the result against the
# test's markers (see CONTRIBUTING.md for the test layout).
#
# Environment: CC (C compiler, default cc), CC_OPT (its optimization flags,
# e.g. -O2), FCC_EXTRA_ARGS (extra fcc options for every test), FILTER (awk
# pattern over category/test_name), JOBS (parallel jobs, default nproc),
# KEEP=1 (keep the work directory with every test's C file and output), FCC
# (the compiler to test, default the one `make` builds), FC_TEST_MEM_CAP_KB
# (the memory cap on fcc in KB, default 3 GB; 0 lifts it). Run it from the
# repository root.
#
# `run_tests.sh --list` prints the selected tests instead of running them, one
# per line: name|fcc arguments|error file|expected_exit file|stderr_contains
# file. tools/emit-corpus.sh uses it.
set -e
ulimit -c 0

LIST_ONLY=""
[ "${1:-}" = "--list" ] && LIST_ONLY=1

FCC="${FCC:-$(make -s print-bin)}"
CC="${CC:-cc}"
TESTDIR="tests/cases"
TMPDIR=$(mktemp -d)
if [ -n "${KEEP:-}" ] && [ -z "$LIST_ONLY" ]; then
    echo "Keeping work directory: $TMPDIR"
else
    trap "rm -rf $TMPDIR" EXIT
fi

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

start_time=$(date +%s%N)
JOBS="${JOBS:-$(nproc)}"

# run_one_test writes the test's result to $TMPDIR/results/<slug>: a first line
# starting PASS or FAIL, then any detail lines for a failure.
run_one_test() {
    local test_display="$1"
    local fc_args="$2"
    local error_file="$3"
    local expected_exit_file="$4"
    local stderr_contains_file="$5"

    local slug="${test_display//\//_}"
    local c_file="$TMPDIR/${slug}.c"
    local bin_file="$TMPDIR/${slug}"
    local out="$TMPDIR/results/$slug"

    if [ -z "$fc_args" ]; then
        echo "FAIL  $test_display (the test directory has no .fc file)" > "$out"
        return
    fi

    # Compile C -> binary with the test's own directory on the include path, so
    # a test can carry local .h files.
    local src_dir
    src_dir="$(dirname "$(echo $fc_args | awk '{print $1}')")"
    local fcc_cmd="$FCC $fc_args${FCC_EXTRA_ARGS:+ $FCC_EXTRA_ARGS} -o $c_file"
    local cc_cmd="$CC -std=c11 -Wall -Werror${CC_OPT:+ $CC_OPT} -I $src_dir -o $bin_file $c_file -lm${EXTRA_LIBS:+ $EXTRA_LIBS}"

    # How to repeat the failing steps by hand, from the repository root. The
    # work directory is gone after the run, so the commands write to /tmp.
    rerun() {
        local t="/tmp/fc_$slug"
        echo "    rerun: ${fcc_cmd//$TMPDIR\/${slug}/$t}"
        [ "$1" = fcc ] || echo "           ${cc_cmd//$TMPDIR\/${slug}/$t} && $t"
    }

    # Cap each compile's virtual memory so a runaway fcc/cc (e.g. an exponential
    # monomorphization) dies as a single failed test instead of exhausting RAM
    # and letting the OOM killer take down the whole session/VM. Normal compiles
    # use a few MB, so the default 3 GiB is generous. Override via
    # FC_TEST_MEM_CAP_KB (0 disables; needed for an fcc built with
    # -fsanitize=address, whose shadow memory reservation exceeds the cap). Not
    # applied on Windows (ulimit -v is a no-op under MSYS2/UCRT).
    if [ -z "$IS_WINDOWS" ] && [ "${FC_TEST_MEM_CAP_KB:-3145728}" != 0 ]; then
        ulimit -v "${FC_TEST_MEM_CAP_KB:-3145728}" 2>/dev/null || true
    fi

    # Compile FC -> C.
    if ! $fcc_cmd 2>"$TMPDIR/${slug}.stderr"; then
        if [ -f "$error_file" ]; then
            local expected_error
            expected_error=$(cat "$error_file")
            if grep -qF -- "$expected_error" "$TMPDIR/${slug}.stderr"; then
                echo "PASS  $test_display (expected error)" > "$out"
            else
                {
                    echo "FAIL  $test_display (wrong error)"
                    echo "    expected: $expected_error"
                    sed 's/^/    got: /' "$TMPDIR/${slug}.stderr"
                    rerun fcc
                } > "$out"
            fi
            return
        fi
        {
            echo "FAIL  $test_display (fc compilation failed)"
            sed 's/^/    /' "$TMPDIR/${slug}.stderr"
            rerun fcc
        } > "$out"
        return
    fi

    if [ -f "$error_file" ]; then
        {
            echo "FAIL  $test_display (expected error but compilation succeeded)"
            echo "    expected: $(cat "$error_file")"
            rerun fcc
        } > "$out"
        return
    fi

    if ! $cc_cmd 2>"$TMPDIR/${slug}.cc_stderr"; then
        {
            echo "FAIL  $test_display (C compilation failed)"
            sed 's/^/    /' "$TMPDIR/${slug}.cc_stderr"
            rerun cc
        } > "$out"
        return
    fi

    # Run the binary and check its exit code (0 unless expected_exit says
    # otherwise). The outer 2>/dev/null drops bash's "Aborted (core dumped)"
    # notice for the many tests that exit through abort().
    local stderr_file="$TMPDIR/${slug}.run_stderr"
    local actual_exit expected_exit=0
    set +e
    { "$bin_file" > /dev/null 2>"$stderr_file"; actual_exit=$?; } 2>/dev/null
    set -e
    [ -f "$expected_exit_file" ] && expected_exit=$(tr -d '[:space:]' < "$expected_exit_file")
    if ! exit_matches "$expected_exit" "$actual_exit"; then
        {
            echo "FAIL  $test_display (exit code: expected $expected_exit, got $actual_exit)"
            sed 's/^/    /' "$stderr_file"
            rerun cc
        } > "$out"
        return
    fi

    # Each non-empty, non-comment line of expected_stderr_contains must appear
    # (as a fixed string) somewhere in stderr. The --backtraces tests use this,
    # since their exact frame layout varies but key tokens are stable.
    if [ -f "$stderr_contains_file" ]; then
        local missing=""
        while IFS= read -r line; do
            [ -n "$line" ] || continue
            case "$line" in \#*) continue ;; esac
            if ! grep -qF -- "$line" "$stderr_file"; then
                missing="${missing}    missing: ${line}\n"
            fi
        done < "$stderr_contains_file"
        if [ -n "$missing" ]; then
            {
                echo "FAIL  $test_display (stderr missing expected substrings)"
                printf "$missing"
                echo "    ---- actual stderr ----"
                sed 's/^/    /' "$stderr_file"
                rerun cc
            } > "$out"
            return
        fi
    fi

    echo "PASS  $test_display" > "$out"
}

export -f run_one_test exit_matches
export FCC CC CC_OPT FCC_EXTRA_ARGS TMPDIR EXTRA_LIBS IS_WINDOWS

# List every test, one per line: name|fcc args|error|exit|stderr_contains|skip.
# The last field names why this run skips the test, or is empty.
list_tests() {
    local category_dir category fc_file test_name test_subdir fc_args skip line words
    for category_dir in "$TESTDIR"/*/; do
        category=$(basename "$category_dir")

        for fc_file in "$category_dir"*.fc; do
            [ -f "$fc_file" ] || continue
            test_name=$(basename "$fc_file" .fc)
            echo "$category/$test_name|$fc_file|${category_dir}${test_name}.error|${category_dir}${test_name}.expected_exit||"
        done

        for test_subdir in "$category_dir"*/; do
            [ -d "$test_subdir" ] || continue
            test_name=$(basename "$test_subdir")
            fc_args=$(find "$test_subdir" -name "*.fc" | sort | tr '\n' ' ')
            # A directory with no .fc file is reported (run_one_test fails
            # it), not silently dropped.
            if [ -z "$fc_args" ]; then
                echo "$category/$test_name||||"
                continue
            fi

            # skip_windows: the --backtraces tests rely on execinfo backtrace(),
            # which is glibc/macOS only. skip_o2 opts a test out of the -O2 runs
            # (e.g. backtrace tests asserting frames that TCO elides at -O2);
            # only_o2 opts it out of the default unoptimized runs.
            skip=""
            if [ -n "$IS_WINDOWS" ] && [ -f "${test_subdir}skip_windows" ]; then
                skip="windows"
            elif { [ -n "$IS_O2" ] && [ -f "${test_subdir}skip_o2" ]; } || \
                 { [ -z "$IS_O2" ] && [ -f "${test_subdir}only_o2" ]; }; then
                skip="opt"
            fi

            if [ -f "${test_subdir}deps" ]; then
                while IFS= read -r line; do
                    [ -n "$line" ] && fc_args="$fc_args $line"
                done < "${test_subdir}deps"
            fi
            if [ -f "${test_subdir}flags" ]; then
                while IFS= read -r line; do
                    [ -n "$line" ] && fc_args="$fc_args --flag $line"
                done < "${test_subdir}flags"
            fi
            # Literal extra fcc args, one per line (`#` comments skipped), such
            # as --backtraces or a @response.rsp file.
            if [ -f "${test_subdir}fcc_args" ]; then
                while IFS= read -r line; do
                    [ -n "$line" ] || continue
                    case "$line" in \#*) continue ;; esac
                    fc_args="$fc_args $line"
                done < "${test_subdir}fcc_args"
            fi

            read -ra words <<< "$fc_args"
            fc_args="${words[*]}"
            echo "$category/$test_name|$fc_args|${test_subdir}error|${test_subdir}expected_exit|${test_subdir}expected_stderr_contains|$skip"
        done
    done
}

test_list="$TMPDIR/test_list"
list_tests | awk -F'|' -v pat="${FILTER:-}" 'pat == "" || $1 ~ pat' > "$test_list"

if [ -n "$LIST_ONLY" ]; then
    awk -F'|' '$6 == "" && $2 != "" { print $1 "|" $2 "|" $3 "|" $4 "|" $5 }' "$test_list"
    exit 0
fi

mkdir -p "$TMPDIR/results"
awk -F'|' '$6 == ""' "$test_list" | xargs -P "$JOBS" -I {} bash -c '
    IFS="|" read -r name args err exit scont skip <<< "{}"
    run_one_test "$name" "$args" "$err" "$exit" "$scont"
'

# Report in list order, each failure with its details under it, then repeat
# the failures at the end. The counts go to $TMPDIR/counts.
awk -F'|' -v dir="$TMPDIR/results" -v counts="$TMPDIR/counts" '
    $6 != "" { print "  SKIP  " $1 " (" $6 ")"; skipped++; next }
    {
        slug = $1; gsub("/", "_", slug)
        file = dir "/" slug
        n = 0
        while ((getline line < file) > 0) text[n++] = line
        close(file)
        if (n == 0) text[n++] = "FAIL  " $1 " (no result: the test runner itself failed)"
        text[0] = "  " text[0]
        for (i = 0; i < n; i++) print text[i]
        if (text[0] ~ /^  PASS/) passed++
        else {
            failed++
            for (i = 0; i < n; i++) report = report text[i] "\n"
        }
    }
    END {
        if (failed) printf "\nFailed tests:\n%s", report > "/dev/stderr"
        print passed + 0, failed + 0, skipped + 0 > counts
    }' "$test_list" 2> "$TMPDIR/fail_report"

read -r passed failed skipped < "$TMPDIR/counts"
elapsed_ms=$(( ($(date +%s%N) - start_time) / 1000000 ))

echo ""
summary="$passed passed, $failed failed"
[ "$skipped" -gt 0 ] && summary="$summary, $skipped skipped"
printf "%s in %d.%03ds (%s)\n" "$summary" $((elapsed_ms / 1000)) $((elapsed_ms % 1000)) "$CC"

if [ "$failed" -gt 0 ]; then
    cat "$TMPDIR/fail_report"
    exit 1
fi
