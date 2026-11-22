# Assertion helpers, sourced by scripts/run_tests.sh.
#
# Each test runs under set -e in its own directory ($CASE) with an empty data
# directory ($DATA). A failed helper explains itself and ends the test.
# Note: "! command" does not trigger set -e, so use lacks for negative checks.

# launch EXIT_STATUS [MINIOS ARGS]: stdin is the script; fills out and err.
launch() {
    local expected=$1 status=0
    shift
    "$MINIOS" "$@" > "$CASE/out" 2> "$CASE/err" || status=$?
    if [ "$status" != "$expected" ]; then
        echo "exit status $status, expected $expected"
        cat "$CASE/err"
        return 1
    fi
}

# batch EXIT_STATUS: run the script on stdin against $DATA.
batch() {
    launch "$1" --batch --data-dir "$DATA"
}

# script LINE...: a successful batch run that must not write to stderr.
script() {
    printf '%s\n' "$@" | batch 0
    empty "$CASE/err"
}

# has TEXT [FILE]: some line contains TEXT.
has() {
    local file=${2:-$CASE/out}
    if ! grep -aqF -- "$1" "$file"; then
        echo "missing '$1' in $(basename "$file")"
        return 1
    fi
}

# line TEXT [FILE]: some line is exactly TEXT.
line() {
    local file=${2:-$CASE/out}
    if ! grep -aqxF -- "$1" "$file"; then
        echo "missing line '$1' in $(basename "$file")"
        return 1
    fi
}

# lacks TEXT [FILE]: no line contains TEXT.
lacks() {
    local file=${2:-$CASE/out}
    if [ ! -f "$file" ]; then
        echo "no such file: $file"
        return 1
    fi
    if grep -aqF -- "$1" "$file"; then
        echo "unexpected '$1' in $(basename "$file")"
        return 1
    fi
}

# times COUNT REGEX [FILE]: exactly COUNT lines match.
times() {
    local file=${3:-$CASE/out} found
    found=$(grep -acE -- "$2" "$file" || true)
    if [ "$found" != "$1" ]; then
        echo "found $found lines matching '$2', expected $1"
        return 1
    fi
}

empty() {
    if [ -s "$1" ]; then
        echo "expected $(basename "$1") to be empty:"
        cat "$1"
        return 1
    fi
}

same() {
    if [ "$1" != "$2" ]; then
        printf 'expected: %s\nactual:   %s\n' "$1" "$2"
        return 1
    fi
}

repeat() { # repeat COUNT CHARACTER
    head -c "$1" /dev/zero | tr '\0' "$2"
}

creates() { # creates COUNT BURST BYTES
    local i
    for ((i = 1; i <= $1; i++)); do
        echo "task create T$i $2 $3"
    done
}

# A case is NAME.in plus a hand-checked NAME.expected holding only the responses:
# the 3-line banner and the echoed "minios> " commands are removed before diff.
# CR is stripped from both files so that a Windows checkout also works.
golden_case() {
    local input=$1
    tr -d '\r' < "$input" | batch 0
    empty "$CASE/err"
    sed -e '1,3d' -e '/^minios> /d' "$CASE/out" > "$CASE/actual"
    tr -d '\r' < "${input%.in}.expected" > "$CASE/expected"
    diff -u "$CASE/expected" "$CASE/actual"
}
