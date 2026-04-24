**English** | [简体中文](README.zh-CN.md)

# PLIN Cloud-Edge-Device Learned Index

This repository implements a Cloud-Edge-Device learned-index runtime built around PLIN. The system combines regional learned indexes, local B+ tree shards, hot-key caching, LSTM-based hot-key prediction, and an optional RDMA transport for the End-to-Edge query path.

![PLIN Cloud-Edge-Device system overview](figure/overview.png)

## Overview

The runtime is composed of three process types:

| Layer | Executable | Responsibility |
|---|---|---|
| Cloud | `cloud_server` | Maintains a global workload view, runs Cloud LSTM hot-key prediction, sends hot-key updates, and routes cross-Edge queries. |
| Edge | `edge_server` | Owns a group of End key ranges, builds a regional PLIN index, pushes learned-index parameters to End nodes, serves same-Edge queries, and forwards Cloud control messages. |
| End | `end_node` | Owns a local B+ tree shard, a hot cache, and a parent-Edge PLIN parameter cache; executes lookups and reports stage-level statistics. |

The default topology contains one Cloud, two Edge nodes, and ten End nodes. The topology is configured in `src/common/topology.yaml`, including node addresses, ports, ownership, and key ranges.

## Query Path

Each End node resolves a query key through four stages:

1. **Local B+ tree**: if the key belongs to the current End range, the End queries its local shard directly.
2. **Hot cache**: if the local shard does not resolve the key, the End checks its libcuckoo hot cache.
3. **Same-Edge PLIN**: if the target End belongs to the same Edge, the End predicts a logical PLIN leaf using the parent-Edge parameter cache. With RDMA enabled, the End first attempts an RDMA READ against the stable Edge snapshot. If RDMA is unavailable or the key is not found in the snapshot, it falls back to `EDGE_FETCH_REQ`.
4. **Cross-Edge routing**: if the target End belongs to another Edge, the request is routed as End -> Edge -> Cloud -> target Edge.

Stage-level lookup counters are printed by `end_node` and aggregated by the benchmark script.

## Architecture

```text
                      +----------------------+
                      | Cloud                |
                      | cloud_server         |
                      | - workload view      |
                      | - Cloud LSTM         |
                      | - HOT_UPDATE         |
                      | - cross-Edge routing |
                      +----------+-----------+
                                 |
                 Edge control TCP|
                                 |
             +-------------------+-------------------+
             |                                       |
    +--------+---------+                    +--------+---------+
    | Edge 1           |                    | Edge 2           |
    | edge_server      |                    | edge_server      |
    | regional PLIN    |                    | regional PLIN    |
    | RDMA/TCP endpoint|                    | RDMA/TCP endpoint|
    +---+---+---+---+--+                    +---+---+---+---+--+
        |   |   |   |                           |   |   |   |
      End1 ... End5                           End6 ... End10
```

End-to-Edge traffic can use TCP, RDMA, or auto mode. Edge-to-Cloud traffic remains TCP.

## Transport Modes

| Mode | Description |
|---|---|
| `tcp` | Uses length-prefixed TCP frames. This mode does not require RDMA hardware. |
| `rdma` | Uses RDMA CM/libibverbs for End-to-Edge connections. Control frames use RDMA SEND/RECV; the same-Edge fast path can use RDMA READ. |
| `auto` | End nodes try RDMA first and fall back to TCP. This is useful for development and compatibility checks; use `rdma` for strict RDMA performance experiments. |

RDMA only affects End-to-Edge query traffic. Cloud control traffic and cross-Edge routing remain TCP-based.

## Repository Layout

```text
.
├── src/
│   ├── core/index/        # PLIN core, local model, leaf nodes, B+ tree support
│   ├── common/            # protocol, RPC, transport, RDMA snapshot, topology
│   ├── cloud/             # Cloud runtime and Cloud LSTM runner
│   ├── edge/              # Edge runtime and regional PLIN service
│   ├── end/               # End runtime, hot cache, parent PLIN cache
│   └── tools/workload/    # workload generation tool source
├── hot_lstm/              # LSTM training/export code and model files
├── scripts/               # run, stop, status, benchmark helpers
├── doc/                   # architecture notes and generated figures
├── output/                # runtime logs and benchmark reports
├── third_party/           # TLX and optional libtorch location
├── libcuckoo/             # hot-cache dependency
└── legacy/                # archived prototype code, not part of the active build
```

## Components

### `src/core/index/`

| File | Responsibility |
|---|---|
| `plin_index.h` | Regional PLIN index used by Edge, including `bulk_load`, `find`, `find_through_net`, split, and rebuild operations. |
| `cache_model.h` | Parent-Edge PLIN parameter cache used by End nodes to predict a key's leaf slot. |
| `serialize.h` | Serialization and deserialization for `Param[][]`, used by `PLIN_PARAM_PUSH`. |
| `piecewise_linear_model.h` | PGM-style piecewise linear fitting for learned model segments. |
| `leaf_node.h`, `inner_node.h` | PLIN leaf and inner node structures. |
| `b_plus.h` | B+ tree implementation used for leaf overflow. |
| `parameters.h` | Key/payload types, block size, epsilon, split/rebuild thresholds, and related PLIN settings. |

### `src/common/`

| File | Responsibility |
|---|---|
| `proto.h` | Message and status enums shared by all runtime processes. |
| `rpc.h`, `rpc.cpp` | Length-prefixed frame encoding and decoding. |
| `transport.h`, `transport.cpp` | End-to-Edge transport abstraction and TCP implementation. |
| `rdma_transport.h`, `rdma_transport.cpp` | Optional RDMA CM/libibverbs transport. |
| `rdma_snapshot.h` | Stable Edge snapshot layout exposed to the End RDMA READ fast path. |
| `range_map.h`, `range_map.cpp` | Topology parser and helpers such as `locate_end`, `edge_of`, `same_edge`, and `siblings_of`. |
| `loopback_test.cpp` | Basic RPC and topology test. |

### `src/cloud/`

| File | Responsibility |
|---|---|
| `cloud_server.cpp` | Cloud runtime process for Edge registration, hot-key update generation, and cross-Edge lookup routing. |
| `cloud_lstm_runner.cpp` | libtorch implementation that loads `cloud_lstm.pt` and calls the TorchScript model. |
| `cloud_lstm_runner_stub.cpp` | Fallback implementation used when libtorch is unavailable. |

### `src/edge/`

| File | Responsibility |
|---|---|
| `edge_server.cpp` | Edge runtime process that builds regional PLIN, serves End queries, forwards Cloud control messages, and exposes RDMA snapshots when enabled. |

### `src/end/`

| File | Responsibility |
|---|---|
| `end_node.cpp` | End runtime process that loads the local shard, connects to the parent Edge, runs lookup self-tests, and executes benchmarks. |
| `hot_cache.h`, `hot_cache.cpp` | Thin wrapper around libcuckoo for hot-key lookup and batch updates. |
| `parent_plin_cache.h`, `parent_plin_cache.cpp` | End-side cache for parent-Edge PLIN parameters. |
| `end_lstm_runner.cpp` | End LSTM model loader. |
| `end_lstm_runner_stub.cpp` | Fallback implementation used when libtorch is unavailable. |

## Data and Models

| File | Format | Purpose |
|---|---|---|
| `Data.txt` | one `<key> <payload>` pair per line | Global sorted key/payload data source. |
| `workload_log.csv` | `timestamp,device_id,key,operation` | Access stream for hot-key training and benchmark replay. |
| `hot_lstm/models/cloud_lstm.pt` | TorchScript | Cloud hot-key prediction model. |
| `hot_lstm/models/end_lstm_<id>.pt` | TorchScript | End-side model files. The current End runtime primarily validates model loading. |

During benchmark replay, the workload `key` column is interpreted as a Data row position and mapped to the real key.

## Message Protocol

Message types are defined in `src/common/proto.h`.

| Message | Direction | Purpose |
|---|---|---|
| `HEARTBEAT` | Edge/End registration | Register Edge with Cloud or End with Edge. |
| `HEARTBEAT_ACK` | Cloud -> Edge | Acknowledge Edge registration. |
| `PLIN_PARAM_PUSH` | Edge -> End | Push serialized parent-Edge PLIN parameters. |
| `HOT_UPDATE` | Cloud -> Edge -> End | Batch-write hot key/payload pairs into an End hot cache. |
| `EDGE_FETCH_REQ` | End -> Edge | Same-Edge lookup request with key and predicted slot. |
| `EDGE_FETCH_RESP` | Edge/Cloud -> caller | Lookup status and payload. |
| `CROSS_EDGE_REQ` | End -> Edge -> Cloud -> Edge | Cross-Edge lookup request. |
| `RDMA_SNAPSHOT_INFO` | Edge -> End | Remote memory address and rkey metadata for RDMA READ. |

## Build

Default TCP build:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"
```

Build outputs:

```text
build/cloud/cloud_server
build/edge/edge_server
build/end/end_node
build/common/loopback_test
```

Quick check:

```bash
build/common/loopback_test
```

Optional RDMA build:

```bash
sudo apt install rdma-core libibverbs-dev librdmacm-dev
cmake -B build-rdma -DCMAKE_BUILD_TYPE=Release -DPLIN_ENABLE_RDMA=ON
cmake --build build-rdma -j "$(nproc)"
```

Real RDMA experiments require visible RDMA devices, for example `/sys/class/infiniband/<device>` and `/dev/infiniband/uverbs*`, inside the host or container.

## Run

Start the full system:

```bash
bash scripts/run_all.sh \
  /path/to/Data.txt \
  src/common/topology.yaml \
  /path/to/workload_log.csv
```

Run with RDMA-enabled binaries in auto transport mode:

```bash
PLIN_BUILD_DIR=build-rdma \
PLIN_EDGE_TRANSPORT=auto \
PLIN_END_TRANSPORT=auto \
bash scripts/run_all.sh /path/to/Data.txt src/common/topology.yaml /path/to/workload_log.csv
```

Check process status:

```bash
bash scripts/status_all.sh
```

Stop all runtime processes:

```bash
bash scripts/stop_all.sh
```

## Benchmark

Run 10,000 queries per End, 100,000 total queries:

```bash
bash scripts/bench.sh 10000 \
  /path/to/Data.txt \
  /path/to/workload_log.csv \
  src/common/topology.yaml
```

Outputs:

```text
output/benchmark_3layer.csv
output/benchmark_3layer.md
```

Useful environment variables:

| Variable | Default | Meaning |
|---|---:|---|
| `PLIN_BENCH_WAIT_MS` | `12000` | Time for End nodes to drain initial `HOT_UPDATE` messages before replay. |
| `PLIN_BENCH_TIMEOUT_SEC` | `900` | Wait limit for all 10 End benchmark rows. |
| `PLIN_BENCH_SKIP_BUILD` | unset | Set to `1` to skip the build step. |
| `PLIN_BUILD_DIR` | `build` | Build directory to use, for example `build-rdma`. |
| `PLIN_ENABLE_RDMA` | unset | Set to `1` in `scripts/bench.sh` to configure an RDMA build. |
| `PLIN_EDGE_TRANSPORT` | `auto` | Edge listener mode: `tcp`, `rdma`, or `auto`. |
| `PLIN_END_TRANSPORT` | same as Edge | End connection mode: `tcp`, `rdma`, or `auto`. |
| `PLIN_RDMA_PORT_OFFSET` | `1000` | RDMA CM listens on `edge_port + offset`. |
| `PYTHON` | `python3` | Python executable used for benchmark report generation. |

## Maintenance Notes

- Active runtime source code lives under `src/`.
- `legacy/` contains archived prototype sources and is not part of the active CMake build.
- Large generated data, build directories, and runtime logs should stay out of commits.
- `scripts/run_all.sh` starts Cloud, Edge, and End processes from the selected build directory.
- For RDMA performance experiments, use `PLIN_EDGE_TRANSPORT=rdma` and `PLIN_END_TRANSPORT=rdma` to avoid mixing TCP fallback into the measurements.
