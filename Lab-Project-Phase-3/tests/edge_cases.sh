# Edge, persistence, and fault cases, sourced by scripts/run_tests.sh.
# Every function named test_* is one test.

SYNTAX_ERROR='ERROR: Invalid command or argument; enter help for syntax and limits.'
TOO_LONG='ERROR: command too long (maximum 2046 bytes); entire line discarded.'
NUL_BYTE='ERROR: command contains a NUL byte; entire line discarded.'

# ---- Task capacity ----

test_capacity_accepts_20_tasks() {
    { creates 20 1 1; echo tasks; echo check; } | batch 0
    times 20 '^Created P'
    has 'records=20/20'
    has 'CHECK PASS'
}

test_capacity_rejects_task_21_without_side_effects() {
    { creates 20 1 10; echo 'task create T21 1 10'; echo tasks; echo memory; echo check; } | batch 0
    line 'ERROR: Task table full (20 records). Use reset for a new experiment.'
    times 20 '^Created P'
    lacks 'Created P21'
    line 'Memory: used=200/65536 bytes free=65336 allocations=20'
    line "Ready queue:$(printf ' P%d' {1..20})"
    has 'CHECK PASS'
}

test_capacity_also_limits_sessions() {
    { creates 20 1 1; echo 'session start S 1 1'; } | batch 0
    has 'ERROR: Task table full'
}

test_capacity_counts_finished_tasks_until_reset() {
    { creates 20 1 1; echo run; echo 'task create X 1 1'; echo reset; echo 'task create Y 1 1'; } | batch 0
    has 'ERROR: Task table full'
    line 'Created P1 Y: READY, burst=1, payload=1 bytes'
}

test_budget_split_across_20_tasks_exactly() {
    { creates 19 1 3276; echo 'task create Last 1 3292'; echo memory; echo check; } | batch 0
    line 'Memory: used=65536/65536 bytes free=0 allocations=20'
    has 'CHECK PASS'
}

test_queue_wraparound_with_block_and_wake() {
    { creates 20 3 1; echo 'quantum 1'; for _ in {1..18}; do echo step; done
      echo 'block 2'; echo 'wake 2'; echo tasks; echo check; } | batch 0
    line 'Ready queue: P19 P20 P1 P3 P4 P5 P6 P7 P8 P9 P10 P11 P12 P13 P14 P15 P16 P17 P18 P2'
    has 'CHECK PASS'
}

# ---- Notes persistence ----

test_note_survives_restart() {
    script 'note create keep first line' quit
    script 'note read keep' quit
    line 'first line'
}

test_note_append_survives_restart() {
    script 'note create keep one' quit
    script 'note append keep two' quit
    script 'note read keep' quit
    same $'one\ntwo' "$(cat "$DATA/notes/keep.txt")"
    line one
    line two
}

test_note_delete_survives_restart() {
    script 'note create gone text' quit
    script 'note delete gone' quit
    script 'note read gone' quit
    has 'ERROR: Cannot read note'
}

test_note_survives_reset_and_eof() {
    script 'note create keep kept' reset
    script 'note read keep'
    line kept
}

test_note_create_never_overwrites_across_restart() {
    script 'note create n original' quit
    script 'note create n replacement' quit
    has 'ERROR: Cannot create note'
    same original "$(cat "$DATA/notes/n.txt")"
}

test_task_state_does_not_persist() {
    script 'task create A 3 10' step quit
    script tasks memory quit
    line 'Tasks | tick=0 quantum=2 records=0/20'
    line 'Memory: used=0/65536 bytes free=65536 allocations=0'
}

test_note_path_traversal_creates_no_file() {
    script 'note create ../escape x' 'note create /abs x' 'note create a/b x'
    times 3 '^ERROR: Note name'
    same '' "$(find "$CASE" -name '*escape*' -o -name 'abs*' -o -name 'b.txt')"
}

test_note_text_of_several_read_chunks() {
    local text
    text=$(repeat 1800 q)
    script "note create long $text" 'note read long'
    line "$text"
}

test_note_directory_cannot_be_read_as_a_note() {
    mkdir "$DATA/notes/dir.txt"
    script 'note read dir'
    has 'ERROR: Note read/output/close failed.'
}

test_notes_fail_cleanly_without_notes_directory() {
    rmdir "$DATA/notes"
    script 'note create x text' check
    has 'ERROR: Cannot create note'
    has 'CHECK PASS'
}

# ---- Input boundaries ----

test_name_48_accepted_49_rejected() {
    script "task create $(repeat 48 n) 1 1" "task create $(repeat 49 m) 1 1"
    times 1 '^Created P'
    has 'ERROR: Name: 1..48'
}

test_note_name_48_accepted_49_rejected() {
    script "note create $(repeat 48 n) x" "note create $(repeat 49 m) x"
    times 1 '^OK: note create'
    has 'ERROR: Note name'
}

test_maximum_burst_and_quantum() {
    script 'quantum 1000000' 'task create Max 1000000 65536' run check
    line '[0,1000000) P1 Max          RUNNING -> FINISHED remaining=0'
    has 'CHECK PASS'
}

test_numeric_overflow_rejected() {
    script 'task create X 99999999999999999999999 1' 'task create X 1 99999999999999999999999' \
           'quantum 99999999999999999999999' 'task create X 4294967297 1' tasks
    times 4 '^ERROR: Invalid command'
    has 'records=0/20'
}

test_non_decimal_numbers_rejected() {
    script 'quantum -1' 'quantum +1' 'quantum 0x10' 'quantum 1.5' 'quantum 1e3' 'quantum two' tasks
    times 6 '^ERROR: Invalid command'
    has 'quantum=2'
}

test_line_of_2046_bytes_accepted() {
    local prefix='note create x '
    script "$prefix$(repeat $((2046 - ${#prefix})) a)"
    line 'OK: note create x'
}

test_line_of_2047_bytes_discarded() {
    local prefix='note create x '
    script "$prefix$(repeat $((2047 - ${#prefix})) a)" 'note read x'
    line "$TOO_LONG"
    has 'ERROR: Cannot read note'
}

test_crlf_line_of_2046_bytes_accepted() {
    local prefix='note create x '
    printf '%s\r\n' "$prefix$(repeat $((2046 - ${#prefix})) a)" | batch 0
    empty "$CASE/err"
    line 'OK: note create x'
}

test_crlf_line_of_2047_bytes_discarded() {
    local prefix='note create x '
    printf '%s\r\nnote read x\r\n' "$prefix$(repeat $((2047 - ${#prefix})) a)" | batch 0
    line "$TOO_LONG"
    has 'ERROR: Cannot read note'
}

test_only_one_final_cr_is_free() {
    local prefix='note create x '
    printf '%s\r\r\n' "$prefix$(repeat $((2046 - ${#prefix})) a)" | batch 0
    line "$TOO_LONG"
    lacks 'OK: note create'
}

test_oversized_line_tail_never_executes() {
    script "$(repeat 2047 a)task create Tail 1 1" tasks
    line "$TOO_LONG"
    lacks 'Created P'
    has 'records=0/20'
}

# NUL bytes go straight from printf into the pipe; no shell variable holds one.

test_nul_at_line_start_rejected() {
    { printf '\0task create Hidden 1 1\n'; echo tasks; } | batch 0
    empty "$CASE/err"
    line "$NUL_BYTE"
    lacks 'Created P'
    has 'records=0/20'
}

test_nul_in_middle_of_command_rejected() {
    { printf 'note create n hello\0world\n'; echo 'note read n'; } | batch 0
    empty "$CASE/err"
    line "$NUL_BYTE"
    lacks 'OK: note create'
    same '' "$(ls -A "$DATA/notes")"
}

test_nul_at_line_end_rejected() {
    { printf 'task create End 1 1\0\n'; echo tasks; } | batch 0
    empty "$CASE/err"
    line "$NUL_BYTE"
    lacks 'Created P'
    has 'records=0/20'
}

test_nul_inside_oversized_line_discarded() {
    { repeat 1000 a; printf '\0'; repeat 2000 b; printf '\n'; echo tasks; } | batch 0
    empty "$CASE/err"
    times 1 '^ERROR:'
    line "$TOO_LONG"
    has 'records=0/20'
}

test_oversized_line_with_early_nul_never_runs_tail() {
    # 2,047 bytes fill the old fgets buffer exactly, so the old reader ran the tail.
    { printf '#\0'; repeat 2045 x; printf 'task create Tail 1 1\n'; echo tasks; } | batch 0
    empty "$CASE/err"
    line "$TOO_LONG"
    lacks 'Created P'
    has 'records=0/20'
}

test_input_recovers_after_nul_line() {
    { printf 'tasks\0junk\n'; echo 'task create After 1 1'; echo tasks; } | batch 0
    empty "$CASE/err"
    times 1 '^ERROR:'
    line "$NUL_BYTE"
    times 1 '^minios> tasks$'
    line 'Created P1 After: READY, burst=1, payload=1 bytes'
    has 'records=1/20'
}

test_crlf_input() {
    printf 'task create A 1 1\r\nrun\r\nnote create c hello\r\nquit\r\n' | batch 0
    line 'Created P1 A: READY, burst=1, payload=1 bytes'
    same hello "$(cat "$DATA/notes/c.txt")"
    lacks 'ERROR:'
}

test_tabs_and_repeated_spaces() {
    printf '\ttask  create\tA   3 4  \ncheck\n' | batch 0
    line 'Created P1 A: READY, burst=3, payload=4 bytes'
}

test_last_line_without_newline() {
    printf 'task create A 1 7' | batch 0
    has 'Created P1 A'
    has 'released_allocations=1 released_bytes=7'
}

test_blank_and_comment_lines_ignored() {
    script '' '   ' '# task create Hidden 1 1' '#x' tasks
    lacks 'ERROR:'
    has 'records=0/20'
}

test_commands_are_case_sensitive() {
    script TASKS Quit tasks
    times 2 '^ERROR: Invalid command'
}

test_quit_stops_later_commands() {
    script quit 'task create Never 1 1'
    lacks 'Created P'
}

test_quit_with_argument_rejected() {
    script 'quit now' tasks
    line "$SYNTAX_ERROR"
    has 'records=0/20'
}

# ---- Launch and interface ----

test_phase_3_banner() {
    script quit
    line 'MiniOS for a Study Room | C11 Phase 3'
    line 'One simulated CPU | quantum=2 | payload budget=65536 bytes'
}

test_help_lists_every_command() {
    local command
    script help
    for command in 'task create' 'session start' 'block PID' 'wake PID' 'quantum N' step run metrics \
                   export memory 'note create' 'note append' 'note read' 'note delete' check reset \
                   fail-next-allocation quit; do
        has "$command"
    done
}

test_interactive_prompt() {
    echo quit | launch 0 --data-dir "$DATA"
    has 'minios> Shutdown:'
}

test_default_data_directory() {
    echo quit | launch 0 --batch
    has 'event=START' "$DATA/events.log"
}

test_unknown_option_rejected() {
    launch 1 --bogus < /dev/null
    has 'Usage:' "$CASE/err"
}

test_data_dir_option_without_value_rejected() {
    launch 1 --data-dir < /dev/null
    has 'Usage:' "$CASE/err"
}

test_missing_data_directory_rejected() {
    launch 1 --batch --data-dir "$CASE/absent" < /dev/null
    has 'Cannot open event log' "$CASE/err"
}

test_overlong_data_directory_rejected() {
    launch 1 --batch --data-dir "$(repeat 433 d)" < /dev/null
    has 'Invalid quantum or data directory path' "$CASE/err"
}

# ---- Scheduling output, trace, and export ----

test_run_echoes_exactly_200_slices_without_summary() {
    script 'task create A 200 1' 'quantum 1' run
    times 200 '^\['
    lacks 'further slice'
}

test_run_hides_slice_201() {
    script 'task create A 201 1' 'quantum 1' run
    times 200 '^\['
    has '... 1 further slice(s) not shown'
    line 'Run complete at tick 201: no READY or WAITING tasks remain.'
}

test_hidden_slices_stay_in_trace() {
    script 'task create A 500 1' 'quantum 1' run metrics
    times 500 '^1,1,' "$DATA/trace.csv"
    line '1,1,499,500,0,FINISHED' "$DATA/trace.csv"
    line 'Dispatches=500 (all tasks in this experiment)'
}

test_hidden_slices_keep_event_log_bounded() {
    script 'session start S 500 1' 'quantum 1' run
    times 200 'event=DISPATCH' "$DATA/events.log"
    times 201 'event=SLICE' "$DATA/events.log"
    times 1 'event=SESSION_COMPLETE' "$DATA/events.log"
    has 'slices=500 hidden=300 end=500' "$DATA/events.log"
    line 'SESSION COMPLETE: P1 S at tick 500'
}

test_echo_limit_restarts_for_each_run() {
    script 'quantum 1' 'task create A 150 1' run 'task create B 150 1' run
    times 300 '^\['
    lacks 'further slice'
}

test_long_task_name_shown_in_full() {
    local name
    name="$(repeat 41 n)_suffix" # 48 characters, the maximum
    script "task create $name 1 1" tasks run metrics
    has "1   $name READY"
    has "P1 $name RUNNING -> FINISHED"
    has "1   $name 0 "
}

# Exact rows: a 13-character name is one past the narrowest (12-character) column and
# must appear whole, while a short name keeps the original padding in every table.
test_task_name_over_12_characters_keeps_table_format() {
    script 'task create Short 1 1' 'task create ThirteenChars 1 1' tasks run metrics
    line "1   Short$(repeat 16 ' ')READY$(repeat 5 ' ')1$(repeat 5 ' ')1$(repeat 4 ' ')0$(repeat 7 ' ')1"
    line "2   ThirteenChars$(repeat 8 ' ')READY$(repeat 5 ' ')1$(repeat 5 ' ')1$(repeat 4 ' ')0$(repeat 7 ' ')1"
    line "[0,1) P1 Short$(repeat 8 ' ')RUNNING -> FINISHED remaining=0"
    line '[1,2) P2 ThirteenChars RUNNING -> FINISHED remaining=0'
    line "1   Short$(repeat 12 ' ')0$(repeat 3 ' ')1$(repeat 5 ' ')0$(repeat 5 ' ')1$(repeat 4 ' ')1$(repeat 4 ' ')0$(repeat 4 ' ')0$(repeat 4 ' ')1"
    line "2   ThirteenChars$(repeat 4 ' ')0$(repeat 3 ' ')1$(repeat 5 ' ')1$(repeat 5 ' ')2$(repeat 4 ' ')2$(repeat 4 ' ')1$(repeat 4 ' ')1$(repeat 4 ' ')1"
}

test_step_reports_scheduler_error() {
    local status=0
    echo step | "$UNIT" "$DATA" --queue-probe > "$CASE/out" 2> "$CASE/err" || status=$?
    same 1 "$status"
    line 'ERROR: Invalid scheduler queue entry.'
    has 'FATAL: internal invariant failed.' "$CASE/err"
}

test_step_is_never_hidden() {
    { echo 'task create A 250 1'; echo 'quantum 1'; for _ in {1..250}; do echo step; done; } | batch 0
    times 250 '^\['
}

test_trace_numbers_experiments_after_reset() {
    script 'task create A 1 1' run reset 'task create B 2 1' run
    same $'experiment,pid,start,end,remaining,state\n1,1,0,1,0,FINISHED\n2,1,0,2,0,FINISHED' \
         "$(cat "$DATA/trace.csv")"
}

test_trace_restarts_and_events_append_per_launch() {
    script 'task create A 1 1' run
    script quit
    same 'experiment,pid,start,end,remaining,state' "$(cat "$DATA/trace.csv")"
    times 2 'event=START' "$DATA/events.log"
}

test_export_contains_finished_tasks_only() {
    script 'task create A 1 1' 'task create B 5 1' step export
    line 'OK: metrics.csv exported (finished tasks only).'
    same $'pid,name,arrival,burst,first_run,completion,turnaround,waiting,response,dispatches\n1,A,0,1,0,1,1,0,0,1' \
         "$(cat "$DATA/metrics.csv")"
}

test_export_replaces_previous_file() {
    script 'task create A 1 1' run export reset export
    times 1 '.*' "$DATA/metrics.csv"
}

# ---- Measured shutdown ----

test_shutdown_counts_ready_and_waiting_payloads() {
    script 'task create R 3 1000' 'task create W 3 500' 'task create F 1 77' 'block 2' step step quit
    line 'Shutdown: used_bytes=0 allocations=0 released_allocations=2 released_bytes=1500'
}

test_shutdown_after_all_finished_releases_nothing() {
    script 'task create A 1 9' run quit
    line 'Shutdown: used_bytes=0 allocations=0 released_allocations=0 released_bytes=0'
}

test_shutdown_after_reset_releases_nothing() {
    script 'task create A 1 9' reset quit
    line 'Shutdown: used_bytes=0 allocations=0 released_allocations=0 released_bytes=0'
}

test_shutdown_on_eof_is_measured() {
    script 'task create A 4 321'
    line 'Shutdown: used_bytes=0 allocations=0 released_allocations=1 released_bytes=321'
}

test_shutdown_event_matches_console() {
    script 'task create A 4 321' 'task create B 4 9' quit
    has 'event=SHUTDOWN pid=0 used_bytes=0 allocations=0 released_allocations=2 released_bytes=330' \
        "$DATA/events.log"
}

test_shutdown_line_is_not_hard_coded() {
    "$UNIT" "$DATA" --shutdown-probe < /dev/null > "$CASE/out"
    line 'Shutdown: used_bytes=7 allocations=1 released_allocations=3 released_bytes=90'
}

test_invariant_failure_stops_and_cleans_up() {
    local status=0
    echo tasks | "$UNIT" "$DATA" --shutdown-probe > "$CASE/out" 2> "$CASE/err" || status=$?
    same 1 "$status"
    has 'FATAL: internal invariant failed.' "$CASE/err"
    has 'released_allocations=3 released_bytes=90'
}

# ---- Injected I/O failures ----

test_event_log_write_failure_sets_exit_status() {
    ln -s /dev/full "$DATA/events.log"
    echo quit | batch 1
    has 'WARNING: event log write failed.' "$CASE/err"
}

test_trace_header_write_failure_stops_startup() {
    ln -s /dev/full "$DATA/trace.csv"
    echo quit | batch 1
    has 'Cannot write scheduling trace.' "$CASE/err"
}

test_trace_write_failure_during_run_detected() {
    local status=0
    ln -s /dev/null "$DATA/events.log"
    # An 8 KiB file-size limit makes the trace fail; output uses pipes, which have no limit.
    printf 'task create A 1000 1\nquantum 1\nrun\nquit\n' |
        (trap '' XFSZ; ulimit -f 8; exec "$MINIOS" --batch --data-dir "$DATA") 2>&1 |
        cat > "$CASE/out" || status=$?
    same 1 "$status"
    has 'WARNING: scheduling trace'
}

test_trace_flush_failure_detected() {
    local status=0
    ln -s /dev/null "$DATA/events.log"
    # 180 slices are all echoed, so each is flushed; their 3.5 KiB trace hits the 2 KiB limit.
    printf 'task create A 180 1\nquantum 1\nrun\nquit\n' |
        (trap '' XFSZ; ulimit -f 2; exec "$MINIOS" --batch --data-dir "$DATA") 2>&1 |
        cat > "$CASE/out" || status=$?
    same 1 "$status"
    has 'WARNING: scheduling trace flush failed.'
}

test_trace_write_failure_warned_once() {
    local status=0
    ln -s /dev/null "$DATA/events.log"
    # About 400 KiB of trace against an 8 KiB limit fails on every buffer flush.
    printf 'task create A 20000 1\nquantum 1\nrun\nquit\n' |
        (trap '' XFSZ; ulimit -f 8; exec "$MINIOS" --batch --data-dir "$DATA") > /dev/null 2> "$CASE/err" ||
        status=$?
    same 1 "$status"
    times 1 'WARNING: scheduling trace write failed' "$CASE/err"
}

test_stdout_write_failure_sets_exit_status() {
    local status=0
    printf 'help\ntask create A 3 10\nrun\nquit\n' |
        "$MINIOS" --batch --data-dir "$DATA" > /dev/full 2> "$CASE/err" || status=$?
    same 1 "$status"
    has 'ERROR: standard output write failed.' "$CASE/err"
    has 'event=SHUTDOWN' "$DATA/events.log"
}

test_closed_output_pipe_is_reported_after_cleanup() {
    # About 350 KiB of output overfills the pipe after head has already exited.
    { for _ in {1..1000}; do echo help; done; } |
        { status=0; "$MINIOS" --batch --data-dir "$DATA" 2> "$CASE/err" || status=$?
          echo "$status" > "$CASE/status"; } |
        head -c 10 > /dev/null || true
    same 1 "$(cat "$CASE/status")"
    has 'ERROR: standard output write failed.' "$CASE/err"
    has 'event=SHUTDOWN' "$DATA/events.log"
}

test_export_write_failure_reported() {
    ln -s /dev/full "$DATA/metrics.csv"
    script export
    line 'ERROR: Metrics write/close failed.'
}

test_export_open_failure_reported() {
    mkdir "$DATA/metrics.csv"
    script export
    line 'ERROR: Cannot open metrics.csv.'
}

test_note_write_failure_reported() {
    ln -s /dev/full "$DATA/notes/full.txt"
    script 'note append full text'
    has 'ERROR: Note write/close failed'
}

test_stdin_read_failure_sets_exit_status() {
    batch 1 < "$CASE"
    has 'ERROR: standard input read failed.' "$CASE/err"
    has 'Shutdown:'
}

# ---- Gantt script input validation (matplotlib is not needed) ----

gantt_rejects() {
    local status=0
    python3 -B "$ROOT/scripts/gantt_chart.py" "$1" -o "$CASE/chart.png" > "$CASE/out" 2> "$CASE/err" || status=$?
    same 1 "$status"
    has 'ERROR:' "$CASE/err"
    lacks Traceback "$CASE/err"
}

test_gantt_rejects_empty_trace() {
    : > "$CASE/trace.csv"
    gantt_rejects "$CASE/trace.csv"
}

test_gantt_rejects_binary_trace() {
    printf '\377\376\000bad\n' > "$CASE/trace.csv"
    gantt_rejects "$CASE/trace.csv"
}

test_gantt_rejects_oversized_field() {
    { echo experiment,pid,start,end,remaining,state; repeat 140000 a; echo; } > "$CASE/trace.csv"
    gantt_rejects "$CASE/trace.csv"
}

# ---- Experiment script: results are saved only after every run is validated ----

# A one-task workload, and a results folder already holding placeholder results.
experiment_fixture() {
    local file
    mkdir -p "$CASE/workloads" "$CASE/results/runs/A_good_q1" "$CASE/results/runs/B_bad_q1"
    echo 'task create X 3 10' > "$CASE/workloads/A_good.txt"
    for file in runs/A_good_q1/trace.csv runs/A_good_q1/metrics.csv runs/A_good_q1/console.txt \
                runs/B_bad_q1/trace.csv runs/B_bad_q1/metrics.csv runs/B_bad_q1/console.txt \
                quantum_results.csv quantum_results_by_class.csv quantum_results.md; do
        echo placeholder > "$CASE/results/$file"
    done
}

# experiments EXIT_STATUS [BINARY]: run the script on $CASE/workloads with quantum 1 into $CASE/results.
experiments() {
    local status=0
    python3 -B "$ROOT/scripts/run_quantum_experiments.py" --binary "${2:-$MINIOS}" --quanta 1 \
        --workloads "$CASE/workloads" --out "$CASE/results" > "$CASE/out" 2> "$CASE/err" || status=$?
    same "$1" "$status"
}

results_snapshot() {
    (cd "$CASE/results" && find . -type f -print0 | sort -z | xargs -0 sha256sum)
}

test_experiment_failure_keeps_existing_results() {
    local before
    experiment_fixture
    # A blocked task never finishes. The simulator itself succeeds, so this is caught
    # only by the script's own validation, after the point where results used to be copied.
    printf 'task create Y 3 10\nblock 1\n' > "$CASE/workloads/B_bad.txt"
    before=$(results_snapshot)
    experiments 1
    has 'ERROR: B_bad q1: 1 tasks created but 0 finished' "$CASE/err"
    # Neither the failed run nor the earlier valid run has replaced anything.
    same "$before" "$(results_snapshot)"
}

test_experiment_rejects_stale_binary() {
    local before
    experiment_fixture
    printf '#!/bin/sh\necho "MiniOS for a Study Room | C11 Phase 2 prototype"\n' > "$CASE/old-minios"
    chmod +x "$CASE/old-minios"
    before=$(results_snapshot)
    experiments 1 "$CASE/old-minios"
    has 'is not a current MiniOS Phase 3 build' "$CASE/err"
    same "$before" "$(results_snapshot)"
}

test_experiment_success_saves_results() {
    local run=$CASE/results/runs/A_good_q1
    experiment_fixture
    experiments 0
    same $'experiment,pid,start,end,remaining,state\n1,1,0,1,2,READY\n1,1,1,2,1,READY\n1,1,2,3,0,FINISHED' \
         "$(cat "$run/trace.csv")"
    same $'pid,name,arrival,burst,first_run,completion,turnaround,waiting,response,dispatches\n1,X,0,3,0,3,3,0,0,3' \
         "$(cat "$run/metrics.csv")"
    has 'Run complete at tick 3: no READY or WAITING tasks remain.' "$run/console.txt"
    has 'A_good,RR q1,1,1,0.00,3.00,0.00,0,3,3,0.3333,100.00' "$CASE/results/quantum_results.csv"
    has 'A_good,RR q1,1,X,1,0.00,3.00,0.00' "$CASE/results/quantum_results_by_class.csv"
    has '| RR q1 | 0.00 | 3.00 | 0.00 | 0 | 3 | 3 | 0.3333 |' "$CASE/results/quantum_results.md"
    lacks placeholder "$run/trace.csv"
    has placeholder "$CASE/results/runs/B_bad_q1/trace.csv"
}
