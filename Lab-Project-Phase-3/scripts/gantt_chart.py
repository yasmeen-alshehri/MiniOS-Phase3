#!/usr/bin/env python3
"""Draw a Gantt chart (PNG) from a MiniOS scheduling trace.

The trace is written by the simulator to DATA_DIR/trace.csv with the header
    experiment,pid,start,end,remaining,state
and one row per CPU slice. Each `reset` starts a new experiment whose virtual
clock restarts at 0, so one chart shows one experiment (the last by default).

The trace file is opened read-only and is never modified.

Usage examples:
    python3 scripts/gantt_chart.py
    python3 scripts/gantt_chart.py data/trace.csv -o docs/gantt.png
    python3 scripts/gantt_chart.py data/trace.csv --experiment 2 --metrics data/metrics.csv

Exit status: 0 on success, 1 for an invalid/empty trace or output error,
2 for invalid command-line usage.
"""

import argparse
import csv
import os
import sys

EXPECTED_HEADER = ["experiment", "pid", "start", "end", "remaining", "state"]
VALID_STATES = {"READY", "FINISHED"}  # states a slice can end in

# One series (CPU time), so one hue; rows carry task identity.
SLICE_COLOR = "#2a78d6"
SURFACE = "#ffffff"
TEXT_PRIMARY = "#1f1f1e"
TEXT_SECONDARY = "#5c5b55"
GRID = "#d9d8d2"

LABEL_SLICE_LIMIT = 60   # above this many slices, omit per-slice ticks and gaps
SHORT_SPAN_TICKS = 30    # spans up to this length label every slice boundary
MERGE_SLICE_LIMIT = 2000  # above this many slices, draw contiguous runs as one bar


class TraceError(Exception):
    """Raised for a missing, empty, or malformed trace file."""


def parse_int(text, field, line):
    try:
        return int(text)
    except (TypeError, ValueError):
        raise TraceError(f"line {line}: field '{field}' is not an integer: {text!r}")


def read_trace(path):
    """Return {experiment: [slice dict, ...]} after validating every row."""
    if not os.path.isfile(path):
        raise TraceError(f"trace file not found: {path}")
    experiments = {}
    with open(path, newline="", encoding="utf-8") as stream:
        reader = csv.reader(stream)
        header = next(reader, None)
        if header is None:
            raise TraceError("trace file is empty (no header).")
        if [h.strip() for h in header] != EXPECTED_HEADER:
            raise TraceError(f"unexpected header {header}; expected {EXPECTED_HEADER}")
        for row in reader:
            line = reader.line_num
            if not row or all(not cell.strip() for cell in row):
                continue
            if len(row) != len(EXPECTED_HEADER):
                raise TraceError(f"line {line}: expected 6 fields, found {len(row)}")
            exp = parse_int(row[0], "experiment", line)
            pid = parse_int(row[1], "pid", line)
            start = parse_int(row[2], "start", line)
            end = parse_int(row[3], "end", line)
            remaining = parse_int(row[4], "remaining", line)
            state = row[5].strip()
            if exp < 1 or pid < 1 or start < 0 or remaining < 0:
                raise TraceError(f"line {line}: negative or zero identifier/time")
            if end <= start:
                raise TraceError(f"line {line}: slice end {end} is not after start {start}")
            if state not in VALID_STATES:
                raise TraceError(f"line {line}: unknown state {state!r}")
            if (state == "FINISHED") != (remaining == 0):
                raise TraceError(f"line {line}: state {state} disagrees with remaining={remaining}")
            experiments.setdefault(exp, []).append(
                {"pid": pid, "start": start, "end": end, "remaining": remaining, "state": state})
    if not experiments:
        raise TraceError("trace contains a header but no slices; run the scheduler first.")
    # One virtual CPU: slices of one experiment must not overlap.
    for exp, slices in experiments.items():
        ordered = sorted(slices, key=lambda s: s["start"])
        for previous, current in zip(ordered, ordered[1:]):
            if current["start"] < previous["end"]:
                raise TraceError(f"experiment {exp}: overlapping slices at tick {current['start']}")
    return experiments


def read_names(path):
    """Optional PID->name map from an exported metrics.csv (finished tasks only)."""
    names = {}
    with open(path, newline="", encoding="utf-8") as stream:
        for row in csv.DictReader(stream):
            try:
                names[int(row["pid"])] = row["name"]
            except (KeyError, TypeError, ValueError):
                raise TraceError(f"{path}: not a MiniOS metrics.csv file")
    return names


def merge_contiguous(slices):
    """Join back-to-back slices of the same PID (used only for very large traces)."""
    merged = []
    for s in sorted(slices, key=lambda s: s["start"]):
        if merged and merged[-1]["pid"] == s["pid"] and merged[-1]["end"] == s["start"]:
            merged[-1] = dict(merged[-1], end=s["end"], remaining=s["remaining"], state=s["state"])
        else:
            merged.append(dict(s))
    return merged


def draw(slices, experiment, output, title, names):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D
    from matplotlib.patches import Patch
    from matplotlib.ticker import FuncFormatter, MaxNLocator

    slice_count = len(slices)
    if slice_count > MERGE_SLICE_LIMIT:
        slices = merge_contiguous(slices)
    pids = sorted({s["pid"] for s in slices})
    row = {pid: index for index, pid in enumerate(pids)}
    makespan_end = max(s["end"] for s in slices)
    first_start = min(s["start"] for s in slices)

    width = 11 if makespan_end <= 60 else 14
    height = max(2.6, 0.45 * len(pids) + 1.6)
    fig, ax = plt.subplots(figsize=(width, height), dpi=150)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)

    bar_height = 0.6
    # White edge = surface gap between back-to-back slices; on dense charts a
    # slice is narrower than the gap itself, so the edge would hide it.
    edge_width = 1.0 if slice_count <= LABEL_SLICE_LIMIT else 0.0
    for s in slices:
        y = row[s["pid"]]
        ax.broken_barh([(s["start"], s["end"] - s["start"])], (y - bar_height / 2, bar_height),
                       facecolors=SLICE_COLOR, edgecolors=SURFACE, linewidth=edge_width)
        if s["state"] == "FINISHED":
            ax.plot(s["end"], y - bar_height / 2 - 0.08, marker="v", markersize=8,
                    color=TEXT_PRIMARY, linestyle="none", clip_on=False)

    labels = []
    for pid in pids:
        labels.append(f"P{pid} {names[pid]}" if pid in names else f"P{pid}")
    ax.set_yticks(range(len(pids)))
    ax.set_yticklabels(labels, color=TEXT_PRIMARY)
    ax.invert_yaxis()  # P1 at the top, matching the tasks table
    ax.set_ylim(len(pids) - 0.4, -0.75)

    ax.set_xlim(min(0, first_start), makespan_end)
    boundaries = sorted({0, makespan_end} | {s["start"] for s in slices} | {s["end"] for s in slices})
    if slice_count <= LABEL_SLICE_LIMIT and makespan_end - min(0, first_start) <= SHORT_SPAN_TICKS:
        # Short traces: a labelled tick at every slice boundary shows each dispatch exactly.
        ax.set_xticks(boundaries)
    else:
        ax.xaxis.set_major_locator(MaxNLocator(integer=True, nbins=20 if makespan_end < 10000 else 10))
        if slice_count <= LABEL_SLICE_LIMIT:
            # Unlabelled gridlines still mark every slice boundary without label collisions.
            ax.set_xticks(boundaries, minor=True)
            ax.tick_params(axis="x", which="minor", length=0)
    # Plain integers with thousands separators; never scientific offsets.
    ax.xaxis.set_major_formatter(FuncFormatter(lambda value, _pos: f"{int(value):,}"))
    ax.set_xlabel("Virtual time (ticks)", color=TEXT_SECONDARY)
    ax.set_ylabel("Task (PID)", color=TEXT_SECONDARY)
    ax.tick_params(colors=TEXT_SECONDARY, labelsize=9)
    ax.grid(axis="x", which="both", color=GRID, linewidth=0.6)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(GRID)

    heading = title or f"MiniOS round-robin schedule - experiment {experiment}"
    plural = "" if len(pids) == 1 else "s"
    note = f"{slice_count:,} slices, {len(pids)} task{plural}, ticks [{first_start:,}, {makespan_end:,})"
    if slice_count > MERGE_SLICE_LIMIT:
        note += "; contiguous slices of one task drawn as one bar"
    ax.set_title(f"{heading}\n{note}", loc="left", fontsize=11, color=TEXT_PRIMARY)
    ax.legend(handles=[Patch(facecolor=SLICE_COLOR, label="CPU slice (task RUNNING)"),
                       Line2D([], [], marker="v", color=TEXT_PRIMARY, linestyle="none",
                              markersize=8, label="Task FINISHED")],
              loc="lower right", bbox_to_anchor=(1.0, 1.0), ncol=2, frameon=False, fontsize=9)
    fig.tight_layout()
    try:
        fig.savefig(output, facecolor=SURFACE)
    finally:
        plt.close(fig)


def main(argv=None):
    parser = argparse.ArgumentParser(description="Draw a Gantt chart PNG from a MiniOS trace.csv.")
    parser.add_argument("trace", nargs="?", default=os.path.join("data", "trace.csv"),
                        help="path to trace.csv (default: data/trace.csv)")
    parser.add_argument("-o", "--output", help="PNG path (default: gantt.png next to the trace)")
    parser.add_argument("-e", "--experiment", type=int,
                        help="experiment number to draw (default: the last one in the trace)")
    parser.add_argument("-m", "--metrics", help="optional metrics.csv used to label PIDs with names")
    parser.add_argument("-t", "--title", help="chart title")
    args = parser.parse_args(argv)

    output = args.output or os.path.join(os.path.dirname(os.path.abspath(args.trace)), "gantt.png")
    if not output.lower().endswith(".png"):
        parser.error("output file must end in .png")
    if os.path.abspath(output) == os.path.abspath(args.trace):
        parser.error("output must not overwrite the trace file")
    try:
        experiments = read_trace(args.trace)
        experiment = args.experiment if args.experiment is not None else max(experiments)
        if experiment not in experiments:
            raise TraceError(f"experiment {experiment} not in trace; available: {sorted(experiments)}")
        names = read_names(args.metrics) if args.metrics else {}
        try:
            import matplotlib  # noqa: F401
        except ImportError:
            raise TraceError("matplotlib is required: python3 -m pip install -r scripts/requirements.txt")
        draw(experiments[experiment], experiment, output, args.title, names)
    except TraceError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    except (OSError, UnicodeError, csv.Error) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 1
    print(f"Wrote {output} (experiment {experiment}, {len(experiments[experiment])} slices)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
