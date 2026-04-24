#!/usr/bin/env bash
# Run the 3-layer Cloud/Edge/End benchmark and emit CSV + Markdown reports.
#
# Usage:
#   bash scripts/bench.sh [QUERIES_PER_END] [DATA_PATH] [WORKLOAD_PATH] [TOPOLOGY_PATH]
#
# Environment:
#   PLIN_BENCH_WAIT_MS=12000     # let Ends drain HOT_UPDATE before replay
#   PLIN_BENCH_TIMEOUT_SEC=900   # wait for all End benchmark lines
#   PLIN_BENCH_SKIP_BUILD=1      # skip cmake build step
#   PLIN_EDGE_TRANSPORT=auto     # tcp|rdma|auto for End-Edge transport
#   PLIN_BUILD_DIR=build-rdma    # use RDMA-enabled build directory
#   PLIN_ENABLE_RDMA=1           # configure the build directory with RDMA support
# set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QUERIES="${1:-1000}"
DATA="${2:-$PROJ/dataset/Data.txt}"
WLOG="${3:-$PROJ/data/workload_log.csv}"
TOPO="${4:-$PROJ/src/common/topology.yaml}"
OUT="$PROJ/output"
CSV="$OUT/benchmark_3layer.csv"
MD="$OUT/benchmark_3layer.md"
WAIT_MS="${PLIN_BENCH_WAIT_MS:-12000}"
TIMEOUT_SEC="${PLIN_BENCH_TIMEOUT_SEC:-1000}"
PYTHON_BIN="${PYTHON:-python3}"
EDGE_TRANSPORT="${PLIN_EDGE_TRANSPORT:-auto}"
END_TRANSPORT="${PLIN_END_TRANSPORT:-$EDGE_TRANSPORT}"
BUILD_DIR="${PLIN_BUILD_DIR:-build}"
if [[ "$BUILD_DIR" != /* ]]; then
    BUILD_DIR="$PROJ/$BUILD_DIR"
fi
if ! command -v "$PYTHON_BIN" >/dev/null 2>&1 && [ -x /root/miniconda3/bin/python3 ]; then
    PYTHON_BIN="/root/miniconda3/bin/python3"
fi

if [ ! -f "$DATA" ] && [ -f "$PROJ/../PLIN-Cloud-Edge-Device-Learned-Index_odd/dataset/Data.txt" ]; then
    DATA="$PROJ/../PLIN-Cloud-Edge-Device-Learned-Index_odd/dataset/Data.txt"
fi
if [ ! -f "$WLOG" ] && [ -f "$PROJ/../PLIN-Cloud-Edge-Device-Learned-Index_odd/data/workload_log.csv" ]; then
    WLOG="$PROJ/../PLIN-Cloud-Edge-Device-Learned-Index_odd/data/workload_log.csv"
fi
if [ ! -f "$TOPO" ] && [ -f "$PROJ/$TOPO" ]; then
    TOPO="$PROJ/$TOPO"
fi

mkdir -p "$OUT"

if [ ! -f "$DATA" ]; then
    echo "[bench] missing Data.txt: $DATA" >&2
    exit 1
fi
if [ ! -f "$WLOG" ]; then
    echo "[bench] missing workload_log.csv: $WLOG" >&2
    exit 1
fi
if [ ! -f "$TOPO" ]; then
    echo "[bench] missing topology: $TOPO" >&2
    exit 1
fi

if [ "${PLIN_BENCH_SKIP_BUILD:-0}" != "1" ]; then
    CMAKE_ARGS=(-DCMAKE_BUILD_TYPE=Release)
    if [ "${PLIN_ENABLE_RDMA:-0}" = "1" ]; then
        CMAKE_ARGS+=(-DPLIN_ENABLE_RDMA=ON)
    fi
    cmake -B "$BUILD_DIR" "${CMAKE_ARGS[@]}"
    cmake --build "$BUILD_DIR" -j "$(nproc)"
fi

echo "[bench] data=$DATA"
echo "[bench] workload=$WLOG"
echo "[bench] topology=$TOPO"
echo "[bench] queries_per_end=$QUERIES wait_ms=$WAIT_MS"
echo "[bench] edge_transport=$EDGE_TRANSPORT end_transport=$END_TRANSPORT build_dir=$BUILD_DIR"

PLIN_END_EXTRA_ARGS="--bench-workload $WLOG --bench-queries $QUERIES --bench-wait-ms $WAIT_MS" \
PLIN_BUILD_DIR="$BUILD_DIR" \
    bash "$PROJ/scripts/run_all.sh" "$DATA" "$TOPO" "$WLOG"

deadline=$((SECONDS + TIMEOUT_SEC))
while true; do
    count="$( (grep -h '^\[bench\] end=' "$OUT"/end_*.log 2>/dev/null || true) | wc -l | tr -d ' ')"
    if [ "$count" -ge 10 ]; then
        break
    fi
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "[bench] timeout waiting for 10 End benchmark lines; got $count" >&2
        bash "$PROJ/scripts/status_all.sh" || true
        bash "$PROJ/scripts/stop_all.sh" || true
        exit 2
    fi
    sleep 5
done

"$PYTHON_BIN" - "$OUT" "$CSV" "$MD" "$DATA" "$WLOG" "$TOPO" "$QUERIES" "$WAIT_MS" "$EDGE_TRANSPORT" "$END_TRANSPORT" "$BUILD_DIR" <<'PY'
import csv
import datetime as dt
import glob
import os
import re
import sys

out_dir, csv_path, md_path, data_path, workload_path, topo_path, queries, wait_ms, edge_transport, end_transport, build_dir = sys.argv[1:]

rows = []
for path in sorted(glob.glob(os.path.join(out_dir, "end_*.log"))):
    bench_lines = []
    with open(path, errors="replace") as f:
        for line in f:
            if line.startswith("[bench] end="):
                bench_lines.append(line.strip())
    if not bench_lines:
        continue
    line = bench_lines[-1]
    fields = dict(re.findall(r"([A-Za-z0-9_]+)=([^ ]+)", line))
    rows.append({
        "end": int(fields["end"]),
        "queries": int(fields["queries"]),
        "found": int(fields["found"]),
        "not_found": int(fields["not_found"]),
        "stage1": int(fields["stage1"]),
        "stage2": int(fields["stage2"]),
        "stage3": int(fields["stage3"]),
        "stage4": int(fields["stage4"]),
        "seconds": float(fields["seconds"]),
        "qps": float(fields["qps"]),
        "hot_cache_size": int(fields["hot_cache_size"]),
        "log": os.path.basename(path),
    })

rows.sort(key=lambda r: r["end"])
if len(rows) != 10:
    raise SystemExit(f"expected 10 benchmark rows, got {len(rows)}")

headers = ["end", "queries", "found", "not_found", "stage1", "stage2", "stage3", "stage4", "seconds", "qps", "hot_cache_size", "transport", "log"]
for r in rows:
    r["transport"] = end_transport
with open(csv_path, "w", newline="") as f:
    w = csv.DictWriter(f, fieldnames=headers)
    w.writeheader()
    w.writerows(rows)

totals = {
    "queries": sum(r["queries"] for r in rows),
    "found": sum(r["found"] for r in rows),
    "not_found": sum(r["not_found"] for r in rows),
    "stage1": sum(r["stage1"] for r in rows),
    "stage2": sum(r["stage2"] for r in rows),
    "stage3": sum(r["stage3"] for r in rows),
    "stage4": sum(r["stage4"] for r in rows),
}
wall_seconds = max(r["seconds"] for r in rows)
aggregate_qps = totals["queries"] / wall_seconds if wall_seconds > 0 else 0.0
sum_end_qps = sum(r["qps"] for r in rows)

def pct(n):
    return (100.0 * n / totals["queries"]) if totals["queries"] else 0.0

now = dt.datetime.now().isoformat(timespec="seconds")
with open(md_path, "w") as f:
    f.write("# 3-Layer Benchmark Report\n\n")
    f.write(f"- generated_at: `{now}`\n")
    f.write(f"- data: `{data_path}`\n")
    f.write(f"- workload: `{workload_path}`\n")
    f.write(f"- topology: `{topo_path}`\n")
    f.write(f"- queries_per_end: `{queries}`\n")
    f.write(f"- bench_wait_ms: `{wait_ms}`\n\n")
    f.write(f"- edge_transport: `{edge_transport}`\n")
    f.write(f"- end_transport: `{end_transport}`\n")
    f.write(f"- build_dir: `{build_dir}`\n\n")
    f.write("## Summary\n\n")
    f.write("| metric | value |\n")
    f.write("|---|---:|\n")
    f.write(f"| total_queries | {totals['queries']} |\n")
    f.write(f"| found | {totals['found']} |\n")
    f.write(f"| not_found | {totals['not_found']} |\n")
    f.write(f"| wall_seconds_max_end | {wall_seconds:.6f} |\n")
    f.write(f"| aggregate_qps_parallel | {aggregate_qps:.2f} |\n")
    f.write(f"| sum_end_qps | {sum_end_qps:.2f} |\n\n")
    f.write("## Stage Distribution\n\n")
    f.write("| stage | count | percent |\n")
    f.write("|---|---:|---:|\n")
    f.write(f"| stage1_local | {totals['stage1']} | {pct(totals['stage1']):.2f}% |\n")
    f.write(f"| stage2_hot_cache | {totals['stage2']} | {pct(totals['stage2']):.2f}% |\n")
    f.write(f"| stage3_same_edge_plin | {totals['stage3']} | {pct(totals['stage3']):.2f}% |\n")
    f.write(f"| stage4_cross_edge | {totals['stage4']} | {pct(totals['stage4']):.2f}% |\n\n")
    f.write("## Per-End Results\n\n")
    f.write("| end | queries | found | s1 | s2 | s3 | s4 | seconds | qps | hot_cache |\n")
    f.write("|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n")
    for r in rows:
        f.write(
            f"| {r['end']} | {r['queries']} | {r['found']} | {r['stage1']} | {r['stage2']} | "
            f"{r['stage3']} | {r['stage4']} | {r['seconds']:.6f} | {r['qps']:.2f} | {r['hot_cache_size']} |\n"
        )

print(f"[bench] wrote {csv_path}")
print(f"[bench] wrote {md_path}")
print(f"[bench] aggregate_qps_parallel={aggregate_qps:.2f} total_queries={totals['queries']}")
PY

bash "$PROJ/scripts/stop_all.sh" || true
echo "[bench] done"


# PLIN_BENCH_WAIT_MS=12000 bash scripts/bench.sh 1000 \
#     /root/gym/PLIN-Cloud-Edge-Device-Learned-Index_odd/dataset/Data.txt \
#     /root/gym/PLIN-Cloud-Edge-Device-Learned-Index_odd/data/workload_log.csv \
#     src/common/topology.yaml
