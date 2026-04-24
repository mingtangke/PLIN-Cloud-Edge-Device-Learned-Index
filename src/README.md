# src/

Active source tree for the cloud-edge-device learned-index runtime.

## Layout

| Path | Role |
|---|---|
| `core/index/` | Original PLIN learned-index core: model fitting, inner/leaf nodes, B+ overflow tree, parameter serialization, and the End-side `cache_model`. |
| `common/` | Shared runtime contracts: message types, length-prefixed RPC frames, topology parser, and `topology.yaml`. |
| `cloud/` | Cloud process: loads global LSTM, reads workload statistics, pushes `HOT_UPDATE`, and routes cross-Edge lookup requests. |
| `edge/` | Edge process: builds one PLIN per Edge range, pushes PLIN parameters to End nodes, serves sibling End fetches, and bridges Cloud traffic. |
| `end/` | End process: local B+ shard, hot cache, parent PLIN parameter cache, local LSTM loader, and four-stage lookup self-test. |
| `tools/workload/` | Source tools for generating workload input data. |

## Boundaries

- `src/core/index/` is header-heavy and still close to the original PLIN implementation. Keep algorithm changes there.
- `src/common/` must stay dependency-light; it is shared by Cloud, Edge, End, and loopback tests.
- `src/cloud/`, `src/edge/`, and `src/end/` are process entrypoints. Networking behavior should live next to the process that owns it.
- `legacy/` is not part of the active CMake build.

## Build Outputs

The top-level CMake keeps compatibility output paths:

- `build/cloud/cloud_server`
- `build/edge/edge_server`
- `build/end/end_node`
- `build/common/loopback_test`
