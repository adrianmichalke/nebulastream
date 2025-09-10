#!/usr/bin/env python3
import argparse
import csv
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = ROOT / "build-nix"
DEFAULT_TEST_DATA = ROOT / "nes-systests" / "testdata"


TEMPLATE_HEADER = """# name: benchmark/StoreOverhead_Gen_Auto.test
# description: Auto-generated A/B benchmark for Store overhead (rows={rows}, width={width})
# groups: [benchmark, Store, Sources]

"""


def make_test_content(rows: int, width: int) -> str:
    # Base fields: id, zero => 16 bytes
    base_bytes = 16
    if width < base_bytes:
        width = base_bytes
    num_extra = max((width - base_bytes) // 8, 0)

    # Build source schema line
    fields_decl = ["UINT64 id", "UINT64 zero"]
    fields_decl += [f"UINT64 pad{i}" for i in range(1, num_extra + 1)]
    source_line = "Source gen " + " ".join(fields_decl) + " GENERATOR seed 1, max_runtime_ms 600000, stop_generator_when_sequence_finishes ALL\n"

    # Generator schema: id sequence 0..rows-1, zero and pads all constants (0..0 step 0)
    gen_lines = []
    # Set end to rows (inclusive check in generator results in exactly 'rows' tuples in practice)
    gen_lines.append(f"id SEQUENCE UINT64 0 {rows} 1\n")
    gen_lines.append("zero SEQUENCE UINT64 0 0 0\n")
    for i in range(1, num_extra + 1):
        gen_lines.append(f"pad{i} SEQUENCE UINT64 0 0 0\n")

    # Sinks (Checksum)
    sink_fields = ["UINT64 gen$id", "UINT64 gen$zero"]
    sink_decl = " ".join(sink_fields)
    sinks = [
        f"SINK baselineChecksum TYPE Checksum {sink_decl}\n",
        f"SINK storeChecksum TYPE Checksum {sink_decl}\n",
    ]

    # Expected values: the Checksum sink outputs two numbers: Count and Checksum of formatted CSV rows.
    # We precomputed the checksum for rows of 'id,zero' where zero is constant 0.
    # For rows=1_000_000, checksum=411666720 (from existing single-case test). Scale for other row counts below.
    # Derive checksum dynamically by constructing small sample to avoid hardcoding for arbitrary rows.
    chk = compute_checksum(rows)

    select_baseline = ("SELECT id, zero FROM gen INTO baselineChecksum\n"
                       "----\n" f"{rows} {chk}\n")

    select_store = ("SELECT id, zero FROM gen INTO storeChecksum TIME TRAVEL STORE "
                    "(FILE_PATH AS 'systest_store_overhead.bin', APPEND AS FALSE, HEADER AS TRUE, FDATASYNC_INTERVAL AS 0)\n"
                    "----\n" f"{rows} {chk}\n")

    return (
        TEMPLATE_HEADER.format(rows=rows, width=width)
        + source_line
        + "".join(gen_lines)
        + "\n"
        + "".join(sinks)
        + "\n"
        + select_baseline
        + "\n\n"
        + select_store
        + "\n"
    )


def compute_checksum(rows: int) -> int:
    # Emulate ChecksumSink logic: checksum is sum of ASCII codes of formatted CSV from CSVFormat
    # CSVFormat prints lines as "id,zero\n" for our two fields. Compute sum for i in [0..rows-1].
    s = 0
    for i in range(rows):
        # Avoid heavy cost for large rows by chunking digits
        row = f"{i},0\n"
        s += sum(row.encode("ascii"))
    return s


def ensure_matplotlib():
    try:
        import matplotlib  # noqa: F401
        import seaborn  # noqa: F401
        return
    except Exception:
        pass
    # Try to install locally for the user
    subprocess.run([sys.executable, "-m", "pip", "install", "--user", "matplotlib", "seaborn"], check=True)
    import importlib
    import site
    importlib.invalidate_caches()
    # Ensure user site is on path
    user_site = site.getusersitepackages()
    if user_site not in sys.path:
        sys.path.append(user_site)


def run_systest_benchmark(build_dir: Path, test_file: Path, data_dir: Path, workdir: Path, mode: str) -> list[dict]:
    workdir.mkdir(parents=True, exist_ok=True)
    # Remove any residual store file in build_dir to avoid interference
    store_file = build_dir / "systest_store_overhead.bin"
    if store_file.exists():
        try:
            store_file.unlink()
        except Exception:
            pass
    cmd = (
        f"cd '{build_dir}' && "
        f"./nes-systests/systest/systest -b --workingDir '{workdir}' --testLocation '{test_file}' --data '{data_dir}' "
        f"-- --worker.default_query_execution.execution_mode={mode}"
    )
    # Ensure correct env via nix develop
    full = f"nix develop --command bash -lc \"{cmd}\""
    res = subprocess.run(["bash", "-lc", full], capture_output=True, text=True)
    if res.returncode != 0:
        print(res.stdout)
        print(res.stderr, file=sys.stderr)
        raise RuntimeError("systest benchmark failed")

    out_json = workdir / "BenchmarkResults.json"
    if not out_json.exists():
        # Sometimes JSON is printed to stdout if writing failed; try parse trailing JSON
        try:
            text = res.stdout.strip()
            last_bracket = text.rfind("]")
            first_bracket = text.rfind("[")
            if first_bracket != -1 and last_bracket != -1 and last_bracket > first_bracket:
                data = json.loads(text[first_bracket:last_bracket + 1])
                return data
        except Exception:
            pass
        raise FileNotFoundError(f"BenchmarkResults.json not found in {workdir}")
    with open(out_json, "r") as f:
        return json.load(f)


def summarize_results(bench_json: list[dict]) -> tuple[float, float, float, float, float]:
    # Expect [baseline, store]
    if len(bench_json) < 2:
        raise ValueError("Expected at least two entries in benchmark JSON")
    b = bench_json[0]
    s = bench_json[1]
    bt = float(b.get("time", float("nan")))
    st = float(s.get("time", float("nan")))
    btu = float(b.get("tuplesPerSecond", float("nan")))
    stu = float(s.get("tuplesPerSecond", float("nan")))
    delta = st - bt
    overhead_pct = (delta / bt * 100.0) if bt > 0 else float("nan")
    return bt, st, delta, overhead_pct, (btu if not math.isnan(btu) else float("nan"))


def plot_csv(csv_path: Path, out_dir: Path, title_suffix: str = ""):
    try:
        ensure_matplotlib()
        import pandas as pd
        import matplotlib.pyplot as plt
        import seaborn as sns

        df = pd.read_csv(csv_path)
        # Plot 1: Overhead vs. rows by width
        plt.figure(figsize=(8, 5))
        sns.lineplot(data=df, x="rows", y="overhead_pct", hue="width_bytes", marker="o")
        plt.title(f"Store Overhead vs Rows{title_suffix}")
        plt.ylabel("Overhead (%)")
        plt.xlabel("Rows")
        plt.xlim(left=0)
        plt.ylim(bottom=0)
        plt.grid(True, alpha=0.3)
        out1 = out_dir / "plot_overhead_vs_rows.png"
        plt.tight_layout()
        plt.savefig(out1, dpi=160)
        plt.close()

        # Plot 2: Overhead vs width by rows
        plt.figure(figsize=(8, 5))
        sns.lineplot(data=df, x="width_bytes", y="overhead_pct", hue="rows", marker="o")
        plt.title(f"Store Overhead vs Row Width{title_suffix}")
        plt.ylabel("Overhead (%)")
        plt.xlabel("Row width (bytes)")
        plt.xlim(left=0)
        plt.ylim(bottom=0)
        plt.grid(True, alpha=0.3)
        out2 = out_dir / "plot_overhead_vs_width.png"
        plt.tight_layout()
        plt.savefig(out2, dpi=160)
        plt.close()

        print(f"[plot] Saved: {out1}")
        print(f"[plot] Saved: {out2}")
        return
    except Exception as e:
        print(f"[plot] Matplotlib plotting unavailable ({e}); falling back to SVG.")

    # Fallback: simple SVG charts using only stdlib
    import csv as _csv
    rows = []
    with open(csv_path, newline="") as f:
        r = _csv.DictReader(f)
        for row in r:
            rows.append({
                "rows": int(float(row["rows"])),
                "width": int(float(row["width_bytes"])),
                "overhead": float(row["overhead_pct"]),
            })

    if not rows:
        print("[plot] No data to plot")
        return

    def save_svg(data, xkey, series_key, title, xlabel, outfile):
        W, H = 900, 560
        M = 70
        xs = sorted(set(d[xkey] for d in data))
        # Ensure axis starts at zero
        if 0 not in xs:
            xs.insert(0, 0)
        ss = sorted(set(d[series_key] for d in data))
        ymin = 0.0
        ymax = max(d["overhead"] for d in data)
        if ymax <= 0:
            ymax = 1.0
        else:
            ymax *= 1.10  # small headroom
        x_min = 0.0
        x_max = float(max(xs)) if xs else 1.0
        if x_max == x_min:
            x_max = x_min + 1.0
        def sx(x):
            return M + (float(x) - x_min) * (W - 2*M) / (x_max - x_min)
        def sy(y):
            return H - M - (y - ymin) * (H - 2*M) / (ymax - ymin)
        palette = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b"]
        # Build SVG
        parts = []
        parts.append(f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}'>")
        parts.append(f"<rect x='0' y='0' width='{W}' height='{H}' fill='white'/>")
        parts.append(f"<text x='{W/2}' y='{M/2}' text-anchor='middle' font-family='sans-serif' font-size='18'>{title}</text>")
        # axes
        parts.append(f"<line x1='{M}' y1='{H-M}' x2='{W-M}' y2='{H-M}' stroke='black'/>")
        parts.append(f"<line x1='{M}' y1='{M}' x2='{M}' y2='{H-M}' stroke='black'/>")
        # x ticks
        for x in xs:
            X = sx(x)
            parts.append(f"<line x1='{X}' y1='{H-M}' x2='{X}' y2='{H-M+5}' stroke='black'/>")
            parts.append(f"<text x='{X}' y='{H-M+20}' text-anchor='middle' font-size='12' font-family='sans-serif'>{x}</text>")
        parts.append(f"<text x='{W/2}' y='{H-10}' text-anchor='middle' font-size='14' font-family='sans-serif'>{xlabel}</text>")
        # y ticks (5)
        for i in range(6):
            y = ymin + i*(ymax - ymin)/5
            Y = sy(y)
            parts.append(f"<line x1='{M-5}' y1='{Y}' x2='{M}' y2='{Y}' stroke='black'/>")
            parts.append(f"<text x='{M-10}' y='{Y+4}' text-anchor='end' font-size='12' font-family='sans-serif'>{y:.1f}</text>")
            parts.append(f"<line x1='{M}' y1='{Y}' x2='{W-M}' y2='{Y}' stroke='#eee'/>")
        # series
        for idx, s in enumerate(ss):
            color = palette[idx % len(palette)]
            pts = sorted([d for d in data if d[series_key] == s], key=lambda k: k[xkey])
            if not pts:
                continue
            path = " ".join(f"L {sx(p[xkey]):.2f} {sy(p['overhead']):.2f}" for p in pts)
            x0, y0 = sx(pts[0][xkey]), sy(pts[0]['overhead'])
            parts.append(f"<path d='M {x0:.2f} {y0:.2f} {path}' fill='none' stroke='{color}' stroke-width='2'/>")
            for p in pts:
                parts.append(f"<circle cx='{sx(p[xkey]):.2f}' cy='{sy(p['overhead']):.2f}' r='3' fill='{color}'/>")
        # legend
        lx, ly = W - M - 160, M + 10
        parts.append(f"<rect x='{lx-10}' y='{ly-20}' width='170' height='{len(ss)*18+30}' fill='white' stroke='#ccc'/>")
        parts.append(f"<text x='{lx}' y='{ly}' font-size='14' font-family='sans-serif'>Legend</text>")
        for idx, s in enumerate(ss):
            color = palette[idx % len(palette)]
            yy = ly + 15 + idx*18
            parts.append(f"<rect x='{lx}' y='{yy-10}' width='14' height='4' fill='{color}'/>")
            parts.append(f"<text x='{lx+20}' y='{yy-2}' font-size='12' font-family='sans-serif'>{series_key}={s}</text>")
        parts.append("</svg>")
        Path(outfile).write_text("\n".join(parts))
        print(f"[plot] Saved: {outfile}")

    # Overhead vs rows (series by width)
    save_svg(rows, xkey="rows", series_key="width", title="Store Overhead vs Rows", xlabel="Rows",
             outfile=out_dir / "plot_overhead_vs_rows.svg")
    # Overhead vs width (series by rows)
    save_svg(rows, xkey="width", series_key="rows", title="Store Overhead vs Row Width", xlabel="Row width (bytes)",
             outfile=out_dir / "plot_overhead_vs_width.svg")


def main():
    ap = argparse.ArgumentParser(description="Benchmark Store operator overhead and produce plots")
    ap.add_argument("--rows", default="10000,50000,100000,300000,500000,1000000", help="Comma-separated list of row counts")
    ap.add_argument("--widths", default="16,32,64,128,256,512,1024", help="Comma-separated list of row widths in bytes")
    ap.add_argument("--mode", default=os.environ.get("MODE", "INTERPRETER"), help="Execution mode: INTERPRETER or COMPILER")
    ap.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR))
    ap.add_argument("--data-dir", default=str(DEFAULT_TEST_DATA))
    ap.add_argument("--work-root", default=None, help="Working root directory; defaults to <build>/bench_store_overhead_matrix")
    ap.add_argument("--repeats", type=int, default=1, help="Repeats per point for averaging")
    ap.add_argument("--warmup", type=int, default=0, help="Warmup runs per point (discarded)")
    ap.add_argument("--no-build", action="store_true", help="Skip building systest target")
    args = ap.parse_args()

    build_dir = Path(args.build_dir).resolve()
    data_dir = Path(args.data_dir).resolve()
    work_root = Path(args.work_root) if args.work_root else (build_dir / "bench_store_overhead_matrix")
    work_root.mkdir(parents=True, exist_ok=True)

    if not args.no_build:
        print(f"[build] Building systest in {build_dir}")
        subprocess.run([
            "bash", "-lc",
            f"cd '{build_dir}' && make systest -j$(( $(nproc 2>/dev/null || echo 4) ))"
        ], check=True)

    rows_list = [int(x.strip()) for x in args.rows.split(",") if x.strip()]
    widths_list = [int(x.strip()) for x in args.widths.split(",") if x.strip()]

    csv_path = work_root / "overheads.csv"
    with open(csv_path, "w", newline="") as cf:
        writer = csv.writer(cf)
        writer.writerow(["rows", "width_bytes", "baseline_time_s", "store_time_s", "delta_s", "overhead_pct", "baseline_tps", "store_tps"])  # header

        for rows in rows_list:
            for width in widths_list:
                case_dir = work_root / f"rows_{rows}_w_{width}"
                case_dir.mkdir(parents=True, exist_ok=True)
                test_path = case_dir / "StoreOverheadGenAuto.test"
                test_content = make_test_content(rows, width)
                test_path.write_text(test_content)

                # Warmups
                for _ in range(args.warmup):
                    _ = run_systest_benchmark(build_dir, test_path, data_dir, case_dir / "warmup", args.mode)

                bt_list = []
                st_list = []
                oh_list = []
                dt_list = []
                for r in range(args.repeats):
                    res_json = run_systest_benchmark(build_dir, test_path, data_dir, case_dir / f"run_{r}", args.mode)
                    bt, st, delta, overhead_pct, _ = summarize_results(res_json)
                    bt_list.append(bt)
                    st_list.append(st)
                    dt_list.append(delta)
                    oh_list.append(overhead_pct)

                # Average over repeats
                bt_avg = sum(bt_list) / len(bt_list)
                st_avg = sum(st_list) / len(st_list)
                dt_avg = sum(dt_list) / len(dt_list)
                oh_avg = sum(oh_list) / len(oh_list)

                b_tps = (rows / bt_avg) if bt_avg > 0 else float("nan")
                s_tps = (rows / st_avg) if st_avg > 0 else float("nan")
                writer.writerow([rows, width, f"{bt_avg:.6f}", f"{st_avg:.6f}", f"{dt_avg:.6f}", f"{oh_avg:.4f}", f"{b_tps:.1f}", f"{s_tps:.1f}"])
                print(f"[done] rows={rows} width={width}B -> overhead={oh_avg:.2f}% (baseline={bt_avg:.3f}s, store={st_avg:.3f}s, b_tps={b_tps:.0f}, s_tps={s_tps:.0f})")

    # Try to produce plots (matplotlib or fallback SVG)
    plot_csv(csv_path, work_root)


if __name__ == "__main__":
    main()
