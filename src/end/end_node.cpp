#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <tlx/container/btree_map.hpp>

#include "common/proto.h"
#include "common/range_map.h"
#include "common/rdma_snapshot.h"
#include "common/rpc.h"
#include "common/transport.h"
#include "hot_cache.h"
#include "parent_plin_cache.h"

using _key_t     = double;
using _payload_t = uint64_t;

void end_lstm_runner_init(const std::string& model_path);
struct EndRdmaSnapshot;
static void apply_edge_control_frame(const plin::rpc::Frame& f,
                                     plin::end::ParentPlinCache& ppc,
                                     plin::end::HotCache& hc,
                                     EndRdmaSnapshot* rdma_snapshot = nullptr);

static void append_u32(std::vector<uint8_t>& b, uint32_t v) {
    uint8_t raw[4];
    std::memcpy(raw, &v, 4);
    b.insert(b.end(), raw, raw + 4);
}

static void append_u64(std::vector<uint8_t>& b, uint64_t v) {
    uint8_t raw[8];
    std::memcpy(raw, &v, 8);
    b.insert(b.end(), raw, raw + 8);
}

// ── local B+ tree load ────────────────────────────────────────────────────────

struct LocalStore {
    tlx::btree_map<_key_t, _payload_t> btree;
    size_t loaded = 0;
};

static LocalStore load_range(const std::string& path, _key_t lo, _key_t hi) {
    LocalStore s;
    std::ifstream in(path);
    if (!in) { std::cerr << "[end_node] cannot open " << path << "\n"; return s; }
    _key_t k; _payload_t p;
    while (in >> k >> p) {
        if (k >= lo && k <= hi) { s.btree.insert({k, p}); ++s.loaded; }
    }
    return s;
}

static bool find_first_key_in_range(const std::string& path, _key_t lo, _key_t hi,
                                    _key_t& out) {
    std::ifstream in(path);
    if (!in) return false;
    _key_t k;
    _payload_t p;
    while (in >> k >> p) {
        if (k >= lo && k <= hi) {
            out = k;
            return true;
        }
        if (k > hi) break;
    }
    return false;
}

static std::string default_model_path_for(int end_id) {
    namespace fs = std::filesystem;
    std::string per_end = "hot_lstm/models/end_lstm_" + std::to_string(end_id) + ".pt";
    if (fs::exists(per_end)) return per_end;
    if (fs::exists("hot_lstm/models/end_lstm_1.pt")) return "hot_lstm/models/end_lstm_1.pt";
    return "hot_lstm/models/end_lstm.pt";
}

// ── edge transport helpers ───────────────────────────────────────────────────

struct EndRdmaSnapshot {
    plin::rdma::SnapshotInfo info;
    bool loaded = false;
};

enum class RDMALookupStatus {
    HIT,
    MISS,
    UNAVAILABLE,
};

// Send EDGE_FETCH_REQ, receive EDGE_FETCH_RESP.
// Returns true on success; payload written to out.
static bool read_fetch_response(plin::transport::Transport& edge,
                                plin::end::ParentPlinCache& ppc,
                                plin::end::HotCache& hc,
                                EndRdmaSnapshot& rdma_snapshot,
                                _payload_t& out, bool& param_stale) {
    plin::rpc::Frame resp;
    while (edge.read_frame(resp)) {
        if (resp.type == plin::proto::MsgType::EDGE_FETCH_RESP) {
            if (resp.body.size() < 9) return false;
            auto status = static_cast<plin::proto::Status>(resp.body[0]);
            std::memcpy(&out, &resp.body[1], 8);
            param_stale = (resp.body.size() > 9 && resp.body[9] != 0);
            return status == plin::proto::Status::OK;
        }
        if (resp.type == plin::proto::MsgType::PLIN_PARAM_PUSH ||
            resp.type == plin::proto::MsgType::HOT_UPDATE ||
            resp.type == plin::proto::MsgType::RDMA_SNAPSHOT_INFO) {
            apply_edge_control_frame(resp, ppc, hc, &rdma_snapshot);
            continue;
        }
        std::cerr << "[end_node] unexpected frame while waiting fetch resp type="
                  << static_cast<int>(resp.type) << "\n";
        return false;
    }
    return false;
}

static RDMALookupStatus rdma_snapshot_lookup(plin::transport::Transport& edge,
                                             const EndRdmaSnapshot& snapshot,
                                             _key_t key, int predicted_slot,
                                             _payload_t& out) {
    if (!snapshot.loaded || !edge.supports_remote_read()) {
        return RDMALookupStatus::UNAVAILABLE;
    }
    if (snapshot.info.magic != plin::rdma::kSnapshotMagic ||
        snapshot.info.leaf_count == 0 ||
        predicted_slot < 0) {
        return RDMALookupStatus::UNAVAILABLE;
    }

    uint32_t leaf_id = static_cast<uint32_t>(predicted_slot);
    if (leaf_id >= snapshot.info.leaf_count) leaf_id = snapshot.info.leaf_count - 1;

    plin::rdma::LeafDescriptor desc;
    uint64_t desc_addr = snapshot.info.desc_addr
                       + static_cast<uint64_t>(leaf_id) * sizeof(desc);
    if (!edge.read_remote(desc_addr, snapshot.info.desc_rkey, &desc, sizeof(desc))) {
        return RDMALookupStatus::UNAVAILABLE;
    }
    if (desc.version != snapshot.info.version || desc.count == 0) {
        return RDMALookupStatus::UNAVAILABLE;
    }
    if (desc.offset + desc.count > snapshot.info.record_count) {
        return RDMALookupStatus::UNAVAILABLE;
    }

    std::vector<plin::rdma::KeyPayloadRecord> records(desc.count);
    uint64_t record_addr = snapshot.info.record_addr
                         + desc.offset * sizeof(plin::rdma::KeyPayloadRecord);
    size_t bytes = records.size() * sizeof(plin::rdma::KeyPayloadRecord);
    if (!edge.read_remote(record_addr, snapshot.info.record_rkey, records.data(), bytes)) {
        return RDMALookupStatus::UNAVAILABLE;
    }
    for (const auto& rec : records) {
        if (rec.key == key) {
            out = rec.payload;
            return RDMALookupStatus::HIT;
        }
    }
    return RDMALookupStatus::MISS;
}

static bool edge_fetch(plin::transport::Transport& edge, std::mutex& edge_mu,
                       plin::end::ParentPlinCache& ppc,
                       plin::end::HotCache& hc,
                       EndRdmaSnapshot& rdma_snapshot,
                       _key_t key, int predicted_slot,
                       _payload_t& out, bool& param_stale) {
    plin::rpc::Frame req;
    req.type = plin::proto::MsgType::EDGE_FETCH_REQ;
    // 8 bytes key (raw bytes, same endianness as edge), 4 bytes slot
    uint8_t kbuf[8]; std::memcpy(kbuf, &key, 8);
    req.body.insert(req.body.end(), kbuf, kbuf + 8);
    int32_t slot = static_cast<int32_t>(predicted_slot);
    uint8_t sbuf[4]; std::memcpy(sbuf, &slot, 4);
    req.body.insert(req.body.end(), sbuf, sbuf + 4);

    std::lock_guard<std::mutex> lk(edge_mu);
    if (!edge.write_frame(req)) return false;

    return read_fetch_response(edge, ppc, hc, rdma_snapshot, out, param_stale);
}

static bool cross_edge_fetch(plin::transport::Transport& edge, std::mutex& edge_mu,
                             plin::end::ParentPlinCache& ppc,
                             plin::end::HotCache& hc,
                             EndRdmaSnapshot& rdma_snapshot,
                             _key_t key, _payload_t& out) {
    static std::atomic<uint64_t> next_request_id{1};

    plin::rpc::Frame req;
    req.type = plin::proto::MsgType::CROSS_EDGE_REQ;
    append_u64(req.body, next_request_id.fetch_add(1));
    uint8_t kbuf[8];
    std::memcpy(kbuf, &key, 8);
    req.body.insert(req.body.end(), kbuf, kbuf + 8);

    std::lock_guard<std::mutex> lk(edge_mu);
    if (!edge.write_frame(req)) return false;

    bool stale = false;
    return read_fetch_response(edge, ppc, hc, rdma_snapshot, out, stale);
}

static bool send_end_heartbeat(plin::transport::Transport& edge, std::mutex& edge_mu, int end_id) {
    plin::rpc::Frame hb;
    hb.type = plin::proto::MsgType::HEARTBEAT;
    append_u32(hb.body, static_cast<uint32_t>(end_id));
    std::lock_guard<std::mutex> lk(edge_mu);
    return edge.write_frame(hb);
}

// ── 4-stage lookup ────────────────────────────────────────────────────────────

enum class LookupStage { LOCAL = 1, HOT_CACHE = 2, EDGE_PLIN = 3, CROSS_EDGE = 4 };

struct LookupResult {
    bool found = false;
    _payload_t payload = 0;
    LookupStage stage = LookupStage::LOCAL;
};

struct Stats {
    std::atomic<size_t> hit_local{0};
    std::atomic<size_t> hit_hot{0};
    std::atomic<size_t> hit_edge{0};
    std::atomic<size_t> miss_cross{0};
    void print() const {
        std::cout << "[stats] local=" << hit_local
                  << " hot=" << hit_hot
                  << " edge_plin=" << hit_edge
                  << " cross_edge=" << miss_cross << "\n";
    }
};

static bool request_param_push(plin::transport::Transport& edge, std::mutex& edge_mu,
                               plin::end::ParentPlinCache& ppc,
                               plin::end::HotCache& hc,
                               EndRdmaSnapshot& rdma_snapshot);

class EndNode {
 public:
    EndNode(int id, const plin::RangeMap& rm,
            LocalStore&& store,
            std::shared_ptr<plin::transport::Transport> edge)
        : id_(id), rm_(rm), store_(std::move(store)), edge_(std::move(edge)) {}

    LookupResult lookup(_key_t k) {
        LookupResult r;

        // Stage ①: local B+ tree
        if (rm_.locate_end(k) == id_) {
            auto it = store_.btree.find(k);
            if (it != store_.btree.end()) {
                r.found = true; r.payload = it->second;
                r.stage = LookupStage::LOCAL;
            }
            ++stats_.hit_local;
            return r;
        }

        // Stage ②: hot cache
        _payload_t v = 0;
        if (hot_cache_.find(k, v)) {
            r.found = true; r.payload = v;
            r.stage = LookupStage::HOT_CACHE;
            ++stats_.hit_hot;
            return r;
        }

        // Stage ③: same-edge sibling → EDGE_FETCH_REQ via parent Edge
        int tgt_end = rm_.locate_end(k);
        if (tgt_end >= 0 && rm_.same_edge(tgt_end, id_)) {
            if (edge_) {
                int slot = plin_cache_.predict_pos(k);
                auto rdma_status = rdma_snapshot_lookup(*edge_, rdma_snapshot_, k, slot, v);
                bool stale = false;
                bool ok = rdma_status == RDMALookupStatus::HIT;
                if (rdma_status != RDMALookupStatus::HIT) {
                    ok = edge_fetch(*edge_, edge_mu_, plin_cache_, hot_cache_,
                                    rdma_snapshot_, k, slot, v, stale);
                }
                r.found = ok;
                r.payload = v;
                r.stage = LookupStage::EDGE_PLIN;
                if (ok) hot_cache_.upsert(k, v);  // warm hot cache
                if (stale) {
                    request_param_push(*edge_, edge_mu_, plin_cache_, hot_cache_, rdma_snapshot_);
                }
            }
            ++stats_.hit_edge;
            return r;
        }

        // Stage ④: cross-edge via parent Edge and Cloud.
        if (tgt_end >= 0 && edge_) {
            bool ok = cross_edge_fetch(*edge_, edge_mu_, plin_cache_, hot_cache_,
                                       rdma_snapshot_, k, v);
            r.found = ok;
            r.payload = v;
            r.stage = LookupStage::CROSS_EDGE;
            if (ok) hot_cache_.upsert(k, v);
            ++stats_.miss_cross;
            return r;
        }
        r.stage = LookupStage::CROSS_EDGE;
        ++stats_.miss_cross;
        return r;
    }

    plin::end::HotCache&        hot_cache()  { return hot_cache_; }
    plin::end::ParentPlinCache& plin_cache() { return plin_cache_; }
    const Stats& stats() const { return stats_; }
    bool register_with_edge() {
        if (!edge_) return false;
        return send_end_heartbeat(*edge_, edge_mu_, id_);
    }
    EndRdmaSnapshot& rdma_snapshot() { return rdma_snapshot_; }
    std::shared_ptr<plin::transport::Transport> edge_transport() { return edge_; }

 private:
    int id_;
    const plin::RangeMap& rm_;
    LocalStore store_;
    std::shared_ptr<plin::transport::Transport> edge_;
    std::mutex edge_mu_;
    plin::end::HotCache        hot_cache_;
    plin::end::ParentPlinCache plin_cache_;
    EndRdmaSnapshot rdma_snapshot_;
    Stats stats_{};
};

// ── edge control frames: PLIN_PARAM_PUSH / HOT_UPDATE ────────────────────────

static void apply_edge_control_frame(const plin::rpc::Frame& f,
                                     plin::end::ParentPlinCache& ppc,
                                     plin::end::HotCache& hc,
                                     EndRdmaSnapshot* rdma_snapshot) {
    if (f.type == plin::proto::MsgType::PLIN_PARAM_PUSH) {
        ppc.load_from_push(f.body.data(), f.body.size());
        std::cout << "[end_node] received PLIN_PARAM_PUSH "
                  << f.body.size() << " bytes\n";
    } else if (f.type == plin::proto::MsgType::HOT_UPDATE) {
        // HOT_UPDATE: pairs of (key_t=8B, payload_t=8B)
        size_t n = f.body.size() / 16;
        std::vector<std::pair<_key_t, _payload_t>> kv;
        kv.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            _key_t k; _payload_t p;
            std::memcpy(&k, &f.body[i * 16],     8);
            std::memcpy(&p, &f.body[i * 16 + 8], 8);
            kv.push_back({k, p});
        }
        hc.batch_upsert(kv);
        std::cout << "[end_node] HOT_UPDATE " << n
                  << " keys hot_cache_size=" << hc.size() << "\n";
    } else if (f.type == plin::proto::MsgType::RDMA_SNAPSHOT_INFO) {
        if (!rdma_snapshot || f.body.size() != sizeof(plin::rdma::SnapshotInfo)) return;
        std::memcpy(&rdma_snapshot->info, f.body.data(), sizeof(plin::rdma::SnapshotInfo));
        rdma_snapshot->loaded =
            rdma_snapshot->info.magic == plin::rdma::kSnapshotMagic &&
            rdma_snapshot->info.leaf_count > 0 &&
            rdma_snapshot->info.record_count > 0;
        std::cout << "[end_node] RDMA_SNAPSHOT_INFO loaded=" << rdma_snapshot->loaded
                  << " leaves=" << rdma_snapshot->info.leaf_count
                  << " records=" << rdma_snapshot->info.record_count << "\n";
    }
}

static bool request_param_push(plin::transport::Transport& edge, std::mutex& edge_mu,
                               plin::end::ParentPlinCache& ppc,
                               plin::end::HotCache& hc,
                               EndRdmaSnapshot& rdma_snapshot) {
    plin::rpc::Frame req;
    req.type = plin::proto::MsgType::PLIN_PARAM_PUSH;

    std::lock_guard<std::mutex> lk(edge_mu);
    if (!edge.write_frame(req)) return false;

    plin::rpc::Frame resp;
    if (!edge.read_frame(resp)) return false;
    if (resp.type != plin::proto::MsgType::PLIN_PARAM_PUSH) return false;
    apply_edge_control_frame(resp, ppc, hc, &rdma_snapshot);
    if (edge.read_frame(resp, 50)) {
        apply_edge_control_frame(resp, ppc, hc, &rdma_snapshot);
    }
    return true;
}

static bool wait_initial_plin_push(plin::transport::Transport& edge,
                                   plin::end::ParentPlinCache& ppc,
                                   plin::end::HotCache& hc,
                                   EndRdmaSnapshot& rdma_snapshot,
                                   int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (!ppc.loaded() && std::chrono::steady_clock::now() < deadline) {
        plin::rpc::Frame f;
        if (!edge.read_frame(f, 100)) continue;
        apply_edge_control_frame(f, ppc, hc, &rdma_snapshot);
    }
    plin::rpc::Frame f;
    while (edge.read_frame(f, 25)) {
        apply_edge_control_frame(f, ppc, hc, &rdma_snapshot);
        if (f.type != plin::proto::MsgType::RDMA_SNAPSHOT_INFO) break;
    }
    return ppc.loaded();
}

static void edge_receiver(plin::transport::Transport& edge,
                          plin::end::ParentPlinCache& ppc,
                          plin::end::HotCache& hc,
                          EndRdmaSnapshot& rdma_snapshot) {
    plin::rpc::Frame f;
    while (edge.read_frame(f)) {
        apply_edge_control_frame(f, ppc, hc, &rdma_snapshot);
        // EDGE_FETCH_RESP is handled inline in edge_fetch(). This receiver only
        // starts after the M4/M5 self-test, so it cannot consume that response.
    }
}

static void drain_edge_control_frames(plin::transport::Transport& edge,
                                      plin::end::ParentPlinCache& ppc,
                                      plin::end::HotCache& hc,
                                      EndRdmaSnapshot& rdma_snapshot,
                                      int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        plin::rpc::Frame f;
        if (!edge.read_frame(f, 100)) continue;
        if (f.type == plin::proto::MsgType::PLIN_PARAM_PUSH ||
            f.type == plin::proto::MsgType::HOT_UPDATE ||
            f.type == plin::proto::MsgType::RDMA_SNAPSHOT_INFO) {
            apply_edge_control_frame(f, ppc, hc, &rdma_snapshot);
        } else {
            std::cerr << "[end_node] ignored frame while draining type="
                      << static_cast<int>(f.type) << "\n";
        }
    }
}

// ── benchmark workload replay ────────────────────────────────────────────────

static bool parse_workload_position(const std::string& line, int& out_pos) {
    if (line.empty() || line.rfind("timestamp", 0) == 0) return false;
    std::stringstream ss(line);
    std::string tok;
    if (!std::getline(ss, tok, ',')) return false;
    if (!std::getline(ss, tok, ',')) return false;
    if (!std::getline(ss, tok, ',')) return false;
    try {
        out_pos = std::stoi(tok);
        return out_pos > 0;
    } catch (...) {
        return false;
    }
}

static std::vector<int> load_workload_positions(const std::string& workload_path,
                                                size_t limit) {
    std::ifstream in(workload_path);
    if (!in) {
        std::cerr << "[bench] cannot open workload " << workload_path << "\n";
        return {};
    }
    std::vector<int> positions;
    positions.reserve(limit);
    std::string line;
    while (std::getline(in, line) && positions.size() < limit) {
        int pos = 0;
        if (parse_workload_position(line, pos)) positions.push_back(pos);
    }
    return positions;
}

static std::vector<_key_t> resolve_positions_to_keys(const std::string& data_path,
                                                     const std::vector<int>& positions) {
    std::vector<_key_t> keys(positions.size(), 0);
    std::vector<uint8_t> resolved(positions.size(), 0);
    std::vector<std::pair<int, size_t>> requests;
    requests.reserve(positions.size());
    for (size_t i = 0; i < positions.size(); ++i) {
        if (positions[i] > 0) requests.push_back({positions[i], i});
    }
    std::sort(requests.begin(), requests.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    std::ifstream in(data_path);
    if (!in) {
        std::cerr << "[bench] cannot open data " << data_path << "\n";
        return {};
    }

    size_t req_i = 0;
    int row = 0;
    _key_t k = 0;
    _payload_t p = 0;
    while (req_i < requests.size() && (in >> k >> p)) {
        ++row;
        while (req_i < requests.size() && requests[req_i].first < row) ++req_i;
        while (req_i < requests.size() && requests[req_i].first == row) {
            keys[requests[req_i].second] = k;
            resolved[requests[req_i].second] = 1;
            ++req_i;
        }
    }

    std::vector<_key_t> out;
    out.reserve(keys.size());
    for (size_t i = 0; i < keys.size(); ++i) {
        if (resolved[i]) out.push_back(keys[i]);
    }
    return out;
}

static std::vector<_key_t> load_benchmark_keys(const std::string& workload_path,
                                               const std::string& data_path,
                                               size_t limit) {
    std::vector<int> positions = load_workload_positions(workload_path, limit);
    if (positions.empty()) return {};
    std::vector<_key_t> keys = resolve_positions_to_keys(data_path, positions);
    std::cout << "[bench] loaded_positions=" << positions.size()
              << " resolved_keys=" << keys.size() << "\n";
    return keys;
}

static void run_benchmark(EndNode& node, const std::vector<_key_t>& keys, int end_id) {
    size_t found = 0;
    size_t stage1 = 0;
    size_t stage2 = 0;
    size_t stage3 = 0;
    size_t stage4 = 0;

    auto t0 = std::chrono::steady_clock::now();
    for (_key_t k : keys) {
        auto r = node.lookup(k);
        if (r.found) ++found;
        switch (r.stage) {
            case LookupStage::LOCAL:      ++stage1; break;
            case LookupStage::HOT_CACHE:  ++stage2; break;
            case LookupStage::EDGE_PLIN:  ++stage3; break;
            case LookupStage::CROSS_EDGE: ++stage4; break;
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    double seconds = std::chrono::duration<double>(t1 - t0).count();
    double qps = seconds > 0.0 ? static_cast<double>(keys.size()) / seconds : 0.0;

    std::cout << "[bench] end=" << end_id
              << " queries=" << keys.size()
              << " found=" << found
              << " not_found=" << (keys.size() - found)
              << " stage1=" << stage1
              << " stage2=" << stage2
              << " stage3=" << stage3
              << " stage4=" << stage4
              << " seconds=" << seconds
              << " qps=" << qps
              << " hot_cache_size=" << node.hot_cache().size()
              << "\n";
}

// ── self-test (M3 preserved) ──────────────────────────────────────────────────

static void self_test(EndNode& node, const std::vector<_key_t>& sample_keys,
                      const plin::RangeMap& rm, int my_id,
                      const std::string& data_path) {
    size_t hits = 0;
    for (_key_t k : sample_keys) {
        auto r = node.lookup(k);
        if (r.found && r.stage == LookupStage::LOCAL) ++hits;
    }
    std::cout << "[self_test] stage-① local: "
              << hits << "/" << sample_keys.size()
              << (hits == sample_keys.size() ? " OK" : " FAIL") << "\n";

    // Test stage-③: pick the first key of a sibling End's real range. The
    // topology ranges have small gaps, so key_hi + 1 may not belong to anyone.
    const plin::EndInfo* sibling = nullptr;
    _key_t test_key = 0;
    for (int sid : rm.siblings_of(my_id)) {
        for (const auto& e : rm.ends()) {
            if (e.id == sid && find_first_key_in_range(data_path, e.key_lo, e.key_hi, test_key)) {
                sibling = &e;
                break;
            }
        }
        if (sibling) break;
    }
    if (sibling != nullptr) {
        auto r = node.lookup(test_key);
        std::cout << "[self_test] stage-③ sibling key=" << test_key
                  << " found=" << r.found
                  << " stage=" << static_cast<int>(r.stage)
                  << (r.found && r.stage == LookupStage::EDGE_PLIN ? " OK" : " FAIL") << "\n";
        auto hot = node.lookup(test_key);
        std::cout << "[self_test] stage-② hot-cache replay key=" << test_key
                  << " found=" << hot.found
                  << " stage=" << static_cast<int>(hot.stage)
                  << (hot.found && hot.stage == LookupStage::HOT_CACHE ? " OK" : " FAIL") << "\n";
    }

    const plin::EndInfo* remote = nullptr;
    _key_t remote_key = 0;
    for (const auto& e : rm.ends()) {
        if (!rm.same_edge(e.id, my_id) && find_first_key_in_range(data_path, e.key_lo, e.key_hi, remote_key)) {
            remote = &e;
            break;
        }
    }
    if (remote != nullptr) {
        auto r = node.lookup(remote_key);
        std::cout << "[self_test] stage-④ cross-edge key=" << remote_key
                  << " target_end=" << remote->id
                  << " found=" << r.found
                  << " stage=" << static_cast<int>(r.stage)
                  << (r.found && r.stage == LookupStage::CROSS_EDGE ? " OK" : " FAIL") << "\n";
    }
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    int end_id = 0;
    std::string topology_path = "src/common/topology.yaml";
    std::string data_path     = "Data.txt";
    std::string model_path;
    std::string bench_workload_path;
    size_t bench_queries = 0;
    int bench_wait_ms = 0;
    plin::transport::Mode edge_transport_mode = plin::transport::Mode::AUTO;
    int rdma_port_offset = 1000;

    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if      (k == "--id"       && i+1 < argc) end_id       = std::stoi(argv[++i]);
        else if (k == "--topology" && i+1 < argc) topology_path = argv[++i];
        else if (k == "--data"     && i+1 < argc) data_path     = argv[++i];
        else if (k == "--model"    && i+1 < argc) model_path    = argv[++i];
        else if (k == "--bench-workload" && i+1 < argc) bench_workload_path = argv[++i];
        else if (k == "--bench-queries"  && i+1 < argc) bench_queries = std::stoull(argv[++i]);
        else if (k == "--bench-wait-ms"  && i+1 < argc) bench_wait_ms = std::stoi(argv[++i]);
        else if (k == "--edge-transport" && i+1 < argc) edge_transport_mode = plin::transport::parse_mode(argv[++i]);
        else if (k == "--rdma-port-offset" && i+1 < argc) rdma_port_offset = std::stoi(argv[++i]);
    }
    if (end_id <= 0) {
        std::cerr << "Usage: end_node --id <1-10> [--topology <path>] "
                     "[--data <path>] [--model <path>] "
                     "[--bench-workload <csv> --bench-queries <N>] "
                     "[--edge-transport tcp|rdma|auto] [--rdma-port-offset N]\n";
        return 1;
    }
    if (model_path.empty()) {
        model_path = default_model_path_for(end_id);
    }

    std::cout << "[end_node] id=" << end_id << " topo=" << topology_path
              << " data=" << data_path << " model=" << model_path
              << " edge_transport=" << plin::transport::mode_name(edge_transport_mode)
              << " rdma_port_offset=" << rdma_port_offset << "\n";

    // Topology
    plin::RangeMap rm;
    if (!rm.load(topology_path)) { std::cerr << "[end_node] topology failed\n"; return 1; }

    const plin::EndInfo* my_info = nullptr;
    for (const auto& e : rm.ends()) if (e.id == end_id) { my_info = &e; break; }
    if (!my_info) { std::cerr << "[end_node] id=" << end_id << " not found\n"; return 1; }

    const plin::EdgeInfo* my_edge_info = nullptr;
    for (const auto& e : rm.edges()) if (e.id == my_info->edge_id) { my_edge_info = &e; break; }
    if (!my_edge_info) { std::cerr << "[end_node] edge not found\n"; return 1; }

    std::cout << "[end_node] range=[" << my_info->key_lo << ", " << my_info->key_hi
              << "] edge=" << my_info->edge_id << " (" << my_edge_info->host
              << ":" << my_edge_info->port << ")\n";

    // Load local B+
    std::cout << "[end_node] loading local B+ ...\n";
    auto t0 = std::chrono::steady_clock::now();
    LocalStore store = load_range(data_path, my_info->key_lo, my_info->key_hi);
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "[end_node] loaded " << store.loaded << " keys in "
              << std::chrono::duration<double>(t1 - t0).count() << "s\n";
    if (store.loaded == 0) { std::cerr << "[end_node] no keys loaded\n"; return 1; }

    // Sample keys for self-test (before moving store)
    std::vector<_key_t> sample_keys;
    {
        size_t step = std::max<size_t>(1, store.btree.size() / 1000);
        size_t i = 0;
        for (const auto& [k, v] : store.btree) {
            if (i++ % step == 0) sample_keys.push_back(k);
            if (sample_keys.size() >= 1000) break;
        }
    }

    // Init LSTM runner
    end_lstm_runner_init(model_path);

    // Connect to parent Edge (retry up to 10s)
    std::shared_ptr<plin::transport::Transport> edge_transport;
    for (int attempt = 0; attempt < 20 && !edge_transport; ++attempt) {
        std::string err;
        auto t = plin::transport::connect_transport(my_edge_info->host, my_edge_info->port,
                                                    edge_transport_mode, rdma_port_offset, &err);
        if (t) {
            edge_transport = std::shared_ptr<plin::transport::Transport>(std::move(t));
        } else {
            if (!err.empty()) std::cerr << "[end_node] edge connect failed: " << err << "\n";
            std::cout << "[end_node] waiting for edge (attempt " << attempt+1 << ")...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    if (!edge_transport) {
        std::cerr << "[end_node] could not connect to edge; running without stage-③\n";
    } else {
        std::cout << "[end_node] connected to edge transport="
                  << edge_transport->name() << "\n";
    }

    // Build EndNode
    EndNode node(end_id, rm, std::move(store), edge_transport);

    // Wait for PLIN params (up to 5s)
    if (edge_transport) {
        wait_initial_plin_push(*edge_transport, node.plin_cache(), node.hot_cache(),
                               node.rdma_snapshot(), 5000);
        std::cout << "[end_node] plin_cache loaded=" << node.plin_cache().loaded() << "\n";
        if (node.register_with_edge()) {
            std::cout << "[end_node] registered with edge\n";
        }
    }

    // Self-test
    self_test(node, sample_keys, rm, end_id, data_path);
    node.stats().print();

    if (!bench_workload_path.empty()) {
        if (bench_queries == 0) {
            std::cerr << "[bench] --bench-queries must be > 0\n";
            if (edge_transport) edge_transport->close();
            return 2;
        }
        if (edge_transport && bench_wait_ms > 0) {
            std::cout << "[bench] draining edge control frames for "
                      << bench_wait_ms << "ms\n";
            drain_edge_control_frames(*edge_transport, node.plin_cache(), node.hot_cache(),
                                      node.rdma_snapshot(), bench_wait_ms);
        }
        std::vector<_key_t> bench_keys =
            load_benchmark_keys(bench_workload_path, data_path, bench_queries);
        if (bench_keys.empty()) {
            std::cerr << "[bench] no benchmark keys loaded\n";
            if (edge_transport) edge_transport->close();
            return 2;
        }
        run_benchmark(node, bench_keys, end_id);
        if (edge_transport) edge_transport->close();
        return 0;
    }

    std::cout << "[end_node] running (Ctrl-C to stop)...\n";

    // Start background receiver for future PLIN_PARAM_PUSH / HOT_UPDATE frames.
    std::thread recv_thread;
    if (edge_transport) {
        recv_thread = std::thread([edge_transport, &node]() {
            edge_receiver(*edge_transport, node.plin_cache(), node.hot_cache(),
                          node.rdma_snapshot());
        });
    }

    // Keep alive until recv_thread finishes (edge disconnects) or user kills
    if (recv_thread.joinable()) recv_thread.join();
    if (edge_transport) edge_transport->close();
    return 0;
}
