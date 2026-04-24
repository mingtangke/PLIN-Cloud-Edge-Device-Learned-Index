#!/usr/bin/env bash
PROJ="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$PROJ/output"
echo "=== Processes ===" && pgrep -af 'edge_server|end_node|cloud_server' 2>/dev/null || echo "none"
echo ""
for f in "$OUT"/cloud.log "$OUT"/edge_*.log "$OUT"/end_*.log; do
    [ -f "$f" ] || continue
    echo "--- $(basename "$f") (last 3 lines) ---"
    tail -3 "$f"
done
