// M2 loopback test: verifies RPC framing and topology parsing.
//
// Usage: ./loopback_test [path/to/topology.yaml]
//
// Creates a socket pair, spawns a server thread and a client thread.
// Client sends QUERY_REQ; server receives it and replies QUERY_RESP.
// Also loads topology.yaml and prints a summary.

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cassert>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "proto.h"
#include "range_map.h"
#include "rpc.h"

// ── helpers ──────────────────────────────────────────────────────────────────

static void encode_u64_be(uint64_t v, std::vector<uint8_t>& body) {
    for (int i = 7; i >= 0; --i)
        body.push_back(static_cast<uint8_t>((v >> (i * 8)) & 0xff));
}

static uint64_t decode_u64_be(const std::vector<uint8_t>& body, size_t off = 0) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i)
        v = (v << 8) | body[off + i];
    return v;
}

// ── loopback test ────────────────────────────────────────────────────────────

static void run_loopback_test() {
    // Create a socket pair (like a pipe, but bidirectional)
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
        throw std::runtime_error("socketpair failed");

    const uint64_t test_key = 1234567;
    bool client_ok = false;
    bool server_ok = false;

    // Server thread: recv QUERY_REQ, reply QUERY_RESP
    std::thread server_thread([&] {
        plin::rpc::Frame req;
        if (!plin::rpc::read_frame(sv[0], req)) {
            std::cerr << "[server] read_frame failed\n";
            return;
        }
        if (req.type != plin::proto::MsgType::QUERY_REQ || req.body.size() < 8) {
            std::cerr << "[server] unexpected frame type or short body\n";
            return;
        }
        uint64_t key = decode_u64_be(req.body);
        std::cout << "[server] received QUERY_REQ key=" << key << "\n";

        plin::rpc::Frame resp;
        resp.type = plin::proto::MsgType::QUERY_RESP;
        resp.body.push_back(static_cast<uint8_t>(plin::proto::Status::OK));
        uint64_t payload = key * 2;  // fake payload
        encode_u64_be(payload, resp.body);

        if (!plin::rpc::write_frame(sv[0], resp)) {
            std::cerr << "[server] write_frame failed\n";
            return;
        }
        server_ok = true;
        std::cout << "[server] sent QUERY_RESP payload=" << payload << "\n";
        ::close(sv[0]);
    });

    // Client: send QUERY_REQ, recv QUERY_RESP
    {
        plin::rpc::Frame req;
        req.type = plin::proto::MsgType::QUERY_REQ;
        encode_u64_be(test_key, req.body);

        if (!plin::rpc::write_frame(sv[1], req)) {
            std::cerr << "[client] write_frame failed\n";
        } else {
            plin::rpc::Frame resp;
            if (!plin::rpc::read_frame(sv[1], resp)) {
                std::cerr << "[client] read_frame failed\n";
            } else {
                if (resp.type == plin::proto::MsgType::QUERY_RESP && resp.body.size() >= 9) {
                    auto status = static_cast<plin::proto::Status>(resp.body[0]);
                    uint64_t payload = decode_u64_be(resp.body, 1);
                    std::cout << "[client] received QUERY_RESP status="
                              << static_cast<int>(status) << " payload=" << payload << "\n";
                    client_ok = (status == plin::proto::Status::OK && payload == test_key * 2);
                }
            }
        }
        ::close(sv[1]);
    }

    server_thread.join();

    if (server_ok && client_ok) {
        std::cout << "[loopback] RPC round-trip OK\n";
    } else {
        throw std::runtime_error("RPC loopback FAILED");
    }
}

// ── topology test ────────────────────────────────────────────────────────────

static void run_topology_test(const std::string& yaml_path) {
    plin::RangeMap rm;
    if (!rm.load(yaml_path)) {
        throw std::runtime_error("RangeMap::load failed for " + yaml_path);
    }

    const auto& cloud = rm.cloud();
    std::cout << "[topology] cloud " << cloud.host << ":" << cloud.port << "\n";

    for (const auto& e : rm.edges()) {
        std::cout << "[topology] edge " << e.id << " " << e.host << ":" << e.port
                  << " ends=[";
        for (int i = 0; i < (int)e.end_ids.size(); ++i)
            std::cout << (i ? "," : "") << e.end_ids[i];
        std::cout << "]\n";
    }

    for (const auto& e : rm.ends()) {
        std::cout << "[topology] end " << e.id << " edge=" << e.edge_id
                  << " " << e.host << ":" << e.port
                  << " range=[" << e.key_lo << "," << e.key_hi << "]\n";
    }

    // Spot-check locate_end using actual boundary keys from Data.txt
    struct Case { plin::key_t key; int expected_end; };
    std::vector<Case> cases = {
        {-1e13,               1},   // lo sentinel of End 1
        {-50332314890.551338, 1},   // first row of Data.txt
        {-12815012040.383575, 1},   // last row of End 1's block
        {-12815008635.778347, 2},   // first row of End 2's block
        {5194391.707614,      5},   // last row of End 5
        {5202302.971719,      6},   // first row of End 6
        {12813376566.001352,  10},  // first row of End 10
        {1e13,                10},  // hi sentinel of End 10
    };
    bool all_ok = true;
    for (auto& c : cases) {
        int got = rm.locate_end(c.key);
        bool ok = (got == c.expected_end);
        std::cout << "[topology] locate_end(" << c.key << ")=" << got
                  << (ok ? " OK" : " FAIL (expected " + std::to_string(c.expected_end) + ")")
                  << "\n";
        all_ok &= ok;
    }

    // Check same_edge / siblings
    std::cout << "[topology] same_edge(1,3)=" << rm.same_edge(1, 3) << " (expect 1)\n";
    std::cout << "[topology] same_edge(1,6)=" << rm.same_edge(1, 6) << " (expect 0)\n";
    auto sibs = rm.siblings_of(1);
    std::cout << "[topology] siblings_of(1)=[";
    for (size_t i = 0; i < sibs.size(); ++i)
        std::cout << (i ? "," : "") << sibs[i];
    std::cout << "] (expect [2,3,4,5])\n";

    if (!all_ok) { std::cerr << "[topology] locate_end spot-checks FAILED\n"; std::exit(1); }
    std::cout << "[topology] all checks OK\n";
}

// ── main ─────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    std::string topo = "src/common/topology.yaml";
    if (argc > 1) topo = argv[1];

    std::cout << "=== RPC loopback test ===\n";
    run_loopback_test();

    std::cout << "\n=== Topology parse test: " << topo << " ===\n";
    run_topology_test(topo);

    std::cout << "\n[M2] All tests passed.\n";
    return 0;
}
