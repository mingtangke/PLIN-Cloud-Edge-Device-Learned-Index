[English](README.md) | **简体中文**

# PLIN 云边端学习索引系统

本仓库实现了一个基于 PLIN 的云边端学习索引运行时系统。系统结合了边缘侧区域学习索引、端侧本地 B+ 树分片、热点缓存、基于 LSTM 的热点预测，以及可选的 End-to-Edge RDMA 传输路径。

![PLIN 云边端系统总览图](figure/overview.png)

## 项目概览

系统由三类运行时进程组成：

| 层级 | 可执行文件 | 职责 |
|---|---|---|
| Cloud | `cloud_server` | 维护全局访问流视角，加载 Cloud LSTM，生成热点 key 更新，并路由跨 Edge 查询。 |
| Edge | `edge_server` | 管理一组 End 的 key range，构建区域 PLIN，向 End 推送学习索引参数，处理同 Edge 查询，并转发 Cloud 控制消息。 |
| End | `end_node` | 持有本地 B+ 树分片、热点缓存和父 Edge 的 PLIN 参数副本，执行查询并输出阶段统计。 |

当前默认拓扑为 1 个 Cloud、2 个 Edge、10 个 End。拓扑定义在 `src/common/topology.yaml`，可以通过配置文件调整各节点的地址、端口、从属关系和 key range。

## 查询流程

End 对每个查询 key 执行四阶段查找：

1. **本地 B+ 树**：如果 key 属于当前 End 的本地 range，直接查询本地 shard。
2. **热点缓存**：如果本地未命中，查询 End 侧 libcuckoo 热点缓存。
3. **同 Edge PLIN**：如果目标 End 属于同一个 Edge，End 使用父 Edge 下发的 PLIN 参数预测逻辑叶子。启用 RDMA 时，End 优先使用 RDMA READ 读取 Edge 暴露的稳定快照；不可用或未命中时，回退到 `EDGE_FETCH_REQ`。
4. **跨 Edge 路由**：如果目标 End 属于另一个 Edge，请求按 End → Edge → Cloud → Target Edge 的路径路由。

查询阶段统计由 `end_node` 输出，benchmark 脚本会聚合为 CSV 和 Markdown 报告。

## 系统架构

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

End-to-Edge 流量可以使用 TCP、RDMA 或 auto 模式；Edge-to-Cloud 流量始终使用 TCP。

## 传输模式

| 模式 | 说明 |
|---|---|
| `tcp` | 使用长度前缀 TCP frame。该模式不需要 RDMA 硬件。 |
| `rdma` | 使用 RDMA CM/libibverbs 建立 End-to-Edge 连接。控制消息走 RDMA SEND/RECV，同 Edge 快路径可使用 RDMA READ。 |
| `auto` | End 先尝试 RDMA，如果不可用则回退到 TCP。该模式适合开发和兼容性测试；正式 RDMA 性能实验建议使用 `rdma`。 |

RDMA 只作用于 End-to-Edge 查询链路。Cloud 控制链路和跨 Edge 路由仍然使用 TCP。

## 代码结构

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

## 关键组件

### `src/core/index/`

| 文件 | 职责 |
|---|---|
| `plin_index.h` | Edge 侧区域 PLIN 主索引，支持 `bulk_load`、`find`、`find_through_net`、split/rebuild 等操作。 |
| `cache_model.h` | End 侧父 Edge PLIN 参数副本，使用 `Param[][]` 预测 key 的 leaf slot。 |
| `serialize.h` | `Param[][]` 序列化和反序列化，用于 `PLIN_PARAM_PUSH`。 |
| `piecewise_linear_model.h` | PGM-style piecewise linear fitting，用于生成学习模型 segment。 |
| `leaf_node.h`, `inner_node.h` | PLIN leaf 和 inner node 结构。 |
| `b_plus.h` | leaf overflow 的 B+ 树实现。 |
| `parameters.h` | key/payload 类型、block size、epsilon、split/rebuild 阈值等配置。 |

### `src/common/`

| 文件 | 职责 |
|---|---|
| `proto.h` | 消息类型和状态码定义。 |
| `rpc.h`, `rpc.cpp` | 长度前缀 frame 编解码。 |
| `transport.h`, `transport.cpp` | End-to-Edge transport 抽象和 TCP 实现。 |
| `rdma_transport.h`, `rdma_transport.cpp` | 可选 RDMA CM/libibverbs transport。 |
| `rdma_snapshot.h` | Edge 暴露给 End RDMA READ 的稳定快照布局。 |
| `range_map.h`, `range_map.cpp` | 解析 topology，提供 `locate_end`、`edge_of`、`same_edge`、`siblings_of`。 |
| `loopback_test.cpp` | RPC 和 topology 基础测试。 |

### `src/cloud/`

| 文件 | 职责 |
|---|---|
| `cloud_server.cpp` | Cloud 主进程，处理 Edge 注册、热点更新和跨 Edge 查询。 |
| `cloud_lstm_runner.cpp` | libtorch 实现，加载 `cloud_lstm.pt` 并调用 TorchScript。 |
| `cloud_lstm_runner_stub.cpp` | 无 libtorch 时的 fallback。 |

### `src/edge/`

| 文件 | 职责 |
|---|---|
| `edge_server.cpp` | Edge 主进程，构建区域 PLIN，服务 End 查询，转发 Cloud 控制消息，并在 RDMA 模式下暴露快照。 |

### `src/end/`

| 文件 | 职责 |
|---|---|
| `end_node.cpp` | End 主进程，加载本地 shard，连接父 Edge，执行查询、self-test 和 benchmark。 |
| `hot_cache.h`, `hot_cache.cpp` | libcuckoo 热点缓存封装。 |
| `parent_plin_cache.h`, `parent_plin_cache.cpp` | End 侧父 Edge PLIN 参数缓存。 |
| `end_lstm_runner.cpp` | End LSTM loader。 |
| `end_lstm_runner_stub.cpp` | 无 libtorch 时的 fallback。 |

## 数据和模型

| 文件 | 格式 | 用途 |
|---|---|---|
| `Data.txt` | 每行 `<key> <payload>` | 全局有序 key/payload 数据源。 |
| `workload_log.csv` | `timestamp,device_id,key,operation` | 访问流、热点训练、benchmark 回放。 |
| `hot_lstm/models/cloud_lstm.pt` | TorchScript | Cloud 热点预测模型。 |
| `hot_lstm/models/end_lstm_<id>.pt` | TorchScript | End 侧模型文件。当前 End runtime 主要验证加载路径。 |

benchmark 中的 workload `key` 字段按 Data row position 解释，再映射为真实 key。

## 消息协议

消息类型定义在 `src/common/proto.h`。

| 消息 | 方向 | 用途 |
|---|---|---|
| `HEARTBEAT` | Edge/End 注册 | Edge 注册到 Cloud，End 注册到 Edge。 |
| `HEARTBEAT_ACK` | Cloud → Edge | 确认 Edge 注册。 |
| `PLIN_PARAM_PUSH` | Edge → End | 下发序列化后的父 Edge PLIN 参数。 |
| `HOT_UPDATE` | Cloud → Edge → End | 向 End 热点缓存批量写入热点 key/payload。 |
| `EDGE_FETCH_REQ` | End → Edge | 同 Edge 查询请求，包含 key 和预测 slot。 |
| `EDGE_FETCH_RESP` | Edge/Cloud → caller | 查询结果状态和 payload。 |
| `CROSS_EDGE_REQ` | End → Edge → Cloud → Edge | 跨 Edge 查询请求。 |
| `RDMA_SNAPSHOT_INFO` | Edge → End | RDMA READ 快路径所需的远端内存地址和 rkey。 |

## 构建

默认 TCP 构建：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j "$(nproc)"
```

构建输出：

```text
build/cloud/cloud_server
build/edge/edge_server
build/end/end_node
build/common/loopback_test
```

基础测试：

```bash
build/common/loopback_test
```

可选 RDMA 构建：

```bash
sudo apt install rdma-core libibverbs-dev librdmacm-dev
cmake -B build-rdma -DCMAKE_BUILD_TYPE=Release -DPLIN_ENABLE_RDMA=ON
cmake --build build-rdma -j "$(nproc)"
```

真实 RDMA 实验需要容器或主机内可见 `/sys/class/infiniband/<device>` 和 `/dev/infiniband/uverbs*`。

## 运行

启动完整系统：

```bash
bash scripts/run_all.sh \
  /path/to/Data.txt \
  src/common/topology.yaml \
  /path/to/workload_log.csv
```

使用 RDMA-enabled 构建并启用 auto transport：

```bash
PLIN_BUILD_DIR=build-rdma \
PLIN_EDGE_TRANSPORT=auto \
PLIN_END_TRANSPORT=auto \
bash scripts/run_all.sh /path/to/Data.txt src/common/topology.yaml /path/to/workload_log.csv
```

状态检查：

```bash
bash scripts/status_all.sh
```

停止：

```bash
bash scripts/stop_all.sh
```

## Benchmark

每个 End 回放 10,000 条查询，总计 100,000 条查询：

```bash
bash scripts/bench.sh 10000 \
  /path/to/Data.txt \
  /path/to/workload_log.csv \
  src/common/topology.yaml
```

输出：

```text
output/benchmark_3layer.csv
output/benchmark_3layer.md
```

常用环境变量：

| 变量 | 默认值 | 含义 |
|---|---:|---|
| `PLIN_BENCH_WAIT_MS` | `12000` | benchmark 开始前等待 End 消费初始 `HOT_UPDATE` 的时间。 |
| `PLIN_BENCH_TIMEOUT_SEC` | `900` | 等待 10 个 End 输出 benchmark 结果的最长时间。 |
| `PLIN_BENCH_SKIP_BUILD` | unset | 设为 `1` 时跳过构建。 |
| `PLIN_BUILD_DIR` | `build` | 使用指定构建目录，例如 `build-rdma`。 |
| `PLIN_ENABLE_RDMA` | unset | 在 `scripts/bench.sh` 中设为 `1` 时配置 RDMA 构建。 |
| `PLIN_EDGE_TRANSPORT` | `auto` | Edge 监听模式：`tcp`、`rdma` 或 `auto`。 |
| `PLIN_END_TRANSPORT` | same as Edge | End 连接模式：`tcp`、`rdma` 或 `auto`。 |
| `PLIN_RDMA_PORT_OFFSET` | `1000` | RDMA CM 监听端口为 `edge_port + offset`。 |
| `PYTHON` | `python3` | benchmark 报告生成使用的 Python。 |

## 维护说明

- 当前运行时源码位于 `src/`。
- `legacy/` 只保留归档代码，不参与当前 CMake 构建。
- 大数据文件、构建目录和运行日志不应提交。
- `scripts/run_all.sh` 会从指定 build directory 启动 Cloud、Edge 和 End。
- RDMA 性能实验请使用 `PLIN_EDGE_TRANSPORT=rdma` 和 `PLIN_END_TRANSPORT=rdma`，避免 `auto` fallback 混入 TCP 结果。
