#!/usr/bin/env bash
pkill -f 'edge_server' 2>/dev/null; pkill -f 'end_node' 2>/dev/null
pkill -f 'cloud_server' 2>/dev/null; pkill -f 'cloud_lstm.py' 2>/dev/null
echo "[stop_all] done"
