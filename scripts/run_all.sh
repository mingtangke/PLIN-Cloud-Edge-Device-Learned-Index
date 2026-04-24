#!/usr/bin/env bash
# Launch 1 cloud_server + 2 edge_servers + 10 end_nodes.
# Usage: bash scripts/run_all.sh [DATA_PATH] [TOPOLOGY_PATH] [WORKLOAD_PATH]
set -euo pipefail

PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="${1:-$PROJ/dataset/Data.txt}"
TOPO="${2:-$PROJ/src/common/topology.yaml}"
WLOG="${3:-$PROJ/data/workload_log.csv}"
if [ ! -f "$TOPO" ] && [ -f "$PROJ/$TOPO" ]; then
    TOPO="$PROJ/$TOPO"
fi
if [ ! -f "$TOPO" ] && [ "$(basename "$TOPO")" = "topology.yaml" ] && [ -f "$PROJ/src/common/topology.yaml" ]; then
    TOPO="$PROJ/src/common/topology.yaml"
fi
if [ ! -f "$WLOG" ] && [ -f "$PROJ/../PLIN-Cloud-Edge-Device-Learned-Index_odd/data/workload_log.csv" ]; then
    WLOG="$PROJ/../PLIN-Cloud-Edge-Device-Learned-Index_odd/data/workload_log.csv"
fi
OUT="$PROJ/output"
BUILD="${PLIN_BUILD_DIR:-build}"
if [[ "$BUILD" != /* ]]; then
    BUILD="$PROJ/$BUILD"
fi
EDGE_TRANSPORT="${PLIN_EDGE_TRANSPORT:-auto}"
END_TRANSPORT="${PLIN_END_TRANSPORT:-$EDGE_TRANSPORT}"
RDMA_PORT_OFFSET="${PLIN_RDMA_PORT_OFFSET:-1000}"
EDGE_EXTRA_ARGS=()
END_EXTRA_ARGS=()
if [ -n "${PLIN_EDGE_EXTRA_ARGS:-}" ]; then
    # shellcheck disable=SC2206
    EDGE_EXTRA_ARGS=(${PLIN_EDGE_EXTRA_ARGS})
fi
if [ -n "${PLIN_END_EXTRA_ARGS:-}" ]; then
    # shellcheck disable=SC2206
    END_EXTRA_ARGS=(${PLIN_END_EXTRA_ARGS})
fi

mkdir -p "$OUT"

# Stop any previous run
bash "$PROJ/scripts/stop_all.sh" 2>/dev/null || true
sleep 1

echo "=== Starting cloud server ==="
CLOUD_MODEL="$PROJ/hot_lstm/models/cloud_lstm.pt"
nohup "$BUILD/cloud/cloud_server" \
    --topology "$TOPO" --data "$DATA" --workload "$WLOG" --model "$CLOUD_MODEL" \
    --hot-interval 10 --hot-topn 32 --predict-k 3 \
    > "$OUT/cloud.log" 2>&1 &
echo "[run_all] cloud pid=$! log=output/cloud.log"

echo "=== Waiting for cloud ==="
for _ in $(seq 1 60); do
    grep -q "listening on port" "$OUT/cloud.log" 2>/dev/null && break
    sleep 1
done
grep -q "listening on port" "$OUT/cloud.log" \
    && echo "[run_all] cloud ready" \
    || echo "[run_all] WARNING: cloud may not be ready"

echo "=== Starting edge servers ==="
for EDGE_ID in 1 2; do
    nohup "$BUILD/edge/edge_server" \
        --id "$EDGE_ID" --topology "$TOPO" --data "$DATA" \
        --end-transport "$EDGE_TRANSPORT" --rdma-port-offset "$RDMA_PORT_OFFSET" \
        "${EDGE_EXTRA_ARGS[@]}" \
        > "$OUT/edge_${EDGE_ID}.log" 2>&1 &
    echo "[run_all] edge $EDGE_ID pid=$! log=output/edge_${EDGE_ID}.log"
done

# Wait for edges to be ready (grep log for "listening")
echo "=== Waiting for edges ==="
for EDGE_ID in 1 2; do
    for _ in $(seq 1 60); do
        grep -q "listening on port" "$OUT/edge_${EDGE_ID}.log" 2>/dev/null && break
        sleep 1
    done
    grep -q "listening on port" "$OUT/edge_${EDGE_ID}.log" \
        && echo "[run_all] edge $EDGE_ID ready" \
        || echo "[run_all] WARNING: edge $EDGE_ID may not be ready"
done

echo "=== Starting end nodes ==="
for END_ID in $(seq 1 10); do
    MODEL="$PROJ/hot_lstm/models/end_lstm_${END_ID}.pt"
    [ -f "$MODEL" ] || MODEL="$PROJ/hot_lstm/models/end_lstm_1.pt"
    [ -f "$MODEL" ] || MODEL="$PROJ/hot_lstm/models/end_lstm.pt"
    nohup "$BUILD/end/end_node" \
        --id "$END_ID" --topology "$TOPO" --data "$DATA" --model "$MODEL" \
        --edge-transport "$END_TRANSPORT" --rdma-port-offset "$RDMA_PORT_OFFSET" \
        "${END_EXTRA_ARGS[@]}" \
        > "$OUT/end_${END_ID}.log" 2>&1 &
    echo "[run_all] end $END_ID pid=$! log=output/end_${END_ID}.log"
done

echo "=== All started. Monitor with: bash scripts/status_all.sh ==="
