#!/usr/bin/env bash
set -euo pipefail

# Benchmark baseline vs. Store operator overhead using systest benchmark mode.
# Runs nes-systests/benchmark/StoreOverhead_Generator.test (two queries: baseline, store)

MODE=${MODE:-INTERPRETER}         # INTERPRETER or COMPILER
JOBS=${JOBS:-$(nproc || echo 4)}   # parallel build jobs
ROOT_DIR=$(cd "$(dirname "$0")/.." && pwd)
BUILD_DIR=${BUILD_DIR:-"$ROOT_DIR/build-nix"}
WORKDIR=${WORKDIR:-"$BUILD_DIR/bench_store_overhead"}
TEST_FILE="$ROOT_DIR/nes-systests/benchmark/StoreOverhead_Generator.test"
TEST_DATA_DIR="$ROOT_DIR/nes-systests/testdata"

echo "[build] Building systest (MODE=$MODE, JOBS=$JOBS)"
nix develop --command bash -lc "cd '$BUILD_DIR' && make systest -j$JOBS"

mkdir -p "$WORKDIR"
rm -f "$BUILD_DIR/systest_store_overhead.bin" "$WORKDIR/BenchmarkResults.json"

echo "[run] Running systest in benchmark mode (workingDir=$WORKDIR)"
nix develop --command bash -lc "cd '$BUILD_DIR' && \
  ./nes-systests/systest/systest -b \
    --workingDir '$WORKDIR' \
    --testLocation '$TEST_FILE' \
    --data '$TEST_DATA_DIR' \
    -- --worker.default_query_execution.execution_mode=$MODE"

echo "[results] BenchmarkResults.json -> $WORKDIR/BenchmarkResults.json"
if [[ ! -f "$WORKDIR/BenchmarkResults.json" ]]; then
  echo "ERROR: BenchmarkResults.json not found" >&2
  exit 1
fi

python3 - "$WORKDIR/BenchmarkResults.json" << 'PY'
import json, sys
path = sys.argv[1]
with open(path,"r") as f:
    data = json.load(f)
if not isinstance(data, list) or len(data) < 2:
    print("ERROR: Unexpected JSON format or less than 2 entries", file=sys.stderr)
    sys.exit(2)

# Assume order: [baseline, store]
b = data[0]
s = data[1]
bt = float(b.get("time", float("nan")))
st = float(s.get("time", float("nan")))
delta = st - bt
overhead = (delta / bt * 100.0) if bt > 0 else float("nan")
print("Summary: Store overhead vs baseline")
print(f"- Baseline time: {bt:.6f} s")
print(f"- Store time:    {st:.6f} s")
print(f"- Delta:         {delta:.6f} s ({overhead:.2f}%)")
PY

if [[ -f "$BUILD_DIR/systest_store_overhead.bin" ]]; then
  echo "[artifact] Store file: $BUILD_DIR/systest_store_overhead.bin ($(stat -c '%s bytes' "$BUILD_DIR/systest_store_overhead.bin" 2>/dev/null || wc -c < "$BUILD_DIR/systest_store_overhead.bin"))"
fi

echo "Done."

