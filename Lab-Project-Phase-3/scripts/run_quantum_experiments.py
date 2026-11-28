#!/usr/bin/env python3
"""Run every workload under several time quanta and tabulate the results.

For each workload file in experiments/workloads/ and each quantum, the script
runs the real MiniOS binary in batch mode with a fresh temporary data directory:

    quantum Q
    <workload commands>
    run
    metrics
    export
    quit

It then computes each metric independently from the exported metrics.csv and
trace.csv, and cross-checks the values against the simulator's own `metrics`
output. Any disagreement, rejected command, or unfinished task aborts the run,
so a reported number is never taken from one source alone. Nothing in the output
directory is created or replaced unless every run passes these checks.

Outputs (in --out, default experiments/results/):
    quantum_results.csv            one row per workload x quantum
    quantum_results_by_class.csv   per task-class means (name without digits)
    quantum_results.md             report-ready tables
    runs/<workload>_<label>/       console output, metrics.csv, trace.csv

Only the Python standard library is required.
Usage: python3 scripts/run_quantum_experiments.py [--binary ./minios]
"""

import argparse
import csv
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FCFS_QUANTUM = 1000000  # MAX_TICKS: >= every allowed burst, so RR degenerates to FCFS
DEFAULT_QUANTA = "1,2,4,8,fcfs"

MEANS_RE = re.compile(r"Means \((\d+) finished\): turnaround=([\d.]+) waiting=([\d.]+) response=([\d.]+) ticks")
MAKESPAN_RE = re.compile(r"Makespan=(\d+) ticks \[(\d+),(\d+)\) throughput=([\d.]+) tasks/tick")
DISPATCH_RE = re.compile(r"Dispatches=(\d+) ")
UTIL_RE = re.compile(r"CPU utilization=([\d.]+)% \(busy=(\d+)/elapsed=(\d+) ticks")


class ExperimentError(Exception):
    pass


def parse_quanta(text):
    quanta = []
    for item in text.split(","):
        item = item.strip().lower()
        if item == "fcfs":
            quanta.append(("FCFS", FCFS_QUANTUM))
        elif item.isdigit() and 1 <= int(item) <= FCFS_QUANTUM:
            quanta.append((f"q{int(item)}", int(item)))
        else:
            raise argparse.ArgumentTypeError(f"invalid quantum {item!r}")
    return quanta


def default_binary():
    for name in ("minios", "minios.exe"):
        path = os.path.join(ROOT, name)
        if os.path.isfile(path):
            return path
    return os.path.join(ROOT, "minios")


def workload_commands(path):
    commands = []
    with open(path, encoding="utf-8") as stream:
        for line in stream:
            line = line.strip()
            if line and not line.startswith("#"):
                commands.append(line)
    if not commands:
        raise ExperimentError(f"{path}: workload has no commands")
    return commands


def task_class(name):
    return name.rstrip("0123456789") or name


def mean(values):
    return sum(values) / len(values)


def close(a, b, places):
    return abs(a - b) <= 0.5 * 10 ** (-places) + 1e-9


def run_once(binary, workload, label, quantum, keep_dir):
    commands = workload_commands(workload)
    script = "\n".join([f"quantum {quantum}"] + commands + ["run", "metrics", "export", "quit"]) + "\n"
    with tempfile.TemporaryDirectory(prefix="minios-exp-") as data_dir:
        os.makedirs(os.path.join(data_dir, "notes"))
        result = subprocess.run([binary, "--batch", "--data-dir", data_dir], input=script,
                                capture_output=True, text=True, timeout=600)
        output = result.stdout
        if result.returncode != 0:
            raise ExperimentError(f"exit status {result.returncode}: {result.stderr.strip()}")
        if "ERROR:" in output:
            bad = [line for line in output.splitlines() if "ERROR:" in line]
            raise ExperimentError(f"simulator rejected a command: {bad[0]}")
        with open(os.path.join(data_dir, "metrics.csv"), newline="") as stream:
            tasks = list(csv.DictReader(stream))
        with open(os.path.join(data_dir, "trace.csv"), newline="") as stream:
            trace = list(csv.DictReader(stream))
        # Keep the raw files in memory; main() saves them only after every run is validated.
        files = {}
        for name in ("metrics.csv", "trace.csv"):
            with open(os.path.join(data_dir, name), "rb") as stream:
                files[name] = stream.read()

    created = sum(1 for c in commands if c.startswith(("task create", "session start")))
    if len(tasks) != created:
        raise ExperimentError(f"{created} tasks created but {len(tasks)} finished")

    # Independent computation from the exported per-task rows and the slice trace.
    for t in tasks:
        for key in ("arrival", "burst", "first_run", "completion", "turnaround", "waiting",
                    "response", "dispatches"):
            t[key] = int(t[key])
        if t["turnaround"] != t["completion"] - t["arrival"] or t["response"] != t["first_run"] - t["arrival"]:
            raise ExperimentError(f"P{t['pid']}: per-task metric inconsistent")
        # No blocking in these workloads, so turnaround = burst + waiting must hold.
        if t["turnaround"] != t["burst"] + t["waiting"]:
            raise ExperimentError(f"P{t['pid']}: turnaround != burst + waiting")
    slices = [(int(s["start"]), int(s["end"])) for s in trace]
    makespan = max(t["completion"] for t in tasks) - min(t["arrival"] for t in tasks)
    busy = sum(end - start for start, end in slices)
    elapsed = max(end for _, end in slices)
    row = {
        "tasks": len(tasks),
        "mean_waiting": mean([t["waiting"] for t in tasks]),
        "mean_turnaround": mean([t["turnaround"] for t in tasks]),
        "mean_response": mean([t["response"] for t in tasks]),
        "max_waiting": max(t["waiting"] for t in tasks),
        "dispatches": len(slices),
        "makespan": makespan,
        "throughput": len(tasks) / makespan,
        "cpu_utilization_pct": 100.0 * busy / elapsed,
    }
    if row["dispatches"] != sum(t["dispatches"] for t in tasks):
        raise ExperimentError("trace slice count != sum of per-task dispatches")
    if busy != sum(t["burst"] for t in tasks):
        raise ExperimentError("trace busy ticks != sum of bursts")

    # Cross-check against the simulator's own console metrics.
    means, span, disp, util = (MEANS_RE.search(output), MAKESPAN_RE.search(output),
                               DISPATCH_RE.search(output), UTIL_RE.search(output))
    if not (means and span and disp and util):
        raise ExperimentError("could not find the metrics summary in console output")
    checks = [
        (int(means.group(1)) == row["tasks"], "finished count"),
        (close(float(means.group(2)), row["mean_turnaround"], 2), "mean turnaround"),
        (close(float(means.group(3)), row["mean_waiting"], 2), "mean waiting"),
        (close(float(means.group(4)), row["mean_response"], 2), "mean response"),
        (int(span.group(1)) == row["makespan"], "makespan"),
        (close(float(span.group(4)), row["throughput"], 4), "throughput"),
        (int(disp.group(1)) == row["dispatches"], "dispatches"),
        (close(float(util.group(1)), row["cpu_utilization_pct"], 2), "CPU utilization"),
    ]
    failed = [name for ok, name in checks if not ok]
    if failed:
        raise ExperimentError("simulator output disagrees with independent computation: " + ", ".join(failed))

    classes = {}
    for t in tasks:
        classes.setdefault(task_class(t["name"]), []).append(t)
    by_class = {name: {"tasks": len(group),
                       "mean_waiting": mean([t["waiting"] for t in group]),
                       "mean_turnaround": mean([t["turnaround"] for t in group]),
                       "mean_response": mean([t["response"] for t in group])}
                for name, group in classes.items()}
    return row, by_class, (keep_dir, files, output)


def save_run(keep_dir, files, output):
    """Write one validated run: the simulator's CSV files byte for byte, and its console output."""
    os.makedirs(keep_dir, exist_ok=True)
    for name, data in files.items():
        with open(os.path.join(keep_dir, name), "wb") as stream:
            stream.write(data)
    with open(os.path.join(keep_dir, "console.txt"), "w", encoding="utf-8") as stream:
        stream.write(output)


FIELDS = ["workload", "policy", "quantum", "tasks", "mean_waiting", "mean_turnaround", "mean_response",
          "max_waiting", "dispatches", "makespan", "throughput", "cpu_utilization_pct"]


def fmt(value, places=2):
    return f"{value:.{places}f}" if isinstance(value, float) else str(value)


def write_outputs(out_dir, rows, class_rows):
    with open(os.path.join(out_dir, "quantum_results.csv"), "w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        for r in rows:
            writer.writerow({k: (fmt(r[k], 4) if k == "throughput" else fmt(r[k])) for k in FIELDS})
    class_fields = ["workload", "policy", "quantum", "class", "tasks", "mean_waiting", "mean_turnaround",
                    "mean_response"]
    with open(os.path.join(out_dir, "quantum_results_by_class.csv"), "w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=class_fields)
        writer.writeheader()
        for r in class_rows:
            writer.writerow({k: fmt(r[k]) for k in class_fields})

    lines = ["# MiniOS quantum experiments", "",
             "Generated by `scripts/run_quantum_experiments.py`. All tasks arrive at tick 0;",
             f"FCFS is round robin with quantum {FCFS_QUANTUM} (>= every burst). Times are in virtual ticks;",
             "throughput is tasks per tick. CPU utilization is 100% by model construction",
             "(the virtual clock advances only while a task runs) and is therefore omitted.", ""]
    for workload in sorted({r["workload"] for r in rows}):
        lines += [f"## {workload}", "",
                  "| Policy | Mean waiting | Mean turnaround | Mean response | Max waiting | Dispatches | Makespan | Throughput |",
                  "|---|---:|---:|---:|---:|---:|---:|---:|"]
        for r in (r for r in rows if r["workload"] == workload):
            lines.append(f"| {r['policy']} | {r['mean_waiting']:.2f} | {r['mean_turnaround']:.2f} | "
                         f"{r['mean_response']:.2f} | {r['max_waiting']} | {r['dispatches']} | "
                         f"{r['makespan']} | {r['throughput']:.4f} |")
        classes = sorted({c["class"] for c in class_rows if c["workload"] == workload})
        if len(classes) > 1:
            lines += ["", "Per task class (mean response / mean turnaround):", "",
                      "| Policy | " + " | ".join(classes) + " |", "|---|" + "---:|" * len(classes)]
            for r in (r for r in rows if r["workload"] == workload):
                cells = []
                for name in classes:
                    c = next(c for c in class_rows if c["workload"] == workload
                             and c["policy"] == r["policy"] and c["class"] == name)
                    cells.append(f"{c['mean_response']:.2f} / {c['mean_turnaround']:.2f}")
                lines.append(f"| {r['policy']} | " + " | ".join(cells) + " |")
        lines.append("")
    with open(os.path.join(out_dir, "quantum_results.md"), "w", encoding="utf-8", newline="\n") as stream:
        stream.write("\n".join(lines))


def main(argv=None):
    parser = argparse.ArgumentParser(description="Run MiniOS quantum experiments.")
    parser.add_argument("--binary", default=default_binary(), help="MiniOS executable (default: ./minios)")
    parser.add_argument("--workloads", default=os.path.join(ROOT, "experiments", "workloads"))
    parser.add_argument("--out", default=os.path.join(ROOT, "experiments", "results"))
    parser.add_argument("--quanta", type=parse_quanta, default=parse_quanta(DEFAULT_QUANTA),
                        help=f"comma-separated quanta; 'fcfs' = {FCFS_QUANTUM} (default: {DEFAULT_QUANTA})")
    args = parser.parse_args(argv)
    if not os.path.isfile(args.binary):
        print(f"ERROR: MiniOS binary not found: {args.binary} (build it with make)", file=sys.stderr)
        return 1
    workloads = sorted(f for f in os.listdir(args.workloads) if f.endswith(".txt")) \
        if os.path.isdir(args.workloads) else []
    if not workloads:
        print(f"ERROR: no workload .txt files in {args.workloads}", file=sys.stderr)
        return 1
    rows, class_rows, pending = [], [], []
    for file_name in workloads:
        workload = os.path.splitext(file_name)[0]
        for label, quantum in args.quanta:
            keep = os.path.join(args.out, "runs", f"{workload}_{label}")
            try:
                row, by_class, saved = run_once(args.binary, os.path.join(args.workloads, file_name),
                                                label, quantum, keep)
            except (ExperimentError, OSError, subprocess.SubprocessError, ValueError, KeyError) as error:
                print(f"ERROR: {workload} {label}: {error}", file=sys.stderr)
                return 1
            row.update(workload=workload, policy=label if label == "FCFS" else f"RR {label}", quantum=quantum)
            rows.append(row)
            pending.append(saved)
            for name, values in sorted(by_class.items()):
                class_rows.append(dict(values, workload=workload, policy=row["policy"], quantum=quantum,
                                       **{"class": name}))
            print(f"{workload:12} {row['policy']:6} wait={row['mean_waiting']:7.2f} "
                  f"turn={row['mean_turnaround']:7.2f} resp={row['mean_response']:6.2f} "
                  f"disp={row['dispatches']:4} makespan={row['makespan']} [cross-check OK]")
    # Every run passed, so only now is anything in --out created or replaced.
    try:
        os.makedirs(args.out, exist_ok=True)
        for keep, files, output in pending:
            save_run(keep, files, output)
        write_outputs(args.out, rows, class_rows)
    except OSError as error:
        print(f"ERROR: cannot write results to {args.out}: {error}", file=sys.stderr)
        return 1
    print(f"Wrote results to {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
