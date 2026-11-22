#!/usr/bin/env python3
"""Compare MiniOS with an independent round-robin reference model.

The model is written from the documented rules and shares no code or counters with
the C program. In particular, it derives waiting time as
turnaround - burst - blocked time, whereas MiniOS accumulates READY ticks.

1. Random command scripts (fixed seeds) must give the same trace, metrics, state,
   memory totals, number of rejected commands, and shutdown totals.
2. The stored experiment results must match what the model computes from the
   workload files alone, and the binary under test must still reproduce them.

Nothing in the project directory is written. Standard library only.
"""

import argparse
import collections
import csv
import os
import random
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CAPACITY = 20
BUDGET = 65536
FCFS_QUANTUM = 1000000
METRIC_FIELDS = ["pid", "name", "arrival", "burst", "first_run", "completion",
                 "turnaround", "waiting", "response", "dispatches"]
TRACE_FIELDS = ["experiment", "pid", "start", "end", "remaining", "state"]


class Task:
    def __init__(self, pid, name, burst, size, arrival):
        self.pid, self.name, self.burst, self.size, self.arrival = pid, name, burst, size, arrival
        self.left = burst
        self.first = self.done = self.blocked_at = None
        self.blocked = self.dispatches = 0

    @property
    def state(self):
        if self.done is not None:
            return "FINISHED"
        return "WAITING" if self.blocked_at is not None else "READY"


class Model:
    def __init__(self):
        self.quantum, self.experiment = 2, 1
        self.trace, self.errors = [], 0
        self.clear()

    def clear(self):
        self.tasks, self.ready, self.now, self.fail_next = {}, collections.deque(), 0, False

    def live(self):
        return [t for t in self.tasks.values() if t.done is None]

    def used(self):
        return sum(t.size for t in self.live())

    def create(self, name, burst, size):
        if len(self.tasks) == CAPACITY or self.used() + size > BUDGET:
            self.errors += 1
        elif self.fail_next:
            self.fail_next = False
            self.errors += 1
        else:
            task = Task(len(self.tasks) + 1, name, burst, size, self.now)
            self.tasks[task.pid] = task
            self.ready.append(task)

    def step(self):
        if not self.ready:
            return
        task = self.ready.popleft()
        if task.first is None:
            task.first = self.now
        ran = min(self.quantum, task.left)
        start, self.now = self.now, self.now + ran
        task.left -= ran
        task.dispatches += 1
        if task.left:
            self.ready.append(task)
        else:
            task.done = self.now
        self.trace.append([str(v) for v in (self.experiment, task.pid, start, self.now, task.left,
                                            "READY" if task.left else "FINISHED")])

    def apply(self, command):
        verb, *args = command.split()
        if verb in ("task", "session"):
            self.create(args[1], int(args[2]), int(args[3]))
        elif verb == "quantum":
            self.quantum = int(args[0])
        elif verb == "step":
            self.step()
        elif verb == "run":
            while self.ready:
                self.step()
        elif verb == "reset":
            self.clear()
            self.experiment += 1
        elif verb == "debug":
            self.fail_next = True
        elif verb == "block":
            task = self.tasks.get(int(args[0]))
            if task and task.state == "READY":
                self.ready.remove(task)
                task.blocked_at = self.now
            else:
                self.errors += 1
        elif verb == "wake":
            task = self.tasks.get(int(args[0]))
            if task and task.state == "WAITING":
                task.blocked += self.now - task.blocked_at
                task.blocked_at = None
                self.ready.append(task)
            else:
                self.errors += 1
        else:
            raise ValueError("model does not support: " + command)

    def metrics(self):
        rows = []
        for t in self.tasks.values():
            if t.done is not None:
                turnaround = t.done - t.arrival
                rows.append([str(v) for v in (t.pid, t.name, t.arrival, t.burst, t.first, t.done, turnaround,
                                              turnaround - t.burst - t.blocked, t.first - t.arrival,
                                              t.dispatches)])
        return rows


def read_rows(path, header):
    with open(path, newline="") as stream:
        rows = list(csv.reader(stream))
    if not rows or rows[0] != header:
        raise AssertionError(f"{path}: unexpected header")
    return rows[1:]


def random_script(rng):
    commands = []
    for number in range(rng.randint(1, 120)):
        kind = rng.choices(["task", "session", "step", "run", "block", "wake", "quantum", "reset", "debug"],
                           [14, 6, 25, 3, 9, 9, 4, 2, 1])[0]
        if kind in ("task", "session"):
            verb = "task create" if kind == "task" else "session start"
            burst = rng.choice([1, 2, rng.randint(1, 12), rng.randint(1, 40)])
            size = rng.choice([1, rng.randint(1, 4000), rng.randint(1, 4000), rng.randint(1, BUDGET)])
            commands.append(f"{verb} T{number} {burst} {size}")
        elif kind in ("block", "wake"):
            commands.append(f"{kind} {rng.randint(1, 8) if rng.random() < 0.7 else rng.randint(1, 22)}")
        elif kind == "quantum":
            commands.append(f"quantum {rng.choice([1, 2, 3, 4, 8, FCFS_QUANTUM])}")
        elif kind == "debug":
            commands.append("debug fail-next-allocation")
        else:
            commands.append(kind)
    return commands


def compare(binary, commands):
    model = Model()
    for command in commands:
        model.apply(command)
    with tempfile.TemporaryDirectory(prefix="minios-model-") as data:
        script = "\n".join(commands + ["tasks", "metrics", "memory", "check", "export", "quit"]) + "\n"
        result = subprocess.run([binary, "--batch", "--data-dir", data], input=script,
                                capture_output=True, text=True, timeout=120)
        out = result.stdout
        assert result.returncode == 0 and not result.stderr, (result.returncode, result.stderr)
        assert read_rows(os.path.join(data, "trace.csv"), TRACE_FIELDS) == model.trace, "trace differs"
        assert read_rows(os.path.join(data, "metrics.csv"), METRIC_FIELDS) == model.metrics(), "metrics differ"

    assert out.count("ERROR:") == model.errors, "number of rejected commands differs"
    assert "CHECK PASS" in out, "invariant check did not pass"
    queue = " ".join(f"P{t.pid}" for t in model.ready) or "(empty)"
    assert f"Ready queue: {queue}\n" in out, "ready queue differs"
    assert f"Tasks | tick={model.now} quantum={model.quantum} records={len(model.tasks)}/20\n" in out, \
        "task summary differs"
    for t in model.tasks.values():
        row = rf"^{t.pid} +{t.name} +{t.state} +{t.burst} +{t.left} +{t.arrival} +{t.size}$"
        assert re.search(row, out, re.M), f"P{t.pid} task row differs"
    dispatches = sum(t.dispatches for t in model.tasks.values())
    assert f"Dispatches={dispatches} " in out, "dispatch total differs"
    used, count = model.used(), len(model.live())
    assert f"Memory: used={used}/{BUDGET} bytes free={BUDGET - used} allocations={count}\n" in out, \
        "memory totals differ"
    assert ("Shutdown: used_bytes=0 allocations=0 "
            f"released_allocations={count} released_bytes={used}\n") in out, "shutdown totals differ"


def check_random(binary, count):
    for seed in range(count):
        commands = random_script(random.Random(seed))
        try:
            compare(binary, commands)
        except AssertionError as error:
            raise AssertionError(f"seed {seed}: {error}; script: {'; '.join(commands)}")


def experiment_cases():
    folder = os.path.join(ROOT, "experiments", "workloads")
    for file_name in sorted(os.listdir(folder)):
        if not file_name.endswith(".txt"):
            continue
        with open(os.path.join(folder, file_name)) as stream:
            commands = [line.strip() for line in stream if line.strip() and not line.startswith("#")]
        for label, quantum in (("q1", 1), ("q2", 2), ("q4", 4), ("q8", 8), ("FCFS", FCFS_QUANTUM)):
            model = Model()
            for command in [f"quantum {quantum}"] + commands + ["run"]:
                model.apply(command)
            yield file_name[:-4], label, model


def check_stored_runs():
    runs = os.path.join(ROOT, "experiments", "results", "runs")
    seen = 0
    for workload, label, model in experiment_cases():
        run = os.path.join(runs, f"{workload}_{label}")
        assert read_rows(os.path.join(run, "trace.csv"), TRACE_FIELDS) == model.trace, f"{run}: trace"
        assert read_rows(os.path.join(run, "metrics.csv"), METRIC_FIELDS) == model.metrics(), f"{run}: metrics"
        seen += 1
    assert seen == 15, f"expected 15 stored runs, found {seen}"


def check_stored_summary():
    path = os.path.join(ROOT, "experiments", "results", "quantum_results.csv")
    with open(path, newline="") as stream:
        stored = {(r["workload"], r["policy"]): r for r in csv.DictReader(stream)}
    cases = list(experiment_cases())
    assert len(stored) == len(cases), "summary row count differs"
    for workload, label, model in cases:
        row = stored[(workload, label if label == "FCFS" else f"RR {label}")]
        tasks = list(model.tasks.values())
        waits = [t.done - t.arrival - t.burst for t in tasks]
        span = max(t.done for t in tasks) - min(t.arrival for t in tasks)
        expected = {"mean_waiting": sum(waits) / len(tasks),
                    "mean_turnaround": sum(t.done - t.arrival for t in tasks) / len(tasks),
                    "mean_response": sum(t.first - t.arrival for t in tasks) / len(tasks),
                    "max_waiting": max(waits), "dispatches": len(model.trace), "makespan": span}
        for key, value in expected.items():
            assert abs(float(row[key]) - value) <= 0.005 + 1e-9, f"{workload} {label}: {key}"
        assert abs(float(row["throughput"]) - len(tasks) / span) <= 0.00005 + 1e-9, \
            f"{workload} {label}: throughput"


def check_reproduction(binary):
    stored = os.path.join(ROOT, "experiments", "results")

    def text(folder, name):
        with open(os.path.join(folder, name)) as stream:  # universal newlines: CRLF and LF are equal
            return stream.read()

    with tempfile.TemporaryDirectory(prefix="minios-repro-") as fresh:
        result = subprocess.run(
            [sys.executable, "-B", os.path.join(ROOT, "scripts", "run_quantum_experiments.py"),
             "--binary", binary, "--out", fresh], capture_output=True, text=True, timeout=600)
        assert result.returncode == 0, result.stderr
        names = ["quantum_results.csv", "quantum_results_by_class.csv", "quantum_results.md"]
        for run in sorted(os.listdir(os.path.join(stored, "runs"))):
            names += [os.path.join("runs", run, "trace.csv"), os.path.join("runs", run, "metrics.csv")]
        for name in names:
            assert text(fresh, name) == text(stored, name), f"{name} differs from the stored result"


def main():
    parser = argparse.ArgumentParser(description="MiniOS reference-model tests.")
    parser.add_argument("binary")
    parser.add_argument("--scenarios", type=int, default=500)
    args = parser.parse_args()
    binary = os.path.abspath(args.binary)
    checks = [(f"model agrees on {args.scenarios} random scenarios", lambda: check_random(binary, args.scenarios)),
              ("stored experiment runs match the model", check_stored_runs),
              ("stored experiment summary matches the model", check_stored_summary),
              ("experiments reproduce the stored results", lambda: check_reproduction(binary))]
    failed = 0
    for name, check in checks:
        try:
            check()
            print(f"PASS {name}", flush=True)
        except Exception as error:  # report the failure and continue with the other checks
            failed += 1
            print(f"FAIL {name}", flush=True)
            print(f"  {type(error).__name__}: {error}", flush=True)
    print(f"DONE {len(checks)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
