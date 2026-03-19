# PLIN-Cloud-Edge-Device-Learned-Index 🌤️🧠📦

[中文版本](readme.md)

## 1. Project Overview ✨

This project implements a learned index system for a cloud-edge-device collaborative scenario:

- The indexing layer uses Piecewise Linear Models to build a multi-level structure.
- The storage layer combines block organization and overflow-tree (B+ tree) capability in leaf nodes.
- The online stage supports client-side local prediction plus server-side correction lookup.
- During runtime, access logs are collected, and a Python LSTM predicts hot devices for hot-key cache updates.

The core goal is to reduce query latency while maintaining a high hit rate under dynamic workloads.

---

## 2. Code Structure 🧩

### 2.1 Core C++ Components ⚙️

- plin_index.h
  - Core index structure, PlinIndex.
  - Provides key capabilities such as bulk_load, find_through_net, upsert, and remove.
  - Includes split/rebuild logic, concurrency control, and model-parameter distribution.

- inner_node.h / leaf_node.h
  - Implement internal nodes and leaf nodes respectively.
  - Leaf nodes support data blocks, local models, and overflow management.

- piecewise_linear_model.h
  - Uses an optimal piecewise linear fitting algorithm (PGM-style) to construct model segments.

- b_plus.h
  - Provides B+ tree page structure, insertion, lookup, deletion, and redistribution logic.

- cache_model.h
  - Defines the client-side local model, used for position prediction after receiving server parameters.

- hot_key.h
  - DatabaseLogger handles log aggregation, hot-key statistics, and communication with the Python training service.
  - Uses libcuckoo concurrent hash map for hot-key cache maintenance.

### 2.2 Networking and Program Entrypoints 🌐

- server.cpp
  - Listens on TCP port 8888 by default.
  - Loads Data.txt and performs bulk_load.
  - Handles client findid and cache requests.

- client.cpp
  - Loads Data.txt and workload CSV.
  - Requests cache parameters first, then sends findid line by line.
  - Receives Failure! or Update cache! and updates local model.

- test_demo.cpp
  - Generates bulk data (for example Data2.txt).

- device_generator.cpp
  - Generates timestamped CSV workloads with device IDs (Zipf distribution).

- ycsb_generator.cpp
  - Generates YCSB-style workload text.

### 2.3 Python Training and Prediction 🐍

- new_train.py
  - LSTM predictor for device-access sequences.
  - Communicates with C++ on port 60001 by default.
  - Accepts INDEX:start:end and ADJUST:start:end, then replies with DEVICES:x,yEND.

- train.py
  - Hot-key sequence prediction training script (communication-enabled version).

- hot_lstm/lstm.py
  - Offline LSTM training/evaluation entrypoint (more experiment-oriented).

---

## 3. Runtime Environment 🛠️

This repository relies on POSIX headers in C++ (such as netinet/in.h, sys/socket.h, and unistd.h).

- Recommended environment: Linux or WSL2 (Ubuntu)
- Compiler: g++ (with C++20 support)
- Build tool: CMake >= 3.16
- Python: 3.8+
- Deep learning library: PyTorch

Native MSVC on Windows cannot compile the current networking code directly. Running in WSL is recommended.

---

## 4. Dependencies 📦

### 4.1 C++ Dependencies 🔧

- pthread
- libcuckoo (already included in libcuckoo/)

### 4.2 Python Dependencies (from script imports) 🧪

Recommended packages:

- torch
- numpy
- pandas
- scikit-learn
- matplotlib

Install command:

bash
pip install torch numpy pandas scikit-learn matplotlib

---

## 5. Data and File Conventions 🗂️

### 5.1 Initial Bulk-Load Data

- Data.txt
  - Read by the server at startup.
  - Per-line format:

text
<key> <payload>

### 5.2 Workload Log

- data/workload_log.csv (or your own path)
  - CSV header:

text
timestamp,device_id,key,operation

Note: the current client treats the third CSV column as a position index (target_pos), then maps it to keys from Data.txt.

---

## 6. Quick Start 🚀

All commands below are expected to run in the project root.

### 6.1 Build server/client 🏗️

bash
mkdir -p build
cd build
cmake ..
cmake --build . -j

Generated executables:

- s (server)
- c (client)

### 6.2 Prepare Data 🧱

Make sure the following files exist:

- Data.txt
- Workload CSV (for example data/workload_log.csv)

If needed, you can compile data generators manually:

bash
g++ -std=c++20 -O2 test_demo.cpp -o gen_data
./gen_data

g++ -std=c++20 -O2 device_generator.cpp -o gen_workload
./gen_workload

### 6.3 Start Python Prediction Service 🔮

Run in a separate terminal:

bash
python3 new_train.py

Important: when PlinIndex is constructed, the server tries to connect to Python on port 60001. If Python is not running, C++ side connection will fail.

### 6.4 Start Server 🖥️

bash
cd build
./s

### 6.5 Start Client 💻

bash
cd build
./c

The client currently uses a hardcoded workload path in source code. Please update it first (see Section 7).

---

## 7. Hardcoded Paths to Check Before First Run ⚠️

There are several historical absolute paths in the repository. Update them for your machine:

- plin_index.h
  - Log-file path passed into DatabaseLogger constructor.

- client.cpp
  - Default workload path in test().
  - Output path in write_error_rates_to_file().

- new_train.py
  - Default self.log_file path.

- train.py
  - Default self.log_file path.

- hot_key.cpp
  - Test log-reading path.

Recommended relative paths:

- data/workload_log.csv
- build/train_result.txt

---

## 8. Online Interaction Flow 🔄

1. The server loads Data.txt and builds the PLIN index.
2. The client requests cache parameters.
3. The client sends findid timestamp device_id target_pos logic_id for workload entries.
4. The server checks hot-key cache first, then falls back to find_through_net on miss.
5. Successful queries are logged, and Python prediction is triggered by policy.
6. Python returns predicted devices, then the server refreshes hot-key cache.
7. If structural rebuild happens, the client receives Update cache! and pulls parameters again.

---

## 9. Key Parameters 🎛️

Adjust in parameters.h:

- EPSILON_LEAF_NODE / EPSILON_INNER_NODE
- LEAF_NODE_INIT_RATIO / INNER_NODE_INIT_RATIO
- MAX_OVERFLOW_RATIO / MAX_ORPHAN_RATIO
- BLOCK_SIZE

Adjust in net_parameter.h:

- PORT_DEFAULT
- TEST_THREAD
- DATA_NUM

Adjust in hot_key.h:

- HOT_CACHE
- HOT_KEY_NUM
- CACHE_RETRAIN_NUM
- CACHE_RETRAIN_RATE

---

## 10. FAQ ❓

### Q1: Server exits immediately with Python connection error

Start new_train.py first and confirm listener on 127.0.0.1:60001.

### Q2: Client cannot open workload file

Check whether the hardcoded path in client.cpp test() has been changed to your local CSV path.

### Q3: POSIX header errors when building on Windows

Build and run in WSL2/Ubuntu, or perform your own cross-platform networking refactor.


