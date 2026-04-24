#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "cloud_lstm_runner.h"
#include "common/proto.h"
#include "common/range_map.h"
#include "common/rpc.h"

using _key_t = double;
using _payload_t = uint64_t;

namespace {

struct CloudArgs {
    std::string topology_path = "src/common/topology.yaml";
    std::string data_path = "Data.txt";
    std::string workload_path = "data/workload_log.csv";
    std::string model_path = "hot_lstm/models/cloud_lstm.pt";
    int hot_interval_sec = 30;
    int hot_topn = 32;
    int predict_k = 3;
};

struct DataStore {
    std::vector<_key_t> keys;
    std::vector<_payload_t> payloads;
    std::unordered_map<_key_t, _payload_t> by_key;
};

struct WorkloadRecord {
    double timestamp = 0;
    int device_id = 0;
    int key_pos = 0;
};

struct EdgeConn {
    int edge_id = 0;
    int fd = -1;
    std::mutex mu;
};

CloudArgs parse_args(int argc, char** argv) {
    CloudArgs a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if (k == "--topology" && i + 1 < argc) a.topology_path = argv[++i];
        else if (k == "--data" && i + 1 < argc) a.data_path = argv[++i];
        else if (k == "--workload" && i + 1 < argc) a.workload_path = argv[++i];
        else if (k == "--model" && i + 1 < argc) a.model_path = argv[++i];
        else if (k == "--hot-interval" && i + 1 < argc) a.hot_interval_sec = std::stoi(argv[++i]);
        else if (k == "--hot-topn" && i + 1 < argc) a.hot_topn = std::stoi(argv[++i]);
        else if (k == "--predict-k" && i + 1 < argc) a.predict_k = std::stoi(argv[++i]);
    }
    return a;
}

void append_u32(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t raw[4];
    std::memcpy(raw, &v, 4);
    b.insert(b.end(), raw, raw + 4);
}

void append_u64(std::vector<uint8_t>& b, uint64_t v) {
    uint8_t raw[8];
    std::memcpy(raw, &v, 8);
    b.insert(b.end(), raw, raw + 8);
}

void append_double(std::vector<uint8_t>& b, double v) {
    uint8_t raw[8];
    std::memcpy(raw, &v, 8);
    b.insert(b.end(), raw, raw + 8);
}

bool read_u32(const std::vector<uint8_t>& b, size_t off, uint32_t& out) {
    if (off + 4 > b.size()) return false;
    std::memcpy(&out, &b[off], 4);
    return true;
}

bool read_double(const std::vector<uint8_t>& b, size_t off, double& out) {
    if (off + 8 > b.size()) return false;
    std::memcpy(&out, &b[off], 8);
    return true;
}

DataStore load_data(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "[cloud] cannot open data " << path << "\n";
        return {};
    }
    DataStore ds;
    ds.keys.reserve(10000000);
    ds.payloads.reserve(10000000);
    _key_t k;
    _payload_t p;
    while (in >> k >> p) {
        ds.keys.push_back(k);
        ds.payloads.push_back(p);
    }
    ds.by_key.reserve(ds.keys.size() * 2);
    for (size_t i = 0; i < ds.keys.size(); ++i) {
        ds.by_key.emplace(ds.keys[i], ds.payloads[i]);
    }
    return ds;
}

std::vector<WorkloadRecord> load_workload(const std::string& path) {
    std::ifstream in(path);
    if (!in) {
        std::cerr << "[cloud] cannot open workload " << path << "\n";
        return {};
    }
    std::vector<WorkloadRecord> rows;
    std::string line;
    std::getline(in, line);
    while (std::getline(in, line)) {
        if (line.empty() || line.rfind("timestamp", 0) == 0) continue;
        std::stringstream ss(line);
        std::string tok;
        WorkloadRecord r;
        if (!std::getline(ss, tok, ',')) continue;
        if (tok == "timestamp") continue;
        r.timestamp = std::stod(tok);
        if (!std::getline(ss, tok, ',')) continue;
        r.device_id = std::stoi(tok);
        if (!std::getline(ss, tok, ',')) continue;
        r.key_pos = std::stoi(tok);
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.timestamp < b.timestamp; });
    return rows;
}

std::vector<std::vector<float>> build_counts_60x10(const std::vector<WorkloadRecord>& rows) {
    std::vector<std::vector<float>> counts(60, std::vector<float>(10, 0.0f));
    if (rows.empty()) return counts;
    double min_ts = rows.front().timestamp;
    double max_ts = rows.back().timestamp;
    double span = std::max(1e-9, max_ts - min_ts);
    for (const auto& r : rows) {
        int bin = static_cast<int>(((r.timestamp - min_ts) / span) * 60.0);
        if (bin < 0) bin = 0;
        if (bin > 59) bin = 59;
        if (r.device_id >= 1 && r.device_id <= 10) {
            counts[static_cast<size_t>(bin)][static_cast<size_t>(r.device_id - 1)] += 1.0f;
        }
    }
    return counts;
}

int end_for_position(int key_pos, size_t data_size) {
    if (key_pos <= 0 || static_cast<size_t>(key_pos) > data_size) return -1;
    int end_id = (key_pos - 1) / 1000000 + 1;
    if (end_id < 1) return -1;
    if (end_id > 10) end_id = 10;
    return end_id;
}

std::vector<std::vector<int>> build_hot_positions_by_end(
    const std::vector<WorkloadRecord>& rows, size_t data_size, int max_per_end) {
    std::vector<std::unordered_map<int, int>> counts(11);
    for (const auto& r : rows) {
        int end_id = end_for_position(r.key_pos, data_size);
        if (end_id >= 1 && end_id <= 10) {
            counts[static_cast<size_t>(end_id)][r.key_pos] += 1;
        }
    }

    std::vector<std::vector<int>> out(11);
    for (int end_id = 1; end_id <= 10; ++end_id) {
        std::vector<std::pair<int, int>> ranked;
        ranked.reserve(counts[static_cast<size_t>(end_id)].size());
        for (const auto& [pos, cnt] : counts[static_cast<size_t>(end_id)]) {
            ranked.push_back({cnt, pos});
        }
        std::sort(ranked.begin(), ranked.end(),
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (int i = 0; i < max_per_end && i < static_cast<int>(ranked.size()); ++i) {
            out[static_cast<size_t>(end_id)].push_back(ranked[static_cast<size_t>(i)].second);
        }
    }
    return out;
}

plin::rpc::Frame make_fetch_resp(plin::proto::Status status, _payload_t payload = 0) {
    plin::rpc::Frame resp;
    resp.type = plin::proto::MsgType::EDGE_FETCH_RESP;
    resp.body.push_back(static_cast<uint8_t>(status));
    append_u64(resp.body, payload);
    resp.body.push_back(0);
    return resp;
}

class CloudServer {
 public:
    CloudServer(CloudArgs args, plin::RangeMap rm, DataStore data,
                std::vector<WorkloadRecord> workload)
        : args_(std::move(args)),
          rm_(std::move(rm)),
          data_(std::move(data)),
          workload_(std::move(workload)),
          runner_(args_.model_path) {
        counts_ = build_counts_60x10(workload_);
        hot_positions_ = build_hot_positions_by_end(workload_, data_.keys.size(), args_.hot_topn * 4);
    }

    void run() {
        std::thread(&CloudServer::hot_update_loop, this).detach();

        int listen_fd = ::socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1;
        ::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(rm_.cloud().port);
        if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            perror("[cloud] bind");
            return;
        }
        ::listen(listen_fd, 32);
        std::cout << "[cloud_server] listening on port " << rm_.cloud().port << "...\n";

        while (true) {
            int cli = ::accept(listen_fd, nullptr, nullptr);
            if (cli < 0) {
                perror("[cloud] accept");
                continue;
            }
            std::thread(&CloudServer::handle_connection, this, cli).detach();
        }
    }

 private:
    void register_edge(int edge_id, int fd) {
        auto conn = std::make_shared<EdgeConn>();
        conn->edge_id = edge_id;
        conn->fd = fd;
        {
            std::lock_guard<std::mutex> lk(edges_mu_);
            edges_[edge_id] = conn;
        }
        plin::rpc::Frame ack;
        ack.type = plin::proto::MsgType::HEARTBEAT_ACK;
        append_u32(ack.body, static_cast<uint32_t>(edge_id));
        plin::rpc::write_frame(fd, ack);
        std::cout << "[cloud_server] registered edge " << edge_id << "\n";
    }

    std::shared_ptr<EdgeConn> edge_conn(int edge_id) {
        std::lock_guard<std::mutex> lk(edges_mu_);
        auto it = edges_.find(edge_id);
        if (it == edges_.end()) return nullptr;
        return it->second;
    }

    bool forward_to_edge(int edge_id, const plin::rpc::Frame& req, plin::rpc::Frame& resp) {
        auto conn = edge_conn(edge_id);
        if (!conn) return false;
        std::lock_guard<std::mutex> lk(conn->mu);
        if (!plin::rpc::write_frame(conn->fd, req)) return false;
        return plin::rpc::read_frame(conn->fd, resp);
    }

    void handle_cross_edge_req(int fd, const plin::rpc::Frame& req) {
        double key = 0;
        if (!read_double(req.body, 8, key)) {
            auto resp = make_fetch_resp(plin::proto::Status::ERROR);
            plin::rpc::write_frame(fd, resp);
            return;
        }
        int target_end = rm_.locate_end(key);
        int target_edge = rm_.edge_of(target_end);
        if (target_end < 0 || target_edge < 0) {
            auto resp = make_fetch_resp(plin::proto::Status::NOT_FOUND);
            plin::rpc::write_frame(fd, resp);
            return;
        }

        plin::rpc::Frame edge_resp;
        if (!forward_to_edge(target_edge, req, edge_resp)) {
            auto resp = make_fetch_resp(plin::proto::Status::ERROR);
            plin::rpc::write_frame(fd, resp);
            std::cerr << "[cloud_server] cross-edge forward failed edge=" << target_edge << "\n";
            return;
        }
        plin::rpc::write_frame(fd, edge_resp);
        std::cout << "[cloud_server] CROSS_EDGE key=" << key
                  << " target_end=" << target_end
                  << " target_edge=" << target_edge << "\n";
    }

    void handle_connection(int fd) {
        plin::rpc::Frame first;
        if (!plin::rpc::read_frame(fd, first)) {
            ::close(fd);
            return;
        }

        if (first.type == plin::proto::MsgType::HEARTBEAT) {
            uint32_t edge_id = 0;
            if (read_u32(first.body, 0, edge_id)) {
                register_edge(static_cast<int>(edge_id), fd);
                return;  // fd ownership stays in edges_.
            }
        } else if (first.type == plin::proto::MsgType::CROSS_EDGE_REQ) {
            handle_cross_edge_req(fd, first);
        }
        ::close(fd);
    }

    bool send_hot_update(int target_end_id) {
        int edge_id = rm_.edge_of(target_end_id);
        auto conn = edge_conn(edge_id);
        if (!conn) return false;
        if (target_end_id < 1 || target_end_id >= static_cast<int>(hot_positions_.size())) return false;

        plin::rpc::Frame f;
        f.type = plin::proto::MsgType::HOT_UPDATE;
        append_u32(f.body, static_cast<uint32_t>(target_end_id));

        int emitted = 0;
        for (int pos : hot_positions_[static_cast<size_t>(target_end_id)]) {
            if (pos <= 0 || static_cast<size_t>(pos) > data_.keys.size()) continue;
            _key_t key = data_.keys[static_cast<size_t>(pos - 1)];
            if (rm_.locate_end(key) != target_end_id) continue;
            append_double(f.body, key);
            append_u64(f.body, data_.payloads[static_cast<size_t>(pos - 1)]);
            if (++emitted >= args_.hot_topn) break;
        }
        if (emitted == 0) return false;

        std::lock_guard<std::mutex> lk(conn->mu);
        bool ok = plin::rpc::write_frame(conn->fd, f);
        if (ok) {
            std::cout << "[cloud_server] HOT_UPDATE target_end=" << target_end_id
                      << " edge=" << edge_id << " keys=" << emitted << "\n";
        }
        return ok;
    }

    void hot_update_loop() {
        std::this_thread::sleep_for(std::chrono::seconds(8));
        while (true) {
            std::vector<int> top = runner_.predict_top_ends(counts_, args_.predict_k);
            if (top.empty()) {
                top = {0, 1, 5};
            }
            for (int idx : top) {
                int end_id = idx + 1;
                if (end_id >= 1 && end_id <= 10) {
                    send_hot_update(end_id);
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(args_.hot_interval_sec));
        }
    }

    CloudArgs args_;
    plin::RangeMap rm_;
    DataStore data_;
    std::vector<WorkloadRecord> workload_;
    CloudLstmRunner runner_;
    std::vector<std::vector<float>> counts_;
    std::vector<std::vector<int>> hot_positions_;
    std::mutex edges_mu_;
    std::unordered_map<int, std::shared_ptr<EdgeConn>> edges_;
};

}  // namespace

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    auto args = parse_args(argc, argv);
    std::cout << "[cloud_server] topology=" << args.topology_path
              << " data=" << args.data_path
              << " workload=" << args.workload_path
              << " model=" << args.model_path << "\n";

    plin::RangeMap rm;
    if (!rm.load(args.topology_path)) return 1;

    auto data = load_data(args.data_path);
    if (data.keys.empty()) return 1;
    std::cout << "[cloud_server] loaded data keys=" << data.keys.size() << "\n";

    auto workload = load_workload(args.workload_path);
    if (workload.empty()) return 1;
    std::cout << "[cloud_server] loaded workload rows=" << workload.size() << "\n";

    try {
        CloudServer server(args, std::move(rm), std::move(data), std::move(workload));
        server.run();
    } catch (const std::exception& e) {
        std::cerr << "[cloud_server] fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
