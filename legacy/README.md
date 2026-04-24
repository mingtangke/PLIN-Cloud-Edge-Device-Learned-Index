# legacy/

Archive of the original edge–end two-layer implementation. Kept for reference only;
**not** built by the current CMake. The active three-layer architecture lives under
`cloud/`, `edge/`, `end/`, `common/`.

| File | Old role |
|---|---|
| `server.cpp` | Old "edge" server: PLIN `bulk_load`, port 9999, libcuckoo `hot_map_`, Python IPC |
| `client.cpp` | Old "end" client: replays `workload_log.csv`, uses `cache_model.h` locally |
| `new_train.py` | Old LSTM hot-device predictor (Python TCP server on port 60001) |
| `hot_key.{h,cpp}` | Frequency tracking + libcuckoo `hot_map_` population |
| `lstm_server.cpp`, `lstm_client.cpp` | Unused stubs from original repo |
| `test_demo.cpp`, `ycsb_generator.cpp`, `train.py`, `lstm_predictor.pth` | Old experiments / checkpoints |

New architecture migration:
- PLIN + `bulk_load` logic → `edge/edge_server.cpp`
- Python LSTM → libtorch in-process: `cloud/cloud_lstm_runner.*`, `end/end_lstm_runner.*`
- Training stays in Python but moves to `hot_lstm/train.py` + `hot_lstm/export.py`
