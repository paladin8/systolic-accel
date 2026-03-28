"""Analyze synthesis results — print tables and generate PNG charts."""

import json
import sys
from pathlib import Path

import matplotlib
matplotlib.use("Agg")  # non-interactive backend
import matplotlib.pyplot as plt
import numpy as np


def load_results() -> dict:
    path = Path("synth/results.json")
    if not path.exists():
        print(f"Error: {path} not found. Run synth/run_sweep.py first.")
        sys.exit(1)
    return json.loads(path.read_text())


def print_table(headers: list[str], rows: list[list], title: str):
    """Print a simple ASCII table."""
    print(f"\n{title}")
    widths = [max(len(str(h)), max(len(str(r[i])) for r in rows))
              for i, h in enumerate(headers)]
    fmt = " | ".join(f"{{:<{w}}}" for w in widths)
    sep = "-+-".join("-" * w for w in widths)
    print(fmt.format(*headers))
    print(sep)
    for row in rows:
        print(fmt.format(*[str(v) for v in row]))


def fmt_fmax(r: dict) -> str:
    fmax = r.get("fmax_mhz")
    return f"{fmax:.1f}" if fmax is not None else "N/A"


def analyze_datawidth(exp: dict):
    """Experiment 1: data width vs area."""
    results = exp["results"]
    rows = []
    labels, lut_counts = [], []
    for r in results:
        dw = r["config"]["DATA_WIDTH"]
        luts = r.get("SB_LUT4", 0)
        ffs = r.get("SB_DFFER", 0)
        total = r.get("total_cells", 0)
        rows.append([f"INT{dw}", luts, ffs, total, fmt_fmax(r)])
        labels.append(f"INT{dw}")
        lut_counts.append(luts)

    print_table(["Data Width", "LUTs", "FFs", "Total Cells", "Fmax (MHz)"], rows,
                "Experiment 1: Data Width (4×4 array, depth 2)")

    if len(lut_counts) >= 2:
        ratio = lut_counts[1] / lut_counts[0] if lut_counts[0] else 0
        print(f"\nINT{results[0]['config']['DATA_WIDTH']} → "
              f"INT{results[1]['config']['DATA_WIDTH']} LUT ratio: {ratio:.1f}x")

    fig, ax = plt.subplots(figsize=(6, 4))
    x = np.arange(len(labels))
    ax.bar(x, lut_counts, color="#4C72B0")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("LUT Count")
    ax.set_title("Area vs Data Width (4×4 array, depth 2)")
    for i, v in enumerate(lut_counts):
        ax.text(i, v + max(lut_counts) * 0.02, str(v), ha="center", fontsize=9)
    fig.tight_layout()
    fig.savefig("docs/area_vs_datawidth.png", dpi=150)
    plt.close(fig)
    print("  → docs/area_vs_datawidth.png")


def analyze_arraysize(exp: dict):
    """Experiment 2: array size vs area."""
    results = exp["results"]
    rows = []
    labels, lut_counts = [], []
    for r in results:
        n = r["config"]["ROWS"]
        luts = r.get("SB_LUT4", 0)
        ffs = r.get("SB_DFFER", 0)
        total = r.get("total_cells", 0)
        rows.append([f"{n}×{n}", luts, ffs, total, fmt_fmax(r)])
        labels.append(f"{n}×{n}")
        lut_counts.append(luts)

    print_table(["Array Size", "LUTs", "FFs", "Total Cells", "Fmax (MHz)"], rows,
                "Experiment 2: Array Size (INT8, depth 2)")

    if len(lut_counts) >= 2:
        ratio = lut_counts[1] / lut_counts[0] if lut_counts[0] else 0
        print(f"\n{labels[0]} → {labels[1]} LUT ratio: {ratio:.1f}x")

    fig, ax = plt.subplots(figsize=(6, 4))
    x = np.arange(len(labels))
    ax.bar(x, lut_counts, color="#55A868")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("LUT Count")
    ax.set_title("Area vs Array Size (INT8, depth 2)")
    for i, v in enumerate(lut_counts):
        ax.text(i, v + max(lut_counts) * 0.02, str(v), ha="center", fontsize=9)
    fig.tight_layout()
    fig.savefig("docs/area_vs_arraysize.png", dpi=150)
    plt.close(fig)
    print("  → docs/area_vs_arraysize.png")


def analyze_depth(exp: dict):
    """Experiment 3: pipeline depth vs area (synthesis only, no timing)."""
    results = exp["results"]
    rows = []
    depths, lut_counts = [], []
    for r in results:
        d = r["config"]["PIPELINE_DEPTH"]
        luts = r.get("SB_LUT4", 0)
        ffs = r.get("SB_DFFER", 0)
        total = r.get("total_cells", 0)
        rows.append([f"Depth {d}", luts, ffs, total])
        depths.append(d)
        lut_counts.append(luts)

    print_table(["Pipeline Depth", "LUTs", "FFs", "Total Cells"], rows,
                "Experiment 3: Pipeline Depth — Area (4×4 INT16)")

    fig, ax = plt.subplots(figsize=(6, 4))
    x = np.arange(len(depths))
    labels = [f"Depth {d}" for d in depths]
    ax.bar(x, lut_counts, color="#C44E52")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("LUT Count")
    ax.set_title("Area vs Pipeline Depth (4×4 INT16)")
    for i, v in enumerate(lut_counts):
        ax.text(i, v + max(lut_counts) * 0.02, str(v), ha="center", fontsize=9)
    fig.tight_layout()
    fig.savefig("docs/area_vs_depth.png", dpi=150)
    plt.close(fig)
    print("  → docs/area_vs_depth.png")


def analyze_depth_pnr(exp: dict):
    """Experiment 4: pipeline depth with P&R timing → real throughput."""
    results = exp["results"]
    N = results[0]["config"]["ROWS"]

    # Collect data
    depths, lut_counts, fmax_values = [], [], []
    for r in results:
        d = r["config"]["PIPELINE_DEPTH"]
        depths.append(d)
        lut_counts.append(r.get("SB_LUT4", 0))
        fmax_values.append(r.get("fmax_mhz"))

    # Compute throughput: N^2 * Fmax / compute_cycles
    compute_cycles = [(2 * N - 1) + (d - 1) for d in depths]
    throughputs = []
    for i, fmax in enumerate(fmax_values):
        if fmax is not None:
            throughputs.append(N * N * fmax / compute_cycles[i])
        else:
            throughputs.append(None)

    # Table
    rows = []
    for i, d in enumerate(depths):
        fmax_str = f"{fmax_values[i]:.1f}" if fmax_values[i] else "N/A"
        tp_str = f"{throughputs[i]:.1f}" if throughputs[i] else "N/A"
        rows.append([f"Depth {d}", lut_counts[i], compute_cycles[i],
                     fmax_str, tp_str])

    print_table(
        ["Pipeline Depth", "LUTs", "Compute Cycles", "Fmax (MHz)", "Throughput (M MACs/s)"],
        rows,
        "Experiment 4: Pipeline Depth — Timing & Throughput (4×4 INT8, iCE40 HX8K)"
    )
    print(f"  Throughput = N² × Fmax / compute_cycles, N={N}")
    print(f"  Note: depth 3 is synthesis-only at the array level (breaks column chain)")

    labels = [f"Depth {d}" for d in depths]
    x = np.arange(len(depths))

    # Fmax chart
    fmax_plot = [f if f is not None else 0 for f in fmax_values]
    fig, ax = plt.subplots(figsize=(6, 4))
    ax.bar(x, fmax_plot, color="#DD8452")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Fmax (MHz)")
    ax.set_title("Max Frequency vs Pipeline Depth (4×4 INT8, iCE40 HX8K)")
    for i, v in enumerate(fmax_plot):
        if v > 0:
            ax.text(i, v + max(fmax_plot) * 0.02, f"{v:.1f}", ha="center", fontsize=9)
    fig.tight_layout()
    fig.savefig("docs/fmax_vs_depth.png", dpi=150)
    plt.close(fig)
    print("  → docs/fmax_vs_depth.png")

    # Throughput chart
    tp_plot = [t if t is not None else 0 for t in throughputs]
    fig, ax = plt.subplots(figsize=(6, 4))
    ax.bar(x, tp_plot, color="#8172B2")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("Throughput (M MACs/s)")
    ax.set_title("Throughput vs Pipeline Depth (4×4 INT8, iCE40 HX8K)")
    for i, v in enumerate(tp_plot):
        if v > 0:
            ax.text(i, v + max(tp_plot) * 0.02, f"{v:.1f}", ha="center", fontsize=9)
    fig.tight_layout()
    fig.savefig("docs/throughput_vs_depth.png", dpi=150)
    plt.close(fig)
    print("  → docs/throughput_vs_depth.png")


def main():
    data = load_results()

    Path("docs").mkdir(exist_ok=True)

    if "1_datawidth" in data:
        analyze_datawidth(data["1_datawidth"])
    if "2_arraysize" in data:
        analyze_arraysize(data["2_arraysize"])
    if "3_depth" in data:
        analyze_depth(data["3_depth"])
    if "4_depth_pnr" in data:
        analyze_depth_pnr(data["4_depth_pnr"])

    print("\nDone.")


if __name__ == "__main__":
    main()
