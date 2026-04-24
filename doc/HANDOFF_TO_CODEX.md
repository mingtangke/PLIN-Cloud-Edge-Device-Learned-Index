# 交付给 Codex 的未完成任务清单

> Historical note: this handoff captured the pre-M6 state. The active source tree
> is now under `src/` (`src/cloud`, `src/edge`, `src/end`, `src/common`,
> `src/core/index`). Use `doc/ARCHITECTURE.md` as the current architecture
> reference.

## 项目状态快照

**已完成里程碑：**
- **M1** 目录骨架 + CMake + libcuckoo + TLX BTree + libtorch 探测 ✅
- **M2** RPC 帧 (`[u32 len][u8 type][body]`) + topology.yaml 解析器 + loopback 回环测试 ✅
- **M3** End 加载 Data.txt 过滤到本地 TLX B+ 树 + stage-① 本地查询（P 服务器 1,000,000 keys 自测 1000/1000 OK）✅

**正在做（未完成，有 bug）：**
- **M4+M5 合并推进**：Edge 的 PLIN 索引 + `PLIN_PARAM_PUSH` + `EDGE_FETCH_REQ`；End 的 `parent_plin_cache`（`cache_model.h` 薄包装）+ libcuckoo `hot_cache` + 连接父 Edge；`hot_lstm/` 离线训练 + TorchScript 导出 + libtorch 内嵌推理。代码全部写完且本地 + P 服务器都构建通过，但端到端测试尚未跑起来。

**未开始：**
- **M6** Cloud 节点（全局 LSTM + HOT_UPDATE 下发 + 跨 Edge 路由 stub 的实现）
- **M7** 文档改写 (`doc/ARCHITECTURE.md`) + benchmark

---

## 工作目录与环境

### 本地（开发机）
```
/home/ubuntu/gym/PLIN-Cloud-Edge-Device-Learned-Index/
```

### P 服务器（跑 demo）
```
ssh -p 11568 root@connect.westb.seetacloud.com   密码: Senku/Vjwy+H
```
路径：`/root/gym/PLIN-Cloud-Edge-Device-Learned-Index/`
数据：`/root/gym/PLIN-Cloud-Edge-Device-Learned-Index_odd/dataset/Data.txt` (9,999,999 行)
      `/root/gym/PLIN-Cloud-Edge-Device-Learned-Index_odd/data/workload_log.csv` (1,000,009 行)

P 服务器上 Python：`/root/miniconda3/bin/python3` (PyTorch 2.8.0+cu128)

### 同步命令
```bash
sshpass -p 'Senku/Vjwy+H' rsync -az \
  --exclude='build/' --exclude='build.zip' --exclude='dataset/' \
  --exclude='data/' --exclude='.git/' --exclude='third_party/libtorch/' \
  -e 'ssh -p 11568 -o StrictHostKeyChecking=no' \
  /home/ubuntu/gym/PLIN-Cloud-Edge-Device-Learned-Index/ \
  root@connect.westb.seetacloud.com:/root/gym/PLIN-Cloud-Edge-Device-Learned-Index/
```

---

## 🐞 当前已知 Bug / 未收尾的事项

### Bug 1: LSTM 训练 CSV 头重复
**现象：** `hot_lstm/train.py` 第一次跑在 P 上报 `ValueError: could not convert string to float: 'timestamp'`。
`workload_log.csv` 中间有重复的 header 行（`timestamp,device_id,key,operation`）。
**状态：** 用户已在 `hot_lstm/train.py` L27-28 添加 `if r['timestamp'] == 'timestamp': continue` 兜底。
**待做：** 用修复后的 `train.py` 重新跑训练（`--role cloud` 和 `--role end --end-id 1`），产出 `hot_lstm/models/cloud_lstm.ckpt` 和 `end_lstm_1.ckpt`。

### Bug 2: libtorch 下载慢 + CMake 已加 miniconda fallback
**现象：** `scripts/fetch_libtorch.sh` 从 pytorch.org 下 2.8.0 CPU 版 ~200MB，P 服务器很慢（已在后台排队，可能还没完成）。
**状态：** 用户已在顶层 `CMakeLists.txt` L21-44 增加 fallback：先查 `third_party/libtorch/`，找不到再查 `/root/miniconda3/lib/python3.12/site-packages/torch/`（miniconda 的 PyTorch 自带 libtorch + TorchConfig.cmake）。这条路径下需要启用 CUDA 语言才能 `find_package(Torch)` 成功（用户已加 `check_language(CUDA)` + `enable_language(CUDA)`）。
**待做：**
1. 在 P 上用 fallback 重新 cmake：`rm -rf build && cmake -B build`，预期看到 `libtorch found via miniconda PyTorch at ...` 消息。
2. build 时如果 CUDA toolkit 不存在会报错——届时降级回"等 fetch_libtorch.sh 下完 CPU 版"路径。
3. 另一个备选：改写 `fetch_libtorch.sh` 用国内镜像（AutoDL 上 pytorch.org 经常 200KB/s）。

### 未收尾 TODO（M4+M5 代码里留的）
1. **`end/end_node.cpp` 的 `param_stale_flag` 处理**：当 Edge 回的 `EDGE_FETCH_RESP` 中 `param_stale=true` 时，只写了 `// TODO M6+: async re-request PLIN_PARAM_PUSH`，还没实现。需要加一个请求 Edge 重新推 `PLIN_PARAM_PUSH` 的逻辑。
2. **`edge/edge_server.cpp` 的 slot 验证**：当前 `EDGE_FETCH_REQ` 带来的 `predicted_slot` 还没用，Edge 直接 `idx.find(key)` 全量查。应加快速路径：若 `predicted_slot` 指向的 leaf 真的包含 key 就直接返回（减少 PLIN 遍历）。PLIN 暴露的接口见 `plin_index.h`。
3. **End 后台定期跑 LSTM 预测 + 批量 HOT_UPDATE 预取**：M5 设计说 End 后台线程每 N 秒调 `end_lstm_runner.predict(...)`，拿到 `hotkey_end_i`，发批量 `EDGE_FETCH_REQ` 取 payload，灌进 `hot_cache`。目前 `end_node.cpp` 里没有这个周期性循环，只有被动接收 HOT_UPDATE 的代码。

---

## 🏗️ M6 Cloud 层（未开始）

### 目标
1 个 Cloud 节点，监听 `7000`，接 2 个 Edge 的长连接。职责：
- 启动时 `torch::jit::load("hot_lstm/models/cloud_lstm.pt")`（10 维输入 → 10 维输出）
- 持全量 `Data.txt`（或共享路径读）
- 周期性调用 `cloud_lstm_runner.predict_top_ends(counts_10x60, k)` 拿每个 End 的 top-K 目标 End
- 从目标 End 的 `key_range` 里按访问频率挑 top-N 热键 → 组成 `hotkey_cloud_i` → 查 payload → 打包 `HOT_UPDATE{target_end_id, kv_pairs}` → 下发到目标 End 所属 Edge
- 跨 Edge 查询转发（`CROSS_EDGE_REQ`，阶段 ④ 的实路径）

### 待写文件

#### 1. `cloud/cloud_lstm_runner.cpp`（当前是空壳）
要实现：
```cpp
// 构造时 torch::jit::load(model_path)
// 持 std::mutex 做推理串行化
// predict_top_ends(counts_10x60, k) 返回 std::vector<int> (0-indexed end ids)
class CloudLstmRunner {
    torch::jit::Module module_;
    std::mutex mu_;
public:
    explicit CloudLstmRunner(const std::string& pt_path);
    std::vector<int> predict_top_ends(
        const std::vector<std::vector<float>>& counts_10x60, int k);
};
```
参考：`cloud_lstm_runner.cpp` (现在只有 stub 版本 `cloud_lstm_runner_init()`)，`end_lstm_runner.cpp` 的骨架。

#### 2. `cloud/cloud_server.cpp`（当前是空壳）
要实现：
- 解析 `--port`, `--topology`, `--data`, `--model` 参数
- 启动时加载 topology + LSTM 模型
- 读 `Data.txt` 到 `std::unordered_map<_key_t, _payload_t>`（约 8GB 内存，或用分块/mmap）
- 监听 `cloud.port`，接受 2 个 Edge 的长连接，`std::thread` handle 每个
- 后台定时器线程：每 N 秒（N=30 够 demo）：
  1. 读 `data/workload_log.csv` 的最后 W 秒窗口（或维护 ring buffer），聚合成 10 端的访问计数 `[60, 10]` 矩阵
  2. 调 `lstm_runner.predict_top_ends(...)` → `top_ends`
  3. 对每个 End i，取 `top_ends[i]` 的 key_range，从 workload 里抓该 End 访问最频繁的 top-N keys → 去全量索引查 payload → 打 HOT_UPDATE 帧
  4. 按 `target_end_id → edge_id` 路由，给对应 Edge 长连接 write_frame
- 处理从 Edge 上行的 `CROSS_EDGE_REQ`：根据 `range_map` 找到目标 Edge，转发 `ROUTE_REQ`。

#### 3. `edge/edge_server.cpp` 扩展
- 启动时连 Cloud（`rm.cloud().host:port`）
- 处理 Cloud 推来的 `HOT_UPDATE{target_end_id, kv_pairs}`：按 `target_end_id` 查本 Edge 下的 End 连接，把 `HOT_UPDATE` 转发给那个 End（注意：End 的 `edge_receiver` 已经能收 `HOT_UPDATE` 并写 hot_cache——见 `end/end_node.cpp` L222-236）
- 处理跨 Edge 查询：End 发来的 `EDGE_FETCH_REQ` 若 `target_end` 不在本 Edge，应该转为 `CROSS_EDGE_REQ` 发给 Cloud，Cloud 转给对方 Edge，回包链路反向。

#### 4. `end/end_node.cpp` 扩展
- 后台线程跑 LSTM 周期推理（见上面 M4+M5 TODO 3）
- stage ④ 的实路径：目前直接返回 `NOT_IMPLEMENTED`，改为发送 `CROSS_EDGE_REQ` 给父 Edge

### M6 验收标准
1. `scripts/run_all.sh` 能拉起 cloud + 2 edge + 10 end
2. 单端发 `QUERY_REQ` 打到跨 Edge 的 key，能通过 Cloud 中转拿到 payload
3. Cloud 触发一次 `HOT_UPDATE`，End 的 `hot_cache.size()` 增长，stage ② 命中率 > 0
4. End log 中 stage ①/②/③/④ 分布合理

---

## 📄 M7 文档 + Benchmark（未开始）

### 文件
1. **重写 `doc/ARCHITECTURE.md`**：当前内容还是老的 2 层架构（边-端）。要改成 3 层（云-边-端），含：
   - Mermaid flowchart：Cloud ↔ 2×Edge ↔ 10×End
   - Mermaid sequenceDiagram：四段式 lookup + PLIN_PARAM_PUSH 时序
   - 端口 / 消息总表（`common/proto.h` 的 `MsgType`）
   - 四段式查询伪代码（从 `end/end_node.cpp::EndNode::lookup` 同步）
   - 离线数据链路（`device_generator.cpp` → `workload_log.csv` → `train.py` → `.pt`）
2. **`scripts/bench.sh`**：跑一个给每个 End 注入 10k–100k workload 回放的 benchmark，统计吞吐 + 四段式命中率分布。
3. **`output/benchmark_3layer.md`**：写测量结果，对标 `output/benchmark_4threads.md`（M4+M5 加入 hot_cache 后理论上吞吐更高）。

### M7 验收标准
- GitHub 上预览 `doc/ARCHITECTURE.md` 两张 Mermaid 图正常渲染
- `bench.sh` 能一键出 csv + md 报告

---

## 🧪 M4+M5 端到端测试流程（接手立即可做的事）

所有代码已在本地 build 通过。P 服务器 build 也通过（M3 阶段）。下一步是：

```bash
# 1. 同步最新代码到 P
sshpass -p 'Senku/Vjwy+H' rsync -az \
  --exclude='build/' --exclude='build.zip' --exclude='dataset/' \
  --exclude='data/' --exclude='.git/' --exclude='third_party/libtorch/' \
  -e 'ssh -p 11568 -o StrictHostKeyChecking=no' \
  /home/ubuntu/gym/PLIN-Cloud-Edge-Device-Learned-Index/ \
  root@connect.westb.seetacloud.com:/root/gym/PLIN-Cloud-Edge-Device-Learned-Index/

# 2. 在 P 上训练两个模型（train.py 已修好）
ssh ...P
cd /root/gym/PLIN-Cloud-Edge-Device-Learned-Index
WLOG=/root/gym/PLIN-Cloud-Edge-Device-Learned-Index_odd/data/workload_log.csv
/root/miniconda3/bin/python3 hot_lstm/train.py --role cloud --data $WLOG --epochs 10
/root/miniconda3/bin/python3 hot_lstm/train.py --role end --end-id 1 --data $WLOG --epochs 10

# 3. 导出 TorchScript
/root/miniconda3/bin/python3 hot_lstm/export.py --role cloud
/root/miniconda3/bin/python3 hot_lstm/export.py --role end --end-id 1

# 4. 检查 libtorch 是否下完；如果没下完就等或用 miniconda fallback
ls third_party/libtorch/lib/libtorch.so 2>/dev/null \
  || echo "fallback to miniconda via CMakeLists.txt auto-detect"

# 5. 重新 cmake + build（应看到 libtorch 路径 message）
rm -rf build && cmake -B build -DCMAKE_BUILD_TYPE=Release 2>&1 | grep -i libtorch
cmake --build build -j$(nproc)

# 6. 跑起 2 edge + 10 end
bash scripts/run_all.sh \
    /root/gym/PLIN-Cloud-Edge-Device-Learned-Index_odd/dataset/Data.txt \
    common/topology.yaml

# 7. 检查日志
bash scripts/status_all.sh

# 期望看到：
#  - 2 个 edge_server 日志里 "PLIN built in Xs"、"listening on port 7101/7102"
#  - 10 个 end_node 日志里 "connected to edge"、"received PLIN_PARAM_PUSH"
#  - 自测 stage-① 1000/1000 OK
#  - 自测 stage-③ sibling key found=1 stage=3 OK
```

### 已知风险点
- PLIN 的 `bulk_load` 对 500 万 keys（5 个 End）可能要 10–30s 才能建索引完毕。Edge 启动时间会变长，`run_all.sh` 的 "grep listening port" 等待逻辑最多等 60s，应该够用。
- `libtorch` 还没跑过真实推理。`end_lstm_runner.cpp` 里只有 `torch::jit::load` + 一句 log，真正 `predict()` 逻辑要到 M6 阶段才会添加。
- End 的 `parent_plin_cache` 依赖 `cache_model.h` 里的 `local_model::predict_pos`，如果 PLIN 的参数序列化 / 反序列化 endian 不一致会报 `outer_size > 100 exit(0)`（`serialize.h:57`）。这个校验是原作者写的保护，出了问题要看一下 `serialize_parameter` 在本机和 P 是否同架构（都是 x86_64，应没事）。

---

## 📁 关键文件索引

| 文件 | 作用 | 状态 |
|---|---|---|
| `CMakeLists.txt` | 顶层，含 libtorch 三分支探测 | 用户加了 miniconda fallback |
| `common/proto.h` | `MsgType` 枚举 + `Status` | 完整 |
| `common/rpc.{h,cpp}` | 长度前缀帧读写 | 完整 |
| `common/range_map.{h,cpp}` | topology.yaml 解析 | 完整（key_t=double） |
| `common/topology.yaml` | 1 cloud + 2 edge + 10 end 静态拓扑 | 已按 Data.txt 真实 key 分段 |
| `common/loopback_test.cpp` | M2 RPC + 拓扑回环测试 | ✅ 通过 |
| `edge/edge_server.cpp` | **M4 主体**：PLIN bulk_load + TCP + `EDGE_FETCH_REQ` + `PLIN_PARAM_PUSH` | 代码完整，未端到端测试 |
| `end/end_node.cpp` | 四段式 lookup + 边连接 + 后台 receiver | stage-①/②/③ 完整；stage-④ stub；未测 |
| `end/hot_cache.h` | libcuckoo `cuckoohash_map<double, uint64_t>` | 完整 |
| `end/parent_plin_cache.h` | 包装 `cache_model.h`，反序列化 PLIN_PARAM_PUSH | 完整 |
| `end/end_lstm_runner{,_stub}.cpp` | libtorch 加载；真正 predict() 未写 | 骨架 |
| `cloud/cloud_server.cpp` | **M6 待写** | 只有 skeleton（打印启动信息后退出） |
| `cloud/cloud_lstm_runner{,_stub}.cpp` | **M6 待写** | 骨架 |
| `hot_lstm/model.py` | `LSTMPredictor` 共用模型类 | 完整 |
| `hot_lstm/train.py` | 训练入口 | 已修 CSV header 兜底 |
| `hot_lstm/export.py` | `torch.jit.script` 导 TorchScript | 完整 |
| `scripts/run_all.sh` | 起 2 edge + 10 end | 完整（Cloud 暂未加入启动） |
| `scripts/stop_all.sh` / `status_all.sh` | 停 / 查状态 | 完整 |
| `scripts/fetch_libtorch.sh` | 下载 libtorch CPU cxx11 ABI | 完整 |
| `doc/ARCHITECTURE.md` | 老 2 层架构文档，**M7 待重写** | 过时 |

---

## 🚦 接手顺序建议

1. **先收尾 M4+M5**（30 分钟左右）——把当前 bug 修完，跑通 2×edge + 10×end 的端到端测试，让 stage-① / ② / ③ 全部产生日志输出。
2. **再做 M6**（2–4 小时）——Cloud server + HOT_UPDATE 下发 + 跨边路由。参考上面具体 TODO。
3. **最后 M7**（1–2 小时）——文档 + benchmark。

建议每个小阶段都 rsync 回本机 + 跑一下 `build/common/loopback_test`，避免远端破了本机还在老版本。

---

## 记忆 / 用户偏好（从本次 session 学到的）

- 用户要求**加快速度**，能合并的里程碑合并做（M4+M5 就是合并推的）
- P 服务器密码 `Senku/Vjwy+H`（前一次曾经写成 `Senku/Vjwy+`，少了 H 导致认证失败，注意）
- P 上已有老版项目放在 `PLIN-Cloud-Edge-Device-Learned-Index_odd/` 作为备份，包含 `dataset/Data.txt` 和 `data/workload_log.csv`
- 用户倾向 **libtorch 嵌 C++**（不跑 Python 推理进程），训练仍在 Python 离线做
- 杀进程前要问用户（`memory/feedback_ask_before_kill.md`）
- 本次对话已写入多个 feedback memory（reference_servers / project_autofe_补跑 等），不需要重复记录
