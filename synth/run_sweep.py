"""Experiment driver — invokes Yosys across parameter configurations."""

import json
import re
import subprocess
import sys
from pathlib import Path

# ACC_WIDTH = 2 * DATA_WIDTH for fair comparison across widths
EXPERIMENTS = {
    "1_datawidth": {
        "description": "Data width sweep (4x4 array, depth 2)",
        "configs": [
            {"ROWS": 4, "COLS": 4, "DATA_WIDTH": 8,  "ACC_WIDTH": 16, "PIPELINE_DEPTH": 2},
            {"ROWS": 4, "COLS": 4, "DATA_WIDTH": 16, "ACC_WIDTH": 32, "PIPELINE_DEPTH": 2},
            {"ROWS": 4, "COLS": 4, "DATA_WIDTH": 32, "ACC_WIDTH": 64, "PIPELINE_DEPTH": 2},
        ],
    },
    "2_arraysize": {
        "description": "Array size sweep (INT8, depth 2)",
        "configs": [
            {"ROWS": 2,  "COLS": 2,  "DATA_WIDTH": 8, "ACC_WIDTH": 16, "PIPELINE_DEPTH": 2},
            {"ROWS": 4,  "COLS": 4,  "DATA_WIDTH": 8, "ACC_WIDTH": 16, "PIPELINE_DEPTH": 2},
            {"ROWS": 8,  "COLS": 8,  "DATA_WIDTH": 8, "ACC_WIDTH": 16, "PIPELINE_DEPTH": 2},
            {"ROWS": 16, "COLS": 16, "DATA_WIDTH": 8, "ACC_WIDTH": 16, "PIPELINE_DEPTH": 2},
        ],
    },
    "3_depth": {
        "description": "Pipeline depth sweep (4x4 INT16)",
        "configs": [
            {"ROWS": 4, "COLS": 4, "DATA_WIDTH": 16, "ACC_WIDTH": 32, "PIPELINE_DEPTH": 1},
            {"ROWS": 4, "COLS": 4, "DATA_WIDTH": 16, "ACC_WIDTH": 32, "PIPELINE_DEPTH": 2},
            {"ROWS": 4, "COLS": 4, "DATA_WIDTH": 16, "ACC_WIDTH": 32, "PIPELINE_DEPTH": 3},
        ],
    },
}


def config_name(cfg: dict) -> str:
    return f"r{cfg['ROWS']}c{cfg['COLS']}_dw{cfg['DATA_WIDTH']}_aw{cfg['ACC_WIDTH']}_pd{cfg['PIPELINE_DEPTH']}"


def run_yosys(cfg: dict, log_dir: Path) -> dict:
    """Run Yosys for one config and return parsed results."""
    name = config_name(cfg)
    chparam_args = " ".join(f"-set {k} {v}" for k, v in cfg.items())
    cmd = (
        f"read_verilog -sv rtl/mac_unit.sv rtl/systolic_array.sv; "
        f"chparam {chparam_args} systolic_array; "
        f"synth_ice40 -top systolic_array; "
        f"stat"
    )

    log_path = log_dir / f"{name}.log"
    print(f"  Synthesizing {name}...", end=" ", flush=True)

    result = subprocess.run(
        ["yosys", "-p", cmd],
        capture_output=True, text=True, timeout=600,
    )

    log_path.write_text(result.stdout + result.stderr)

    if result.returncode != 0:
        print("FAILED")
        return {"config": cfg, "name": name, "error": "yosys failed"}

    # Parse stat output
    stats = parse_stats(result.stdout)
    print(f"LUTs={stats.get('SB_LUT4', '?')}, FFs={stats.get('SB_DFFER', '?')}")
    return {"config": cfg, "name": name, **stats}


def parse_stats(output: str) -> dict:
    """Extract cell counts from Yosys stat output."""
    stats = {}

    # Find the last stat block
    blocks = output.split("Printing statistics.")
    if len(blocks) < 2:
        return stats
    block = blocks[-1]

    for line in block.splitlines():
        line = line.strip()
        # Match lines like "SB_LUT4    12727"
        m = re.match(r"(SB_\w+)\s+(\d+)", line)
        if m:
            stats[m.group(1)] = int(m.group(2))
        # Match "Number of cells:" line
        m = re.match(r"Number of cells:\s+(\d+)", line)
        if m:
            stats["total_cells"] = int(m.group(1))

    return stats


def main():
    log_dir = Path("synth/logs")
    log_dir.mkdir(parents=True, exist_ok=True)

    # Filter to specific experiment if requested
    experiments = EXPERIMENTS
    if len(sys.argv) > 1:
        key = sys.argv[1]
        matches = {k: v for k, v in EXPERIMENTS.items() if key in k}
        if not matches:
            print(f"Unknown experiment: {key}")
            print(f"Available: {', '.join(EXPERIMENTS.keys())}")
            sys.exit(1)
        experiments = matches

    all_results = {}
    for exp_name, exp in experiments.items():
        print(f"\n=== {exp['description']} ===")
        results = []
        for cfg in exp["configs"]:
            result = run_yosys(cfg, log_dir)
            results.append(result)
        all_results[exp_name] = {"description": exp["description"], "results": results}

    # Save results
    results_path = Path("synth/results.json")
    results_path.write_text(json.dumps(all_results, indent=2))
    print(f"\nResults saved to {results_path}")


if __name__ == "__main__":
    main()
