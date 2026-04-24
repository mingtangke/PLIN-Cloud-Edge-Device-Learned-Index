#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
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
#include <vector>

#include "plin_index.h"
#include "serialize.h"
#include "common/proto.h"
#include "common/range_map.h"
#include "common/rdma_snapshot.h"
#include "common/rpc.h"
#include "common/transport.h"
#ifdef PLIN_ENABLE_RDMA
#include "common/rdma_transport.h"
#endif

using _key_t     = double;
using _payload_t = uint64_t;
using TestIndex  = PlinIndex;

// ── data load ────────────────────────────────────────────────────────────────

struct Dataset {
    std::vector<_key_t>     keys;
    std::vector<_payload_t> payloads;
};

struct EndConn {
    std::shared_ptr<plin::transport::Transport> transport;
    std::shared_ptr<std::mutex> write_mu;
};

struct EdgeRdmaSnapshot {
    std::vector<plin::rdma::LeafDescriptor> descs;
    std::vector<plin::rdma::KeyPayloadRecord> records;
    uint32_t version = plin::rdma::kSnapshotVersion;
};

struct EdgeRuntime {
    int edge_id = 0;
    std::string cloud_host;
    uint16_t cloud_port = 0;
    TestIndex* idx = nullptr;
    std::mutex* plin_mu = nullptr;
    EdgeRdmaSnapshot* rdma_snapshot = nullptr;
    std::mutex ends_mu;
    std::unordered_map<int, EndConn> ends;
};

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

static bool read_u32(const std::vector<uint8_t>& b, size_t off, uint32_t& out) {
    if (off + 4 > b.size()) return false;
    std::memcpy(&out, &b[off], 4);
    return true;
}

static bool read_double(const std::vector<uint8_t>& b, size_t off, double& out) {
    if (off + 8 > b.size()) return false;
    std::memcpy(&out, &b[off], 8);
    return true;
}

static int connect_tcp(const std::string& host, uint16_t port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    ::inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static plin::rpc::Frame make_fetch_resp(plin::proto::Status status,
                                        _payload_t payload = 0) {
    plin::rpc::Frame resp;
    resp.type = plin::proto::MsgType::EDGE_FETCH_RESP;
    resp.body.push_back(static_cast<uint8_t>(status));
    append_u64(resp.body, payload);
    resp.body.push_back(0);
    return resp;
}

// Load keys in [lo, hi] from a text Data.txt (sorted).
// Returns sorted arrays for bulk_load.
static Dataset load_range(const std::string& path, _key_t lo, _key_t hi) {
    std::ifstream in(path);
    if (!in) { std::cerr << "[edge] cannot open " << path << "\n"; return {}; }
    Dataset d;
    _key_t k; _payload_t p;
    while (in >> k >> p) {
        if (k >= lo && k <= hi) {
            d.keys.push_back(k);
            d.payloads.push_back(p);
        }
    }
    return d;
}

static bool write_locked(plin::transport::Transport& transport,
                         const plin::rpc::Frame& frame,
                         const std::shared_ptr<std::mutex>& mu) {
    std::lock_guard<std::mutex> lk(*mu);
    return transport.write_frame(frame);
}

static void register_end(EdgeRuntime& rt, int end_id,
                         const std::shared_ptr<plin::transport::Transport>& transport,
                         const std::shared_ptr<std::mutex>& write_mu) {
    std::lock_guard<std::mutex> lk(rt.ends_mu);
    rt.ends[end_id] = EndConn{transport, write_mu};
    std::cout << "[edge] registered end " << end_id
              << " transport=" << transport->name() << "\n";
}

static void unregister_transport(EdgeRuntime& rt, const plin::transport::Transport* transport) {
    std::lock_guard<std::mutex> lk(rt.ends_mu);
    for (auto it = rt.ends.begin(); it != rt.ends.end();) {
        if (it->second.transport.get() == transport) it = rt.ends.erase(it);
        else ++it;
    }
}

static bool send_to_end(EdgeRuntime& rt, int end_id, const plin::rpc::Frame& frame) {
    EndConn conn;
    {
        std::lock_guard<std::mutex> lk(rt.ends_mu);
        auto it = rt.ends.find(end_id);
        if (it == rt.ends.end()) return false;
        conn = it->second;
    }
    return write_locked(*conn.transport, frame, conn.write_mu);
}

static bool cloud_cross_fetch(EdgeRuntime& rt, const plin::rpc::Frame& req,
                              plin::rpc::Frame& resp) {
    int fd = connect_tcp(rt.cloud_host, rt.cloud_port);
    if (fd < 0) return false;
    bool ok = plin::rpc::write_frame(fd, req) && plin::rpc::read_frame(fd, resp);
    ::close(fd);
    return ok;
}

static void handle_cloud_hot_update(EdgeRuntime& rt, const plin::rpc::Frame& f) {
    uint32_t target = 0;
    if (!read_u32(f.body, 0, target) || f.body.size() < 4) return;

    plin::rpc::Frame to_end;
    to_end.type = plin::proto::MsgType::HOT_UPDATE;
    to_end.body.assign(f.body.begin() + 4, f.body.end());
    size_t n = to_end.body.size() / 16;
    if (send_to_end(rt, static_cast<int>(target), to_end)) {
        std::cout << "[edge] forwarded HOT_UPDATE target_end=" << target
                  << " keys=" << n << "\n";
    } else {
        std::cerr << "[edge] no end connection for HOT_UPDATE target_end=" << target << "\n";
    }
}

static void handle_cloud_cross_req(EdgeRuntime& rt, int cloud_fd,
                                   const plin::rpc::Frame& req) {
    double key = 0;
    if (!read_double(req.body, 8, key)) {
        auto resp = make_fetch_resp(plin::proto::Status::ERROR);
        plin::rpc::write_frame(cloud_fd, resp);
        return;
    }

    _payload_t payload = 0;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(*rt.plin_mu);
        found = rt.idx->find(static_cast<_key_t>(key), payload);
    }
    auto resp = make_fetch_resp(found ? plin::proto::Status::OK
                                      : plin::proto::Status::NOT_FOUND,
                                payload);
    plin::rpc::write_frame(cloud_fd, resp);
    std::cout << "[edge] served CLOUD CROSS_EDGE key=" << key
              << " found=" << found << "\n";
}

static void cloud_receiver_loop(int fd, EdgeRuntime& rt) {
    plin::rpc::Frame f;
    while (plin::rpc::read_frame(fd, f)) {
        if (f.type == plin::proto::MsgType::HEARTBEAT_ACK) {
            std::cout << "[edge] cloud HEARTBEAT_ACK\n";
        } else if (f.type == plin::proto::MsgType::HOT_UPDATE) {
            handle_cloud_hot_update(rt, f);
        } else if (f.type == plin::proto::MsgType::CROSS_EDGE_REQ) {
            handle_cloud_cross_req(rt, fd, f);
        } else {
            std::cerr << "[edge] unexpected cloud msg type " << static_cast<int>(f.type) << "\n";
        }
    }
    std::cerr << "[edge] cloud connection closed\n";
    ::close(fd);
}

static int connect_cloud_control(EdgeRuntime& rt) {
    int fd = -1;
    for (int attempt = 1; attempt <= 30 && fd < 0; ++attempt) {
        fd = connect_tcp(rt.cloud_host, rt.cloud_port);
        if (fd < 0) {
            std::cout << "[edge] waiting for cloud (attempt " << attempt << ")...\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    }
    if (fd < 0) return -1;

    plin::rpc::Frame hb;
    hb.type = plin::proto::MsgType::HEARTBEAT;
    append_u32(hb.body, static_cast<uint32_t>(rt.edge_id));
    if (!plin::rpc::write_frame(fd, hb)) {
        ::close(fd);
        return -1;
    }
    std::cout << "[edge] connected to cloud " << rt.cloud_host << ":" << rt.cloud_port << "\n";
    return fd;
}

// ── PLIN_PARAM_PUSH serialisation ────────────────────────────────────────────

static plin::rpc::Frame make_param_push(TestIndex& idx) {
    std::ostringstream ss;
    serialize_parameter(idx.meta_table.parameter, ss);
    const std::string& raw = ss.str();
    plin::rpc::Frame f;
    f.type = plin::proto::MsgType::PLIN_PARAM_PUSH;
    f.body.assign(raw.begin(), raw.end());
    return f;
}

static EdgeRdmaSnapshot build_rdma_snapshot(TestIndex& idx, const Dataset& ds) {
    EdgeRdmaSnapshot snap;
    const auto& table = idx.meta_table.local_table;
    snap.descs.resize(table.size());
    if (table.empty() || ds.keys.empty()) return snap;

    for (size_t i = 0; i < table.size(); ++i) {
        snap.descs[i].first_key = table[i].key;
        snap.descs[i].version = snap.version;
    }

    std::vector<uint32_t> leaf_ids(ds.keys.size(), 0);
    for (size_t i = 0; i < ds.keys.size(); ++i) {
        int predicted = idx.meta_table.predict_pos(ds.keys[i]);
        size_t leaf = predicted < 0 ? 0 : static_cast<size_t>(predicted);
        if (leaf >= snap.descs.size()) leaf = snap.descs.size() - 1;
        leaf_ids[i] = static_cast<uint32_t>(leaf);
        ++snap.descs[leaf].count;
    }

    uint64_t offset = 0;
    for (auto& desc : snap.descs) {
        desc.offset = offset;
        offset += desc.count;
        desc.count = 0;  // reused as fill cursor below
    }

    snap.records.resize(ds.keys.size());
    for (size_t i = 0; i < ds.keys.size(); ++i) {
        auto& desc = snap.descs[leaf_ids[i]];
        uint64_t pos = desc.offset + desc.count++;
        snap.records[pos] = plin::rdma::KeyPayloadRecord{ds.keys[i], ds.payloads[i]};
    }
    return snap;
}

static plin::rpc::Frame make_rdma_snapshot_info(const plin::rdma::SnapshotInfo& info) {
    plin::rpc::Frame f;
    f.type = plin::proto::MsgType::RDMA_SNAPSHOT_INFO;
    const auto* p = reinterpret_cast<const uint8_t*>(&info);
    f.body.assign(p, p + sizeof(info));
    return f;
}

static void send_rdma_snapshot_info_if_supported(plin::transport::Transport& transport,
                                                 const std::shared_ptr<std::mutex>& write_mu,
                                                 EdgeRuntime& rt) {
    if (!transport.supports_remote_read() || !rt.rdma_snapshot) return;
    if (rt.rdma_snapshot->descs.empty() || rt.rdma_snapshot->records.empty()) return;

    plin::transport::RegisteredMemory desc_mem;
    plin::transport::RegisteredMemory record_mem;
    bool desc_ok = transport.register_memory(rt.rdma_snapshot->descs.data(),
                                             rt.rdma_snapshot->descs.size() *
                                             sizeof(plin::rdma::LeafDescriptor),
                                             desc_mem);
    bool record_ok = transport.register_memory(rt.rdma_snapshot->records.data(),
                                               rt.rdma_snapshot->records.size() *
                                               sizeof(plin::rdma::KeyPayloadRecord),
                                               record_mem);
    if (!desc_ok || !record_ok) {
        std::cerr << "[edge] RDMA snapshot registration failed\n";
        return;
    }

    plin::rdma::SnapshotInfo info;
    info.version = rt.rdma_snapshot->version;
    info.desc_addr = desc_mem.addr;
    info.desc_rkey = desc_mem.rkey;
    info.leaf_count = static_cast<uint32_t>(rt.rdma_snapshot->descs.size());
    info.record_addr = record_mem.addr;
    info.record_rkey = record_mem.rkey;
    info.record_count = rt.rdma_snapshot->records.size();

    auto frame = make_rdma_snapshot_info(info);
    if (write_locked(transport, frame, write_mu)) {
        std::cout << "[edge] RDMA_SNAPSHOT_INFO leaves=" << info.leaf_count
                  << " records=" << info.record_count << "\n";
    }
}

// ── per-End connection handler ────────────────────────────────────────────────

static void handle_end(std::shared_ptr<plin::transport::Transport> transport,
                        TestIndex& idx,
                        const plin::rpc::Frame& param_frame,
                        std::mutex& plin_mu,
                        EdgeRuntime& rt) {
    auto write_mu = std::make_shared<std::mutex>();
    // 1. Push current PLIN parameters
    if (!write_locked(*transport, param_frame, write_mu)) {
        std::cerr << "[edge] PLIN_PARAM_PUSH write failed\n";
        transport->close();
        return;
    }
    send_rdma_snapshot_info_if_supported(*transport, write_mu, rt);

    // 2. Serve EDGE_FETCH_REQ in a loop
    plin::rpc::Frame req;
    while (transport->read_frame(req)) {
        if (req.type == plin::proto::MsgType::HEARTBEAT) {
            uint32_t end_id = 0;
            if (read_u32(req.body, 0, end_id)) {
                register_end(rt, static_cast<int>(end_id), transport, write_mu);
            }
            continue;
        }
        if (req.type == plin::proto::MsgType::PLIN_PARAM_PUSH && req.body.empty()) {
            if (!write_locked(*transport, param_frame, write_mu)) break;
            send_rdma_snapshot_info_if_supported(*transport, write_mu, rt);
            continue;
        }
        if (req.type == plin::proto::MsgType::CROSS_EDGE_REQ) {
            plin::rpc::Frame resp;
            if (!cloud_cross_fetch(rt, req, resp)) {
                resp = make_fetch_resp(plin::proto::Status::ERROR);
            }
            if (!write_locked(*transport, resp, write_mu)) break;
            continue;
        }
        if (req.type != plin::proto::MsgType::EDGE_FETCH_REQ || req.body.size() < 8) {
            std::cerr << "[edge] unexpected msg type " << (int)req.type << "\n";
            break;
        }
        // Decode: 8 bytes key (double BE) + 4 bytes predicted slot (int32 BE)
        double key_val;
        uint8_t key_bytes[8];
        for (int i = 0; i < 8; ++i) key_bytes[i] = req.body[i];
        // double is stored as raw bytes (host order, matching sender)
        std::memcpy(&key_val, key_bytes, 8);

        int32_t predicted_slot = -1;
        if (req.body.size() >= 12) {
            std::memcpy(&predicted_slot, &req.body[8], 4);
        }

        _payload_t payload = 0;
        bool found;
        {
            std::lock_guard<std::mutex> lk(plin_mu);
            if (predicted_slot >= 0) {
                int ret = idx.find_through_net(static_cast<_key_t>(key_val),
                                               payload,
                                               predicted_slot);
                found = (ret == 1);
                if (!found) {
                    found = idx.find(static_cast<_key_t>(key_val), payload);
                }
            } else {
                found = idx.find(static_cast<_key_t>(key_val), payload);
            }
        }

        plin::rpc::Frame resp;
        resp.type = plin::proto::MsgType::EDGE_FETCH_RESP;
        resp.body.push_back(static_cast<uint8_t>(
            found ? plin::proto::Status::OK : plin::proto::Status::NOT_FOUND));
        // 8 bytes payload
        uint8_t pbytes[8];
        std::memcpy(pbytes, &payload, 8);
        resp.body.insert(resp.body.end(), pbytes, pbytes + 8);
        resp.body.push_back(0);  // param_stale_flag = false

        if (!write_locked(*transport, resp, write_mu)) break;
    }
    unregister_transport(rt, transport.get());
    transport->close();
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::cout.setf(std::ios::unitbuf);
    std::cerr.setf(std::ios::unitbuf);

    int edge_id = 0;
    std::string topology_path = "src/common/topology.yaml";
    std::string data_path     = "Data.txt";
    plin::transport::Mode end_transport_mode = plin::transport::Mode::AUTO;
    int rdma_port_offset = 1000;

    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        if      (k == "--id"       && i+1 < argc) edge_id       = std::stoi(argv[++i]);
        else if (k == "--topology" && i+1 < argc) topology_path = argv[++i];
        else if (k == "--data"     && i+1 < argc) data_path     = argv[++i];
        else if (k == "--end-transport" && i+1 < argc) end_transport_mode = plin::transport::parse_mode(argv[++i]);
        else if (k == "--rdma-port-offset" && i+1 < argc) rdma_port_offset = std::stoi(argv[++i]);
    }
    if (edge_id <= 0) {
        std::cerr << "Usage: edge_server --id <1|2> [--topology <path>] [--data <path>] "
                     "[--end-transport tcp|rdma|auto] [--rdma-port-offset N]\n";
        return 1;
    }

    std::cout << "[edge_server] id=" << edge_id << " topo=" << topology_path
              << " data=" << data_path
              << " end_transport=" << plin::transport::mode_name(end_transport_mode)
              << " rdma_port_offset=" << rdma_port_offset << "\n";

    // Load topology
    plin::RangeMap rm;
    if (!rm.load(topology_path)) { std::cerr << "[edge] topology load failed\n"; return 1; }

    // Find our EdgeInfo
    const plin::EdgeInfo* my_edge = nullptr;
    for (const auto& e : rm.edges()) if (e.id == edge_id) { my_edge = &e; break; }
    if (!my_edge) { std::cerr << "[edge] id=" << edge_id << " not in topology\n"; return 1; }

    // Compute key range = union of all managed Ends
    _key_t range_lo =  1e300, range_hi = -1e300;
    for (int eid : my_edge->end_ids) {
        for (const auto& en : rm.ends()) {
            if (en.id == eid) {
                range_lo = std::min(range_lo, en.key_lo);
                range_hi = std::max(range_hi, en.key_hi);
            }
        }
    }
    std::cout << "[edge_server] key_range=[" << range_lo << ", " << range_hi << "]\n";

    // Load data
    std::cout << "[edge_server] loading data...\n";
    auto t0 = std::chrono::steady_clock::now();
    Dataset ds = load_range(data_path, range_lo, range_hi);
    auto t1 = std::chrono::steady_clock::now();
    std::cout << "[edge_server] loaded " << ds.keys.size() << " keys in "
              << std::chrono::duration<double>(t1 - t0).count() << "s\n";

    if (ds.keys.empty()) { std::cerr << "[edge] no data loaded\n"; return 1; }

    // Build PLIN
    std::cout << "[edge_server] bulk_load PLIN...\n";
    t0 = std::chrono::steady_clock::now();
    TestIndex idx("edge_plin");
    idx.bulk_load(ds.keys.data(), ds.payloads.data(), ds.keys.size());
    t1 = std::chrono::steady_clock::now();
    std::cout << "[edge_server] PLIN built in "
              << std::chrono::duration<double>(t1 - t0).count() << "s, params="
              << idx.meta_table.parameter.size() << " levels\n";

    // Serialise params once (shared across all End connections)
    plin::rpc::Frame param_frame = make_param_push(idx);
    std::cout << "[edge_server] param_frame body=" << param_frame.body.size() << " bytes\n";
    EdgeRdmaSnapshot rdma_snapshot = build_rdma_snapshot(idx, ds);
    std::cout << "[edge_server] rdma_snapshot leaves=" << rdma_snapshot.descs.size()
              << " records=" << rdma_snapshot.records.size() << "\n";

    std::mutex plin_mu;
    EdgeRuntime rt;
    rt.edge_id = edge_id;
    rt.cloud_host = rm.cloud().host;
    rt.cloud_port = rm.cloud().port;
    rt.idx = &idx;
    rt.plin_mu = &plin_mu;
    rt.rdma_snapshot = &rdma_snapshot;

    int cloud_fd = connect_cloud_control(rt);
    if (cloud_fd >= 0) {
        std::thread([cloud_fd, &rt]() {
            cloud_receiver_loop(cloud_fd, rt);
        }).detach();
    } else {
        std::cerr << "[edge] cloud unavailable; stage-④ disabled\n";
    }

    auto launch_handler = [&idx, &param_frame, &plin_mu, &rt](
                              std::shared_ptr<plin::transport::Transport> transport) {
        std::thread([transport, &idx, &param_frame, &plin_mu, &rt]() mutable {
            handle_end(transport, idx, param_frame, plin_mu, rt);
        }).detach();
    };

    if (end_transport_mode != plin::transport::Mode::TCP) {
#ifdef PLIN_ENABLE_RDMA
        std::string err;
        uint16_t rdma_port = plin::transport::rdma_port_for(my_edge->port, rdma_port_offset);
        auto rdma_listener = plin::transport::listen_rdma(rdma_port, 16, &err);
        if (rdma_listener) {
            std::cout << "[edge_server] RDMA listening on port " << rdma_port << "...\n";
            std::thread([listener = std::move(rdma_listener), launch_handler]() mutable {
                while (true) {
                    std::string accept_err;
                    auto t = listener->accept(&accept_err);
                    if (!t) {
                        std::cerr << "[edge] RDMA accept failed: " << accept_err << "\n";
                        continue;
                    }
                    launch_handler(std::shared_ptr<plin::transport::Transport>(std::move(t)));
                }
            }).detach();
        } else {
            std::cerr << "[edge] RDMA listen disabled: " << err << "\n";
            if (end_transport_mode == plin::transport::Mode::RDMA) return 1;
        }
#else
        if (end_transport_mode == plin::transport::Mode::RDMA) {
            std::cerr << "[edge] RDMA requested but PLIN_ENABLE_RDMA=OFF\n";
            return 1;
        }
        std::cout << "[edge] RDMA unavailable in this build; using TCP fallback\n";
#endif
    }

    if (end_transport_mode == plin::transport::Mode::RDMA) {
        while (true) std::this_thread::sleep_for(std::chrono::hours(24));
    }

    int listen_fd = plin::transport::listen_tcp(my_edge->port, 16);
    if (listen_fd < 0) {
        perror("[edge] tcp listen");
        return 1;
    }
    std::cout << "[edge_server] TCP listening on port " << my_edge->port << "...\n";

    while (true) {
        auto t = plin::transport::accept_tcp(listen_fd);
        if (!t) { perror("[edge] tcp accept"); continue; }
        launch_handler(std::shared_ptr<plin::transport::Transport>(std::move(t)));
    }
    return 0;
}
