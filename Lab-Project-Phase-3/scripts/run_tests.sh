#!/usr/bin/env bash
# MiniOS test runner. Prints one PASS/FAIL line per test and a final summary.
#
#   bash scripts/run_tests.sh                     build, then test (same as make test)
#   bash scripts/run_tests.sh --mode MODE --binary MINIOS --unit TEST_CORE
#
# MODE is plain (default), sanitize, or valgrind. Every test runs in a temporary
# directory; the project's data/ and experiments/ folders are never written.
# Exit status: 0 all passed, 1 a test failed, 2 the suite could not run.

set -u -o pipefail
export LC_ALL=C

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

die() { echo "ERROR: $*" >&2; exit 2; }
usage() { sed -n '2,9s/^# \{0,1\}//p' "${BASH_SOURCE[0]}"; }

if [ "$#" -eq 0 ]; then
    exec make -C "$ROOT" test
fi

mode=plain binary='' unit=''
while [ "$#" -gt 0 ]; do
    case "$1" in
        --mode|--binary|--unit)
            [ "$#" -ge 2 ] || die "$1 needs a value"
            case "$1" in
                --mode) mode=$2 ;;
                --binary) binary=$2 ;;
                --unit) unit=$2 ;;
            esac
            shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1" ;;
    esac
done

case "$mode" in plain|sanitize|valgrind) ;; *) die "unknown mode: $mode" ;; esac
[ -f "$binary" ] && [ -x "$binary" ] || die "MiniOS binary not found: '$binary' (use make test)"
[ -f "$unit" ] && [ -x "$unit" ] || die "unit-test binary not found: '$unit' (use make test)"
command -v python3 >/dev/null || die "python3 is required for the reference-model tests"
if [ "$mode" = valgrind ]; then
    command -v valgrind >/dev/null || die "valgrind is not installed (sudo apt install valgrind)"
fi
if [ "$mode" = sanitize ]; then
    for program in "$binary" "$unit"; do
        grep -q __asan_init "$program" && grep -q __ubsan_handle "$program" ||
            die "$program is not built with ASan and UBSan (use make sanitize)"
    done
fi

binary=$(readlink -f "$binary")
unit=$(readlink -f "$unit")
WORK=$(mktemp -d "${TMPDIR:-/tmp}/minios-tests.XXXXXX") || die "cannot create a temporary directory"
trap 'rm -rf "$WORK"' EXIT
REPORTS=$WORK/reports
# A hung program must become a FAIL. --foreground keeps Ctrl-C working.
LIMIT='timeout --foreground -k 5 60'
mkdir "$REPORTS" "$WORK/bin"

# Wrappers let every test, including the Python ones, run under the selected tool.
make_wrapper() {
    {
        echo '#!/usr/bin/env bash'
        case "$mode" in
            sanitize)
                echo "export ASAN_OPTIONS='detect_leaks=1:detect_stack_use_after_return=1:strict_string_checks=1:exitcode=99:log_path=$REPORTS/asan'"
                echo "export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1:exitcode=99:log_path=$REPORTS/ubsan'"
                printf 'exec %s %q "$@"\n' "$LIMIT" "$2" ;;
            valgrind)
                printf 'exec %s valgrind ' "$LIMIT"
                printf -- '--quiet --error-exitcode=99 --track-origins=yes '
                printf -- '--leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all '
                printf -- '--log-file=%q/valgrind.%%p %q "$@"\n' "$REPORTS" "$2" ;;
            *)
                printf 'exec %s %q "$@"\n' "$LIMIT" "$2" ;;
        esac
    } > "$1"
    chmod +x "$1"
}
make_wrapper "$WORK/bin/minios" "$binary"
make_wrapper "$WORK/bin/test_core" "$unit"
MINIOS=$WORK/bin/minios
UNIT=$WORK/bin/test_core

snapshot() {
    (cd "$ROOT" && find data experiments -type f -print0 2>/dev/null | sort -z | xargs -0 -r sha256sum)
}
project_before=$(snapshot)

passed=0 failed=0 number=0 failed_names=()

pass() {
    passed=$((passed + 1))
    printf 'PASS  %s\n' "$1"
}

fail() {
    failed=$((failed + 1))
    failed_names+=("$1")
    printf 'FAIL  %s\n' "$1"
    if [ -n "${2:-}" ] && [ -s "$2" ]; then
        head -n 30 "$2" | sed 's/^/        /'
    fi
}

# Move non-empty tool reports into the test's log; succeed if there was one.
take_reports() {
    local file found=1
    for file in "$REPORTS"/*; do
        [ -e "$file" ] || continue
        if [ -s "$file" ]; then
            cat "$file" >> "$1"
            found=0
        fi
        rm -f "$file"
    done
    return "$found"
}

# Call as: run_test NAME FUNCTION [ARGS]. A test fails on its first failed command.
run_test() {
    local name=$1 log status
    shift
    number=$((number + 1))
    CASE=$WORK/case$number
    DATA=$CASE/data
    mkdir -p "$DATA/notes"
    log=$CASE/log
    (set -e; cd "$CASE"; "$@") > "$log" 2>&1
    status=$?
    if take_reports "$log" || [ "$status" -ne 0 ]; then
        fail "$name" "$log"
    else
        pass "$name"
    fi
}

# Relay a program that prints "PASS name" / "FAIL name" lines, ending with "DONE count".
run_group() {
    local group=$1 log=$WORK/$1.log line status seen=0 group_failed=0 done_count=''
    shift
    "$@" > "$log" 2>&1
    status=$?
    while IFS= read -r line; do
        case "$line" in
            'PASS '*) pass "$group: ${line#PASS }"; seen=$((seen + 1)) ;;
            'FAIL '*) fail "$group: ${line#FAIL }"; seen=$((seen + 1)); group_failed=1 ;;
            'DONE '*) done_count=${line#DONE } ;;
            *) printf '        %s\n' "$line" ;;
        esac
    done < "$log"
    if [ "$done_count" != "$seen" ] || { [ "$status" -ne 0 ] && [ "$group_failed" -eq 0 ]; }; then
        fail "$group: finished normally (exit $status, $seen results)" "$log"
    fi
    : > "$log.tool"
    if take_reports "$log.tool"; then
        fail "$group: no memory-tool errors" "$log.tool"
    fi
}

# shellcheck source=tests/helpers.sh
. "$ROOT/tests/helpers.sh"
# shellcheck source=tests/edge_cases.sh
. "$ROOT/tests/edge_cases.sh"

echo "MiniOS test suite | mode=$mode | binary=$binary"

echo
echo "== Functional cases (tests/cases) =="
cases=0
for input in "$ROOT"/tests/cases/*.in; do
    [ -f "$input" ] || continue
    cases=$((cases + 1))
    run_test "case: $(basename "$input" .in)" golden_case "$input"
done
[ "$cases" -gt 0 ] || fail "functional cases were found"

echo
echo "== Edge and fault cases (tests/edge_cases.sh) =="
edge=0
for function in $(compgen -A function | grep '^test_'); do
    edge=$((edge + 1))
    label=${function#test_}
    run_test "edge: ${label//_/ }" "$function"
done
[ "$edge" -gt 0 ] || fail "edge cases were found"

echo
echo "== Unit tests (tests/test_core.c) =="
mkdir "$WORK/unit-data"
run_group unit "$UNIT" "$WORK/unit-data"

echo
echo "== Reference model and experiments (tests/test_model.py) =="
scenarios=500
[ "$mode" != valgrind ] || scenarios=40 # each process start costs about half a second
run_group model python3 -B "$ROOT/tests/test_model.py" "$MINIOS" --scenarios "$scenarios"

echo
echo "== Project safety =="
if [ "$project_before" = "$(snapshot)" ]; then
    pass "safety: data/ and experiments/ are unchanged"
else
    fail "safety: data/ and experiments/ are unchanged"
fi

echo
echo "----------------------------------------"
echo "SUMMARY mode=$mode total=$((passed + failed)) passed=$passed failed=$failed"
if [ "$failed" -ne 0 ]; then
    printf '  failed: %s\n' "${failed_names[@]}"
    echo "RESULT: FAIL"
    exit 1
fi
echo "RESULT: PASS"
