#!/usr/bin/env python3
import argparse
import csv
import json
import math
import os
import subprocess
import sys
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = ROOT / "build-nix"
DEFAULT_TEST_DATA = ROOT / "nes-systests" / "testdata"


TEMPLATE_HEADER = """# name: benchmark/StoreSize_Gen_Auto.test
# description: Auto-generated benchmark for Store size (rows={rows}, width={width}, backend={backend})
# groups: [benchmark, Store, Sources]

"""


def make_test_content(rows: int, width: int, store_path: Path, backend: str = "POSIX") -> str:
    # Base fields: id, zero => 16 bytes
    base_bytes = 16
    if width < base_bytes:
        width = base_bytes
    num_extra = max((width - base_bytes) // 8, 0)

    # Build source schema line
    fields_decl = ["UINT64 id", "UINT64 zero"]
    fields_decl += [f"UINT64 pad{i}" for i in range(1, num_extra + 1)]
    source_line = (
        "Source gen " + " ".join(fields_decl)
        + " GENERATOR seed 1, max_runtime_ms 600000, stop_generator_when_sequence_finishes ALL\n"
    )

    # Generator schema: id sequence 0..rows-1, zero and pads all constants (0..0 step 0)
    gen_lines = []
    gen_lines.append(f"id SEQUENCE UINT64 0 {rows-1} 1\n")
    gen_lines.append("zero SEQUENCE UINT64 0 0 0\n")
    for i in range(1, num_extra + 1):
        gen_lines.append(f"pad{i} SEQUENCE UINT64 0 0 0\n")

    # Low-overhead sink for result validation (uses only id, zero)
    sink_decl = "UINT64 gen$id UINT64 gen$zero"
    sinks = [
        f"SINK checksum TYPE Checksum {sink_decl}\n",
    ]

    # Compute expected checksum for the two-field CSV view "id,zero" with zero constant
    chk = compute_checksum(rows)
    backend_norm = backend.upper()
    # Use absolute store path to avoid ambiguity
    sp = str(store_path)
    select_store = (
        "SELECT id, zero FROM gen INTO checksum TIME TRAVEL STORE "
        "(FILE_PATH AS '" + sp + "', APPEND AS FALSE, HEADER AS TRUE, FDATASYNC_INTERVAL AS 0, ASYNC_BACKEND AS '"
        + backend_norm + "')\n"
        "----\n" f"{rows} {chk}\n"
    )

    return (
        TEMPLATE_HEADER.format(rows=rows, width=width, backend=backend_norm)
        + source_line
        + "".join(gen_lines)
        + "\n"
        + "".join(sinks)
        + "\n"
        + select_store
        + "\n"
    )


def compute_checksum(rows: int) -> int:
    s = 0
    for i in range(rows):
        row = f"{i},0\n"
        s += sum(row.encode("ascii"))
    return s


def run_systest(build_dir: Path, test_file: Path, data_dir: Path, workdir: Path, mode: str) -> bool:
    workdir.mkdir(parents=True, exist_ok=True)
    cmd = (
        f"cd '{build_dir}' && "
        f"./nes-systests/systest/systest --workingDir '{workdir}' --testLocation '{test_file}' --data '{data_dir}' "
        f"-- --worker.default_query_execution.execution_mode={mode}"
    )
    full = f"nix develop --command bash -lc \"{cmd}\""
    res = subprocess.run(["bash", "-lc", full], capture_output=True, text=True)
    if res.returncode != 0:
        # Keep going; size artifacts are still expected to exist
        sys.stderr.write("[warn] systest reported failures; continuing to collect sizes.\n")
        sys.stderr.flush()
        return False
    return True


def bytes_to_mb(x: int) -> float:
    return x / (1024.0 * 1024.0)


def plot_csv(csv_path: Path, out_dir: Path, title_suffix: str = "", name_suffix: str = ""):
    try:
        import matplotlib.pyplot as plt
        import pandas as pd
        import seaborn as sns

        df = pd.read_csv(csv_path)
        df["input_mb"] = df["input_bytes"] / (1024 * 1024)
        df["store_mb"] = df["store_bytes"] / (1024 * 1024)

        suffix = f"_{name_suffix}" if name_suffix else ""

        # Plot 1: Stored bytes vs. Input bytes (reference line y=x)
        plt.figure(figsize=(8, 6))
        sns.lineplot(data=df, x="input_mb", y="store_mb", hue="backend", style="width_bytes", marker="o")
        mx = max(df["input_mb"].max(), df["store_mb"].max())
        plt.plot([0, mx], [0, mx], linestyle='--', color='gray', label='y = x')
        plt.title(f"Stored vs Input Size{title_suffix}")
        plt.xlabel("Input Size (MiB)")
        plt.ylabel("Stored Size (MiB)")
        plt.xlim(left=0)
        plt.ylim(bottom=0)
        plt.grid(True, alpha=0.3)
        plt.legend()
        out1 = out_dir / f"plot_store_vs_input{suffix}.png"
        plt.tight_layout()
        plt.savefig(out1, dpi=160)
        plt.close()

        # Plot 2: Payload/Input ratio vs Input size
        plt.figure(figsize=(8, 6))
        sns.lineplot(data=df, x="input_mb", y="ratio_payload_vs_input", hue="backend", style="width_bytes", marker="o")
        plt.title(f"Payload/Input Ratio vs Input Size{title_suffix}")
        plt.xlabel("Input Size (MiB)")
        plt.ylabel("Payload/Input Ratio")
        plt.xlim(left=0)
        plt.ylim(bottom=0)
        plt.grid(True, alpha=0.3)
        out2 = out_dir / f"plot_ratio_vs_input{suffix}.png"
        plt.tight_layout()
        plt.savefig(out2, dpi=160)
        plt.close()

        print(f"[plot] Saved: {out1}")
        print(f"[plot] Saved: {out2}")
        return
    except Exception as e:
        print(f"[plot] Matplotlib plotting unavailable ({e}); falling back to SVG.")

    # Fallback SVG
    import csv as _csv
    rows = []
    with open(csv_path, newline="") as f:
        r = _csv.DictReader(f)
        for row in r:
            rows.append({
                "input_mb": float(row["input_bytes"]) / (1024 * 1024),
                "store_mb": float(row["store_bytes"]) / (1024 * 1024),
                "ratio": float(row["ratio_payload_vs_input"]),
                "backend": row.get("backend", "POSIX"),
                "width": int(float(row["width_bytes"]))
            })

    def save_svg_xy(data, xkey, ykey, title, xlabel, ylabel, outfile):
        W, H = 900, 560
        M = 70
        xs = sorted(set(d[xkey] for d in data))
        if 0.0 not in xs:
            xs.insert(0, 0.0)
        ss = sorted(set(d["backend"] for d in data))
        ymin = 0.0
        ymax = max(d[ykey] for d in data) if data else 1.0
        if ymax <= 0:
            ymax = 1.0
        else:
            ymax *= 1.10
        x_min = 0.0
        x_max = float(max(xs)) if xs else 1.0
        if x_max == x_min:
            x_max = x_min + 1.0
        def sx(x):
            return M + (float(x) - x_min) * (W - 2*M) / (x_max - x_min)
        def sy(y):
            return H - M - (y - ymin) * (H - 2*M) / (ymax - ymin)
        palette = ["#1f77b4", "#ff7f0e", "#2ca02c", "#d62728", "#9467bd", "#8c564b"]
        parts = []
        parts.append(f"<svg xmlns='http://www.w3.org/2000/svg' width='{W}' height='{H}'>")
        parts.append(f"<rect x='0' y='0' width='{W}' height='{H}' fill='white'/>")
        parts.append(f"<text x='{W/2}' y='{M/2}' text-anchor='middle' font-family='sans-serif' font-size='18'>{title}</text>")
        parts.append(f"<line x1='{M}' y1='{H-M}' x2='{W-M}' y2='{H-M}' stroke='black'/>")
        parts.append(f"<line x1='{M}' y1='{M}' x2='{M}' y2='{H-M}' stroke='black'/>")
        parts.append(f"<text x='{W/2}' y='{H-10}' text-anchor='middle' font-size='14' font-family='sans-serif'>{xlabel}</text>")
        parts.append(f"<text x='{M/2}' y='{H/2}' transform='rotate(-90 {M/2},{H/2})' text-anchor='middle' font-size='14' font-family='sans-serif'>{ylabel}</text>")
        for x in xs:
            X = sx(x)
            parts.append(f"<line x1='{X}' y1='{H-M}' x2='{X}' y2='{H-M+5}' stroke='black'/>")
            parts.append(f"<text x='{X}' y='{H-M+20}' text-anchor='middle' font-size='12' font-family='sans-serif'>{x:.2f}</text>")
        for i in range(6):
            y = ymin + i*(ymax - ymin)/5
            Y = sy(y)
            parts.append(f"<line x1='{M-5}' y1='{Y}' x2='{M}' y2='{Y}' stroke='black'/>")
            parts.append(f"<text x='{M-10}' y='{Y+4}' text-anchor='end' font-size='12' font-family='sans-serif'>{y:.2f}</text>")
            parts.append(f"<line x1='{M}' y1='{Y}' x2='{W-M}' y2='{Y}' stroke='#eee'/>")
        for idx, s in enumerate(ss):
            color = palette[idx % len(palette)]
            pts = sorted([d for d in data if d["backend"] == s], key=lambda k: k[xkey])
            if not pts:
                continue
            path = " ".join(f"L {sx(p[xkey]):.2f} {sy(p[ykey]):.2f}" for p in pts)
            x0, y0 = sx(pts[0][xkey]), sy(pts[0][ykey])
            parts.append(f"<path d='M {x0:.2f} {y0:.2f} {path}' fill='none' stroke='{color}' stroke-width='2'/>")
            for p in pts:
                parts.append(f"<circle cx='{sx(p[xkey]):.2f}' cy='{sy(p[ykey]):.2f}' r='3' fill='{color}'/>")
        parts.append("</svg>")
        Path(outfile).write_text("\n".join(parts))
        print(f"[plot] Saved: {outfile}")

    ts = csv_path.stem.replace("overheads_", "")
    suffix = f"_{ts}" if ts else ""
    save_svg_xy(rows, "input_mb", "store_mb", f"Stored vs Input Size [{ts}]", "Input Size (MiB)", "Stored Size (MiB)", out_dir / f"plot_store_vs_input{suffix}.svg")
    save_svg_xy(rows, "input_mb", "ratio", f"Payload/Input Ratio vs Input Size [{ts}]", "Input Size (MiB)", "Payload/Input Ratio", out_dir / f"plot_ratio_vs_input{suffix}.svg")


def main():
    ap = argparse.ArgumentParser(description="Benchmark input vs. binary stored size for Store operator")
    ap.add_argument("--rows", default="10000,25000,50000,100000,200000,400000,800000", help="Comma-separated list of row counts")
    ap.add_argument("--widths", default="16,32,64,128,256,512,1024", help="Comma-separated list of row widths (bytes)")
    ap.add_argument("--store-backends", default="POSIX", help="Comma-separated backends: POSIX,IO_URING")
    ap.add_argument("--mode", default=os.environ.get("MODE", "INTERPRETER"), help="Execution mode: INTERPRETER or COMPILER")
    ap.add_argument("--build-dir", default=str(DEFAULT_BUILD_DIR))
    ap.add_argument("--data-dir", default=str(DEFAULT_TEST_DATA))
    ap.add_argument("--work-root", default=None, help="Working root; defaults to <build>/bench_store_size")
    ap.add_argument("--no-build", action="store_true", help="Skip building systest target")
    args = ap.parse_args()

    build_dir = Path(args.build_dir).resolve()
    data_dir = Path(args.data_dir).resolve()
    work_root = Path(args.work_root) if args.work_root else (build_dir / "bench_store_size")
    work_root.mkdir(parents=True, exist_ok=True)

    if not args.no_build:
        subprocess.run(["bash", "-lc", f"cd '{build_dir}' && make systest -j$(( $(nproc 2>/dev/null || echo 4) ))"], check=True)

    rows_list = [int(x.strip()) for x in args.rows.split(",") if x.strip()]
    widths_list = [int(x.strip()) for x in args.widths.split(",") if x.strip()]
    backends = [x.strip().upper() for x in args.store_backends.split(",") if x.strip()]

    ts = datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_path = work_root / f"overheads_{ts}.csv"
    with open(csv_path, "w", newline="") as cf:
        writer = csv.writer(cf)
        writer.writerow(["rows", "width_bytes", "backend", "input_bytes", "store_bytes", "header_bytes", "payload_bytes", "ratio_file_vs_input", "ratio_payload_vs_input"])  # header

        for rows in rows_list:
            for width in widths_list:
                for backend in backends:
                    case_dir = work_root / f"rows_{rows}_w_{width}_{backend}"
                    case_dir.mkdir(parents=True, exist_ok=True)
                    store_file = case_dir / f"store_{rows}_{width}_{backend}_{ts}.bin"
                    test_path = case_dir / f"StoreSizeGenAuto_{backend}.test"
                    test_content = make_test_content(rows, width, store_file, backend)
                    test_path.write_text(test_content)

                    _ = run_systest(build_dir, test_path, data_dir, case_dir / "run", args.mode)

                    # Compute sizes
                    row_width = max(16, width)
                    input_bytes = rows * row_width
                    store_bytes = store_file.stat().st_size if store_file.exists() else 0
                    # Derive header and payload: header = max(0, store - input), payload = store - header
                    header_bytes = max(0, store_bytes - input_bytes)
                    payload_bytes = max(0, store_bytes - header_bytes)
                    ratio_file = (store_bytes / input_bytes) if input_bytes > 0 else math.nan
                    ratio_payload = (payload_bytes / input_bytes) if input_bytes > 0 else math.nan

                    writer.writerow([rows, row_width, backend, input_bytes, store_bytes, header_bytes, payload_bytes, f"{ratio_file:.6f}", f"{ratio_payload:.6f}"])
                    print(f"[done] backend={backend} rows={rows} width={row_width}B -> input={input_bytes}B store={store_bytes}B ratio_file={ratio_file:.4f} ratio_payload={ratio_payload:.4f}")

    plot_csv(csv_path, work_root, title_suffix=f" [{ts}]", name_suffix=ts)


if __name__ == "__main__":
    main()
