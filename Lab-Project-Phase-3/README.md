# MiniOS for a Study Room

**ICS 433 Operating Systems Lab | Phase 3 (final)**

A C11 terminal application for simulated study tasks, round-robin scheduling,
task-owned memory, persistent revision notes, and study sessions.

## Team

| Name | KFUPM ID |
|---|---|
| Yasmeen Alshehri | 202271660 |
| Fatimah Alshehab | 202278660 |
| Raneem Alshahrani | 202277080 |
| Sarah Almas | 202261060 |

## Operating-system concepts

| Concept | Where | What is demonstrated |
|---|---|---|
| Process abstraction and states | `tasks.c`, `core.h` | Task records with PIDs and READY, RUNNING, WAITING, FINISHED states |
| CPU scheduling | `scheduler.c` | Round robin over a circular FIFO queue; FCFS with a large quantum |
| Memory accounting | `memory.c` | PID ownership, a 64 KiB budget, rollback, release on completion |
| Persistence | `notes.c` | Notes that survive reset and restart |
| Accounting and logging | `logger.c`, `scheduler.c` | Event log, slice trace, scheduling metrics |
| Consistency | `core.c` | Invariant check after every command |

## Files

| Location | Purpose |
|---|---|
| `src/` | Nine commented C implementation files. |
| `include/` | Eight commented headers containing interfaces and shared types. |
| `tests/cases/` | Functional test inputs (`.in`) and hand-checked responses (`.expected`). |
| `tests/edge_cases.sh` | Boundary, persistence, and injected-fault tests. |
| `tests/helpers.sh` | Assertion helpers used by the shell tests. |
| `tests/test_core.c` | Unit tests for invariants, queue, memory, and shutdown. |
| `tests/test_model.py` | Independent reference model and experiment regression tests. |
| `scripts/run_tests.sh` | Test runner with PASS/FAIL results. |
| `scripts/run_quantum_experiments.py` | Runs the quantum experiments. |
| `scripts/gantt_chart.py` | Draws a Gantt chart from a trace. |
| `experiments/workloads/` | The three experiment workloads. |
| `experiments/results/` | Stored tables, charts, and raw output of each run. |
| `docs/` | Phase 2 progress report and screenshots. |
| `data/` | Runtime logs, traces, exported metrics, and notes. |
| `Makefile` | Build, run, test, sanitizer, Valgrind, and cleanup commands. |

### Phase 2 documentation

These show the Phase 2 version, so the banner, `metrics`, `run`, and shutdown
messages differ from the current program.

- [Phase 2 Progress Report](docs/Progress%20Report.pdf)
- [Screenshot 1](docs/Photo1.png): Application startup, command help, and task creation.
- [Screenshot 2](docs/Photo2.png): Task blocking, waking, and round-robin execution.
- [Screenshot 3](docs/Photo3.png): Task metrics, memory verification, CSV export, and note operations.
- [Screenshot 4](docs/Photo4.png): Note deletion, reset, forced allocation failure handling, and clean shutdown.

## Requirements

Use Linux, or Ubuntu through WSL.

| Task | Needs |
|---|---|
| Build and run | GCC and GNU Make; the application uses only the standard C library |
| `make test` | Also Bash and Python 3 (standard library only) |
| `make sanitize` | Also the GCC sanitizer runtimes (included with Ubuntu's GCC) |
| `make valgrind` | Also Valgrind |
| Gantt charts | Also matplotlib, see [Gantt charts](#gantt-charts) |

```sh
sudo apt update
sudo apt install build-essential python3 python3-venv valgrind
```

## Build and run

Run all commands from the folder that contains `Makefile`.

```sh
cd Lab-Project-Phase-3
make
make run
```

The program displays `minios>`. Enter `help` to see its commands.
Run `make` commands in the Linux terminal; enter MiniOS commands at `minios>`.

`make run` creates `data/notes/`. To launch the program directly, create the
directories first:

```sh
mkdir -p data/notes
./minios                                          # interactive, uses data/
./minios --batch < tests/cases/01_phase2_scenario.in   # scripted, echoes commands
./minios --data-dir OTHER_DIRECTORY               # use another existing directory
```

The exit status is 1 for a startup, input, log/trace, or invariant failure. A
rejected command prints `ERROR:` but does not change the exit status.

## Commands

| Command | Purpose |
|---|---|
| `task create NAME BURST BYTES` | Create a READY task. |
| `session start NAME TICKS BYTES` | Create a study-session task. |
| `tasks` | Show task records and ready-queue order. |
| `block PID` / `wake PID` | Move a task between READY and WAITING. |
| `quantum N` | Set the time quantum for future slices. |
| `step` / `run` | Execute one slice / execute until no READY task remains. |
| `metrics` / `export` | Show metrics / save completed-task metrics as CSV. |
| `memory` | Show current payload bytes and allocation count. |
| `note create NAME TEXT` | Create a new note without replacing an existing one. |
| `note append NAME TEXT` | Append a line to an existing note. |
| `note read NAME` / `note delete NAME` | Read or delete a note. |
| `check` | Check all invariants. |
| `reset` | Release payloads, clear task records, and reset the virtual clock. |
| `debug fail-next-allocation` | Demonstrate safe handling of an allocation failure. |
| `help` / `quit` | Show commands / clean up and exit. |

Names use 1-48 ASCII letters, digits, underscores, or hyphens. Bursts and quantum
are positive integers up to 1,000,000. Task records are limited to 20, including
finished records until reset. Each payload is 1-65,536 bytes, subject to the total
65,536-byte budget. Time is measured in virtual ticks. Note text can contain spaces.
An input line holds at most 2,046 bytes. A longer line, or a line containing a
NUL byte, is rejected and discarded whole; no part of it runs as a command.

`run` prints its first 200 slices and summarizes the rest. `trace.csv` always
records every slice.

## Quick example

```text
session start Study 4 256
task create Review 3 128
task create Practice 2 64
run
metrics
memory
quit
```

With quantum 2, dispatch order is P1, P2, P3, P1, P2; execution ends at tick 9.

## Metrics

| Metric | Definition | Tasks included |
|---|---|---|
| Turnaround | completion - arrival | Finished |
| Waiting | ticks spent READY, excluding WAITING time | Finished |
| Response | first run - arrival | Finished |
| Dispatches | CPU slices granted | All in the experiment |
| Makespan | latest completion - earliest arrival | Finished |
| Throughput | finished tasks / makespan, in tasks per tick | Finished |
| CPU utilization | executed ticks / elapsed ticks | All in the experiment |

A dispatch is not always a context switch, because the last READY task can be
dispatched repeatedly. CPU utilization is always 100% once time has passed,
because the model has no idle time; do not report it as a performance result.

## Invariants and shutdown statistics

`app_check()` runs after every command and on `check`. It verifies:

- **Identity:** PIDs are 1..n, the next PID is n+1, and names are valid.
- **State:** no task is RUNNING between commands, a task is FINISHED exactly
  when no work remains, and a session is notified exactly when finished.
- **Time:** arrivals follow creation order; first run, completion, waiting, and
  dispatch counts fit the work done; executed ticks equal the virtual clock.
- **Queue:** each READY task is queued once and nothing else is queued.
- **Memory:** each unfinished task owns one payload, finished tasks own none,
  and the totals match the registry and the budget.

A violation prints `FATAL: internal invariant failed.`, cleans up, and exits
with status 1.

Shutdown reports measured values:

```text
Shutdown: used_bytes=0 allocations=0 released_allocations=2 released_bytes=1500
```

The first two values are what remains in the accounting after cleanup. The last
two are what cleanup released, for example tasks still READY or WAITING at exit.

## Testing

```sh
make test        # functional, edge, fault, unit, model, and regression tests
make sanitize    # the same suite under ASan, LeakSanitizer, and UBSan
make valgrind    # the same suite under Valgrind (slower, about a minute)
```

`bash scripts/run_tests.sh` is equivalent to `make test`. Each test prints
`PASS` or `FAIL`, and the run ends with a summary:

```text
SUMMARY mode=<mode> total=<n> passed=<n> failed=<n>
RESULT: PASS or FAIL
```

Any failure makes the command exit with a nonzero status. To save evidence for
the report:

```sh
mkdir -p build
set -o pipefail
make test 2>&1 | tee build/test.log
```

| Group | What it covers |
|---|---|
| Functional cases | Round robin, FCFS, metrics, block/wake, reset, allocation failure, budget, notes, sessions, invalid input |
| Edge and fault cases | 20-task limit, persistence across restarts, input limits, CRLF, output limit, shutdown totals, failed writes and reads |
| Unit tests | A targeted corruption for each invariant, real `malloc` failure, `free` counts, queue wraparound |
| Reference model | Random scenarios compared with an independent Python model (500; 40 under Valgrind) |
| Regression | Stored experiment results match the model and are reproduced by the current binary |
| Safety | `data/` and `experiments/` are unchanged by the tests |

### Sanitizers

`make sanitize` builds separate binaries in `build/` with AddressSanitizer,
LeakSanitizer, and UndefinedBehaviorSanitizer. Any report fails the test that
caused it, even if the program output was correct. For a manual session:

```sh
mkdir -p data/notes
make build/minios-san
./build/minios-san
```

### Valgrind

`make valgrind` fails a test on any invalid access, use of an uninitialized
value, or unfreed block. For a manual session:

```sh
mkdir -p data/notes
make build/minios-debug
valgrind --leak-check=full --show-leak-kinds=all --error-exitcode=99 ./build/minios-debug
```

### Adding a functional case

Create `tests/cases/NAME.in` with commands and `tests/cases/NAME.expected` with
the expected responses, leaving out the three banner lines and the echoed
commands. Work out the expected values by hand; do not copy them from the output
of the program being tested.

## Experiments

Three workloads (mixed, uniform, stress) run with quantum 1, 2, 4, 8, and an
FCFS-equivalent quantum of 1,000,000. The results are in
[quantum_results.md](experiments/results/quantum_results.md).

To reproduce them without overwriting the stored results:

```sh
make
python3 scripts/run_quantum_experiments.py --out build/experiments
diff --strip-trailing-cr experiments/results/quantum_results.csv build/experiments/quantum_results.csv
```

No `diff` output means the results are identical. Without `--out`, the script
replaces the files in `experiments/results/`.

### Gantt charts

```sh
python3 -m venv .venv
.venv/bin/pip install -r scripts/requirements.txt
mkdir -p build
.venv/bin/python scripts/gantt_chart.py experiments/results/runs/W1_mixed_q2/trace.csv \
    -m experiments/results/runs/W1_mixed_q2/metrics.csv -o build/gantt.png
```

With no arguments the script reads `data/trace.csv` and writes `data/gantt.png`.
Use `-e N` to choose an experiment when the trace contains a `reset`.

## Runtime files and cleanup

| File | Behavior |
|---|---|
| `data/events.log` | Appended on every launch |
| `data/trace.csv` | Replaced on every launch |
| `data/metrics.csv` | Replaced by `export` |
| `data/notes/NAME.txt` | Kept across reset and restart until deleted |

Task records and the virtual clock do not survive restart. Both `quit` and EOF
release outstanding payloads and close files.

`make clean` removes `minios` and `build/` while preserving saved notes, logs,
and CSV outputs. Leave `minios`, `build/`, `.venv/`, and runtime files in `data/`
out of the submission ZIP.

## Known limitations

- One virtual CPU, no context-switch cost, and no idle time.
- All experiment tasks arrive at tick 0. The clock moves only during slices, so
  staggered arrivals cannot be kept identical across different quanta.
- Blocking is a manual command; there is no timed I/O or automatic wake.
- Memory management is quota and ownership accounting over `malloc`, without
  placement, fragmentation, or paging.
- `trace.csv` is unbounded: about 690 MB in the worst case (20 tasks of
  1,000,000 ticks at quantum 1).
- A failed write to standard output is not reflected in the exit status.
- When the trace cannot be written during a long run, the warning can repeat
  many times.
- Two instances must not share a data directory. On case-insensitive file
  systems such as `/mnt/c`, note names that differ only in case are the same note.
