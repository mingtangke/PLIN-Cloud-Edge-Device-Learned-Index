# PLIN-Cloud-Edge-Device-Learned-Index 🌤️🧠📦

[English Version](readme_en.md)

## 1. 项目简介 ✨

本项目实现了一个面向云-边-端协同场景的学习型索引系统：

- 索引层采用分段线性模型（Piecewise Linear Model）构建多层结构。
- 存储层在叶节点中融合了块组织与溢出树（B+ 树）能力。
- 在线阶段支持客户端本地模型预测 + 服务端纠偏查询。
- 运行中会记录访问日志，并通过 Python LSTM 预测热点设备，用于热键缓存更新。

核心目标是：在动态工作负载下，降低查询延迟并保持较高命中率。

---

## 2. 代码结构 🧩

### 2.1 核心 C++ 组件 ⚙️

- `plin_index.h`
	- 系统核心索引结构 `PlinIndex`。
	- 提供 `bulk_load`、`find_through_net`、`upsert`、`remove` 等关键能力。
	- 包含 split/rebuild 逻辑、并发控制和模型参数下发。

- `inner_node.h` / `leaf_node.h`
	- 分别实现内部节点与叶节点组织。
	- 叶节点支持数据块、局部模型和溢出管理。

- `piecewise_linear_model.h`
	- 引入了最优分段线性拟合算法（来自 PGM 思路），用于构建模型段。

- `b_plus.h`
	- 提供 B+ 树页结构、插入、查询、删除及重分配相关逻辑。

- `cache_model.h`
	- 客户端本地模型定义，接收服务端参数后执行位置预测。

- `hot_key.h`
	- `DatabaseLogger` 负责日志聚合、热点统计、与 Python 训练服务通信。
	- 通过 `libcuckoo` 并发哈希维护热键缓存。

### 2.2 网络与程序入口 🌐

- `server.cpp`
	- 监听 TCP 端口（默认 8888）。
	- 加载 `Data.txt` 后执行 `bulk_load`。
	- 处理客户端 `findid` / `cache` 请求。

- `client.cpp`
	- 加载 `Data.txt` 和工作负载 CSV。
	- 先请求一次缓存参数，再按行发起 `findid`。
	- 接收 `Failure!` 或 `Update cache!` 响应并更新本地模型。

- `test_demo.cpp`
	- 生成批量数据（如 `Data2.txt`）。

- `device_generator.cpp`
	- 生成带时间戳、设备 ID 的 CSV 工作负载（Zipf 分布）。

- `ycsb_generator.cpp`
	- 生成 YCSB 风格 workload 文本。

### 2.3 Python 训练与预测 🐍

- `new_train.py`
	- 设备访问序列预测（LSTM）。
	- 与 C++ 通信端口默认 60001。
	- 接收 `INDEX:start:end` / `ADJUST:start:end` 指令并回传 `DEVICES:x,yEND`。

- `train.py`
	- 热键序列预测训练脚本（可通信版）。

- `hot_lstm/lstm.py`
	- 离线 LSTM 训练/评估入口（更偏实验分析）。

---

## 3. 运行环境 🛠️

本仓库 C++ 代码依赖 POSIX 头文件（如 `netinet/in.h`, `sys/socket.h`, `unistd.h`）。

- 推荐环境：Linux 或 WSL2 (Ubuntu)
- 编译器：g++（支持 C++20）
- 构建工具：CMake >= 3.16
- Python：3.8+
- 深度学习库：PyTorch

Windows 原生 MSVC 环境无法直接编译当前网络代码，建议在 WSL 中运行。

---

## 4. 依赖 📦

### 4.1 C++ 依赖 🔧

- pthread
- libcuckoo（仓库已包含 `libcuckoo/`）

### 4.2 Python 依赖（按脚本导入） 🧪

建议安装：

- torch
- numpy
- pandas
- scikit-learn
- matplotlib

可用命令：

```bash
pip install torch numpy pandas scikit-learn matplotlib
```

---

## 5. 数据与文件约定 🗂️

### 5.1 初始加载数据

- `Data.txt`
	- 服务端启动时读取。
	- 每行格式：

```text
<key> <payload>
```

### 5.2 工作负载日志

- `data/workload_log.csv`（或你自行指定的路径）
	- CSV 表头：

```text
timestamp,device_id,key,operation
```

说明：当前客户端会把第三列当作数据位置索引（target_pos）再映射到 `Data.txt` 的 key。

---

## 6. 快速开始 🚀

以下步骤默认在项目根目录执行。

### 6.1 构建 server/client 🏗️

```bash
mkdir -p build
cd build
cmake ..
cmake --build . -j
```

生成可执行文件：

- `s`（服务端）
- `c`（客户端）

### 6.2 准备数据 🧱

确保根目录存在：

- `Data.txt`
- 工作负载 CSV（例如 `data/workload_log.csv`）

如果你需要生成数据，可单独编译：

```bash
g++ -std=c++20 -O2 test_demo.cpp -o gen_data
./gen_data

g++ -std=c++20 -O2 device_generator.cpp -o gen_workload
./gen_workload
```

### 6.3 启动 Python 预测服务 🔮

在单独终端运行：

```bash
python3 new_train.py
```

注意：服务端在构造 `PlinIndex` 时会尝试连接 Python 端口 60001，如果 Python 服务未启动，C++ 端会连接失败。

### 6.4 启动服务端 🖥️

```bash
cd build
./s
```

### 6.5 启动客户端 💻

```bash
cd build
./c
```

客户端当前使用源码里硬编码的 workload 路径，请先按第 7 节修改为你的本地路径。

---

## 7. 首次运行前必须检查的硬编码路径 ⚠️

仓库中存在多处历史开发环境路径，请根据你的机器修改：

- `plin_index.h`
	- `DatabaseLogger` 构造时的日志文件路径（当前为 Linux 绝对路径）。

- `client.cpp`
	- `test()` 中默认 workload 文件路径。
	- `write_error_rates_to_file()` 输出路径。

- `new_train.py`
	- `self.log_file` 默认日志路径。

- `train.py`
	- `self.log_file` 默认日志路径。

- `hot_key.cpp`
	- 读取日志的测试路径。

建议统一改为项目相对路径，例如：

- `data/workload_log.csv`
- `build/train_result.txt`

---

## 8. 在线交互流程 🔄

1. 服务端加载 `Data.txt` 并建立 PLIN 索引。
2. 客户端请求 `cache`，拉取服务端模型参数。
3. 客户端按工作负载发送 `findid timestamp device_id target_pos logic_id`。
4. 服务端先查热点缓存，未命中则走 `find_through_net`。
5. 服务端将成功查询写入日志队列，并根据策略触发 Python 预测。
6. Python 返回预测设备后，服务端更新热键缓存。
7. 若结构重建发生，客户端收到 `Update cache!` 并重新拉参。

---

## 9. 关键参数 🎛️

在 `parameters.h` 可调整：

- `EPSILON_LEAF_NODE` / `EPSILON_INNER_NODE`
- `LEAF_NODE_INIT_RATIO` / `INNER_NODE_INIT_RATIO`
- `MAX_OVERFLOW_RATIO` / `MAX_ORPHAN_RATIO`
- `BLOCK_SIZE`

在 `net_parameter.h` 可调整：

- `PORT_DEFAULT`
- `TEST_THREAD`
- `DATA_NUM`

在 `hot_key.h` 可调整：

- `HOT_CACHE`
- `HOT_KEY_NUM`
- `CACHE_RETRAIN_NUM`
- `CACHE_RETRAIN_RATE`

---

## 10. 常见问题 ❓

### Q1: 服务端启动后立刻退出，报 Python 连接错误

请先运行 `new_train.py`，确认监听 `127.0.0.1:60001`。

### Q2: 客户端提示 workload 打不开

检查 `client.cpp` 中 `test()` 的硬编码路径是否已改为你自己的 CSV 路径。

### Q3: Windows 下直接编译报 POSIX 头文件错误

请在 WSL2/Ubuntu 编译运行，或自行进行跨平台网络层改造。

### Q4: 查询结果误差率波动较大

检查：

- 数据分布是否与训练阶段一致
- 热键参数（如 `HOT_KEY_NUM`）是否过小
- 日志窗口与触发阈值（`CACHE_RETRAIN_NUM`）是否合理

---

## 11. 已知限制 🚧

- 当前默认流程高度依赖硬编码路径，需要先做本地化修改。
- CMake 默认只编译 `server.cpp` 和 `client.cpp`，其他工具程序需手动编译。
- 训练与服务通信端口固定在代码中（默认 60001）。
- 目前未提供完整的跨平台网络适配和一键脚本。

---

## 12. 参考 📚

- `piecewise_linear_model.h` 中的分段模型实现基于 PGM-index 思想（文件头包含来源说明）。

