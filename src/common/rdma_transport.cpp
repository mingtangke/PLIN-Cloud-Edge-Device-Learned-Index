#include "rdma_transport.h"

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace plin::transport {

namespace {

constexpr size_t kMaxFrameBytes = 4 * 1024 * 1024;
constexpr int kRecvDepth = 8;
constexpr int kSendDepth = 8;
constexpr int kPollSleepUs = 50;
constexpr int kDefaultCmTimeoutMs = 3000;

struct RdmaBuffer {
    std::vector<uint8_t> bytes;
    ibv_mr* mr = nullptr;
};

bool poll_cq(ibv_cq* cq, uint64_t* wr_id, int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms < 0 ? 60000 : timeout_ms);
    while (timeout_ms < 0 || std::chrono::steady_clock::now() < deadline) {
        ibv_wc wc{};
        int n = ibv_poll_cq(cq, 1, &wc);
        if (n < 0) return false;
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(kPollSleepUs));
            continue;
        }
        if (wc.status != IBV_WC_SUCCESS) {
            std::cerr << "[rdma] CQ failure status=" << ibv_wc_status_str(wc.status) << "\n";
            return false;
        }
        if (wr_id) *wr_id = wc.wr_id;
        return true;
    }
    return false;
}

bool wait_cm_event(rdma_event_channel* ec, rdma_cm_event_type expected,
                   rdma_cm_id** out_id, int timeout_ms = kDefaultCmTimeoutMs) {
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(timeout_ms < 0 ? 60000 : timeout_ms);
    while (timeout_ms < 0 || std::chrono::steady_clock::now() < deadline) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(ec->fd, &rfds);

        timeval tv{};
        timeval* tvp = nullptr;
        if (timeout_ms >= 0) {
            auto now = std::chrono::steady_clock::now();
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            if (remaining.count() < 0) remaining = std::chrono::milliseconds(0);
            tv.tv_sec = static_cast<long>(remaining.count() / 1000);
            tv.tv_usec = static_cast<long>((remaining.count() % 1000) * 1000);
            tvp = &tv;
        }
        int ready = ::select(ec->fd + 1, &rfds, nullptr, nullptr, tvp);
        if (ready < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (ready == 0) return false;

        rdma_cm_event* event = nullptr;
        int rc = rdma_get_cm_event(ec, &event);
        if (rc != 0) return false;
        auto type = event->event;
        rdma_cm_id* id = event->id;
        rdma_ack_cm_event(event);
        if (type == expected) {
            if (out_id) *out_id = id;
            return true;
        }
        if (expected == RDMA_CM_EVENT_CONNECT_REQUEST &&
            type == RDMA_CM_EVENT_DISCONNECTED) {
            continue;
        }
        if (type == RDMA_CM_EVENT_DISCONNECTED ||
            type == RDMA_CM_EVENT_REJECTED ||
            type == RDMA_CM_EVENT_CONNECT_ERROR ||
            type == RDMA_CM_EVENT_ADDR_ERROR ||
            type == RDMA_CM_EVENT_ROUTE_ERROR) {
            std::cerr << "[rdma] unexpected CM event " << rdma_event_str(type) << "\n";
            return false;
        }
    }
    return false;
}

std::string port_string(uint16_t port) {
    return std::to_string(static_cast<unsigned>(port));
}

class RdmaTransport final : public Transport {
public:
    RdmaTransport(rdma_cm_id* id,
                  rdma_event_channel* event_channel,
                  ibv_pd* pd,
                  ibv_cq* send_cq,
                  ibv_cq* recv_cq)
        : id_(id),
          event_channel_(event_channel),
          pd_(pd),
          send_cq_(send_cq),
          recv_cq_(recv_cq) {}

    ~RdmaTransport() override { close(); }

    bool init_buffers() {
        send_.bytes.resize(kMaxFrameBytes);
        read_.bytes.resize(kMaxFrameBytes);
        send_.mr = ibv_reg_mr(pd_, send_.bytes.data(), send_.bytes.size(), IBV_ACCESS_LOCAL_WRITE);
        read_.mr = ibv_reg_mr(pd_, read_.bytes.data(), read_.bytes.size(), IBV_ACCESS_LOCAL_WRITE);
        if (!send_.mr || !read_.mr) return false;

        recv_.resize(kRecvDepth);
        for (int i = 0; i < kRecvDepth; ++i) {
            recv_[i].bytes.resize(kMaxFrameBytes);
            recv_[i].mr = ibv_reg_mr(pd_, recv_[i].bytes.data(), recv_[i].bytes.size(),
                                     IBV_ACCESS_LOCAL_WRITE);
            if (!recv_[i].mr) return false;
            if (!post_recv(static_cast<size_t>(i))) return false;
        }
        return true;
    }

    const char* name() const override { return "rdma"; }

    bool read_frame(rpc::Frame& out, int timeout_ms = -1) override {
        uint64_t wr_id = 0;
        if (!poll_cq(recv_cq_, &wr_id, timeout_ms)) return false;
        size_t idx = static_cast<size_t>(wr_id);
        if (idx >= recv_.size()) return false;
        auto& b = recv_[idx].bytes;
        if (b.size() < 5) return false;

        uint32_t payload_len = 0;
        std::memcpy(&payload_len, b.data(), sizeof(payload_len));
        payload_len = ntohl(payload_len);
        if (payload_len < 1 || payload_len + 4 > b.size()) return false;

        out.type = static_cast<proto::MsgType>(b[4]);
        size_t body_len = static_cast<size_t>(payload_len - 1);
        out.body.resize(body_len);
        if (body_len > 0) {
            std::memcpy(out.body.data(), b.data() + 5, body_len);
        }
        return post_recv(idx);
    }

    bool write_frame(const rpc::Frame& f) override {
        size_t payload_len = 1 + f.body.size();
        size_t total = 4 + payload_len;
        if (total > send_.bytes.size()) return false;

        uint32_t len_be = htonl(static_cast<uint32_t>(payload_len));
        std::memcpy(send_.bytes.data(), &len_be, 4);
        send_.bytes[4] = static_cast<uint8_t>(f.type);
        if (!f.body.empty()) {
            std::memcpy(send_.bytes.data() + 5, f.body.data(), f.body.size());
        }

        ibv_sge sge{};
        sge.addr = reinterpret_cast<uint64_t>(send_.bytes.data());
        sge.length = static_cast<uint32_t>(total);
        sge.lkey = send_.mr->lkey;

        ibv_send_wr wr{};
        wr.wr_id = 1;
        wr.sg_list = &sge;
        wr.num_sge = 1;
        wr.opcode = IBV_WR_SEND;
        wr.send_flags = IBV_SEND_SIGNALED;

        ibv_send_wr* bad = nullptr;
        if (ibv_post_send(id_->qp, &wr, &bad) != 0) return false;
        return poll_cq(send_cq_, nullptr, kDefaultCmTimeoutMs);
    }

    void close() override {
        if (closed_) return;
        closed_ = true;
        if (id_) rdma_disconnect(id_);
        for (auto& mr : external_mrs_) {
            if (mr) ibv_dereg_mr(mr);
        }
        external_mrs_.clear();
        if (send_.mr) ibv_dereg_mr(send_.mr);
        if (read_.mr) ibv_dereg_mr(read_.mr);
        for (auto& b : recv_) {
            if (b.mr) ibv_dereg_mr(b.mr);
        }
        if (id_) {
            rdma_destroy_qp(id_);
            rdma_destroy_id(id_);
            id_ = nullptr;
        }
        if (send_cq_) {
            ibv_destroy_cq(send_cq_);
            send_cq_ = nullptr;
        }
        if (recv_cq_) {
            ibv_destroy_cq(recv_cq_);
            recv_cq_ = nullptr;
        }
        if (pd_) {
            ibv_dealloc_pd(pd_);
            pd_ = nullptr;
        }
        if (event_channel_) {
            rdma_destroy_event_channel(event_channel_);
            event_channel_ = nullptr;
        }
    }

    bool supports_remote_read() const override { return true; }

    bool register_memory(const void* addr, size_t size, RegisteredMemory& out) override {
        if (!addr || size == 0) return false;
        auto* mr = ibv_reg_mr(pd_, const_cast<void*>(addr), size,
                              IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ);
        if (!mr) return false;
        external_mrs_.push_back(mr);
        out.addr = reinterpret_cast<uint64_t>(addr);
        out.rkey = mr->rkey;
        out.size = size;
        return true;
    }

    bool read_remote(uint64_t remote_addr, uint32_t rkey, void* dst, size_t size) override {
        if (!dst || size == 0) return true;
        size_t done = 0;
        while (done < size) {
            size_t chunk = std::min(read_.bytes.size(), size - done);

            ibv_sge sge{};
            sge.addr = reinterpret_cast<uint64_t>(read_.bytes.data());
            sge.length = static_cast<uint32_t>(chunk);
            sge.lkey = read_.mr->lkey;

            ibv_send_wr wr{};
            wr.wr_id = 2;
            wr.sg_list = &sge;
            wr.num_sge = 1;
            wr.opcode = IBV_WR_RDMA_READ;
            wr.send_flags = IBV_SEND_SIGNALED;
            wr.wr.rdma.remote_addr = remote_addr + done;
            wr.wr.rdma.rkey = rkey;

            ibv_send_wr* bad = nullptr;
            if (ibv_post_send(id_->qp, &wr, &bad) != 0) return false;
            if (!poll_cq(send_cq_, nullptr, kDefaultCmTimeoutMs)) return false;
            std::memcpy(static_cast<uint8_t*>(dst) + done, read_.bytes.data(), chunk);
            done += chunk;
        }
        return true;
    }

private:
    bool post_recv(size_t idx) {
        auto& b = recv_[idx];
        ibv_sge sge{};
        sge.addr = reinterpret_cast<uint64_t>(b.bytes.data());
        sge.length = static_cast<uint32_t>(b.bytes.size());
        sge.lkey = b.mr->lkey;

        ibv_recv_wr wr{};
        wr.wr_id = idx;
        wr.sg_list = &sge;
        wr.num_sge = 1;

        ibv_recv_wr* bad = nullptr;
        return ibv_post_recv(id_->qp, &wr, &bad) == 0;
    }

    rdma_cm_id* id_ = nullptr;
    rdma_event_channel* event_channel_ = nullptr;
    ibv_pd* pd_ = nullptr;
    ibv_cq* send_cq_ = nullptr;
    ibv_cq* recv_cq_ = nullptr;
    RdmaBuffer send_;
    RdmaBuffer read_;
    std::vector<RdmaBuffer> recv_;
    std::vector<ibv_mr*> external_mrs_;
    bool closed_ = false;
};

bool build_qp(rdma_cm_id* id, ibv_pd** pd, ibv_cq** send_cq, ibv_cq** recv_cq) {
    *pd = ibv_alloc_pd(id->verbs);
    if (!*pd) return false;
    *send_cq = ibv_create_cq(id->verbs, kSendDepth + 4, nullptr, nullptr, 0);
    *recv_cq = ibv_create_cq(id->verbs, kRecvDepth + 4, nullptr, nullptr, 0);
    if (!*send_cq || !*recv_cq) return false;

    ibv_qp_init_attr attr{};
    attr.send_cq = *send_cq;
    attr.recv_cq = *recv_cq;
    attr.qp_type = IBV_QPT_RC;
    attr.cap.max_send_wr = kSendDepth + 4;
    attr.cap.max_recv_wr = kRecvDepth + 4;
    attr.cap.max_send_sge = 1;
    attr.cap.max_recv_sge = 1;
    attr.cap.max_inline_data = 0;
    return rdma_create_qp(id, *pd, &attr) == 0;
}

std::unique_ptr<RdmaTransport> make_transport(rdma_cm_id* id,
                                              rdma_event_channel* ec,
                                              std::string* err) {
    ibv_pd* pd = nullptr;
    ibv_cq* send_cq = nullptr;
    ibv_cq* recv_cq = nullptr;
    if (!build_qp(id, &pd, &send_cq, &recv_cq)) {
        if (err) *err = "failed to create RDMA QP";
        if (send_cq) ibv_destroy_cq(send_cq);
        if (recv_cq) ibv_destroy_cq(recv_cq);
        if (pd) ibv_dealloc_pd(pd);
        return nullptr;
    }

    auto t = std::make_unique<RdmaTransport>(id, ec, pd, send_cq, recv_cq);
    if (!t->init_buffers()) {
        if (err) *err = "failed to register RDMA buffers";
        return nullptr;
    }
    return t;
}

class RdmaListenerImpl final : public RdmaListener {
public:
    RdmaListenerImpl(rdma_cm_id* id, rdma_event_channel* ec)
        : id_(id), ec_(ec) {}

    ~RdmaListenerImpl() override { close(); }

    std::unique_ptr<Transport> accept(std::string* err = nullptr) override {
        rdma_cm_id* child = nullptr;
        if (!wait_cm_event(ec_, RDMA_CM_EVENT_CONNECT_REQUEST, &child, -1)) {
            if (err) *err = "RDMA accept failed waiting for CONNECT_REQUEST";
            return nullptr;
        }

        auto t = make_transport(child, nullptr, err);
        if (!t) {
            rdma_reject(child, nullptr, 0);
            return nullptr;
        }

        rdma_conn_param param{};
        param.initiator_depth = 1;
        param.responder_resources = 1;
        param.retry_count = 7;
        param.rnr_retry_count = 7;
        if (rdma_accept(child, &param) != 0) {
            if (err) *err = "rdma_accept failed";
            return nullptr;
        }

        rdma_cm_id* established = nullptr;
        if (!wait_cm_event(ec_, RDMA_CM_EVENT_ESTABLISHED, &established, kDefaultCmTimeoutMs)) {
            if (err) *err = "RDMA accept failed waiting for ESTABLISHED";
            return nullptr;
        }
        return t;
    }

    void close() override {
        if (id_) {
            rdma_destroy_id(id_);
            id_ = nullptr;
        }
        if (ec_) {
            rdma_destroy_event_channel(ec_);
            ec_ = nullptr;
        }
    }

private:
    rdma_cm_id* id_ = nullptr;
    rdma_event_channel* ec_ = nullptr;
};

}  // namespace

std::unique_ptr<RdmaListener> listen_rdma(uint16_t port, int backlog, std::string* err) {
    auto* ec = rdma_create_event_channel();
    if (!ec) {
        if (err) *err = "rdma_create_event_channel failed";
        return nullptr;
    }

    rdma_cm_id* id = nullptr;
    if (rdma_create_id(ec, &id, nullptr, RDMA_PS_TCP) != 0) {
        if (err) *err = "rdma_create_id failed";
        rdma_destroy_event_channel(ec);
        return nullptr;
    }

    addrinfo hints{};
    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(nullptr, port_string(port).c_str(), &hints, &res) != 0 || !res) {
        if (err) *err = "getaddrinfo failed for RDMA listen";
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        return nullptr;
    }

    int rc = rdma_bind_addr(id, reinterpret_cast<sockaddr*>(res->ai_addr));
    freeaddrinfo(res);
    if (rc != 0) {
        if (err) *err = "rdma_bind_addr failed";
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        return nullptr;
    }
    if (rdma_listen(id, backlog) != 0) {
        if (err) *err = "rdma_listen failed";
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        return nullptr;
    }
    return std::make_unique<RdmaListenerImpl>(id, ec);
}

std::unique_ptr<Transport> connect_rdma(const std::string& host, uint16_t port, std::string* err) {
    auto* ec = rdma_create_event_channel();
    if (!ec) {
        if (err) *err = "rdma_create_event_channel failed";
        return nullptr;
    }

    rdma_cm_id* id = nullptr;
    if (rdma_create_id(ec, &id, nullptr, RDMA_PS_TCP) != 0) {
        if (err) *err = "rdma_create_id failed";
        rdma_destroy_event_channel(ec);
        return nullptr;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    if (getaddrinfo(host.c_str(), port_string(port).c_str(), &hints, &res) != 0 || !res) {
        if (err) *err = "getaddrinfo failed for RDMA connect";
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        return nullptr;
    }

    int rc = rdma_resolve_addr(id, nullptr, reinterpret_cast<sockaddr*>(res->ai_addr),
                               kDefaultCmTimeoutMs);
    freeaddrinfo(res);
    if (rc != 0 || !wait_cm_event(ec, RDMA_CM_EVENT_ADDR_RESOLVED, nullptr)) {
        if (err) *err = "rdma_resolve_addr failed";
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        return nullptr;
    }

    auto t = make_transport(id, ec, err);
    if (!t) {
        rdma_destroy_id(id);
        rdma_destroy_event_channel(ec);
        return nullptr;
    }

    if (rdma_resolve_route(id, kDefaultCmTimeoutMs) != 0 ||
        !wait_cm_event(ec, RDMA_CM_EVENT_ROUTE_RESOLVED, nullptr)) {
        if (err) *err = "rdma_resolve_route failed";
        return nullptr;
    }

    rdma_conn_param param{};
    param.initiator_depth = 1;
    param.responder_resources = 1;
    param.retry_count = 7;
    param.rnr_retry_count = 7;
    if (rdma_connect(id, &param) != 0 ||
        !wait_cm_event(ec, RDMA_CM_EVENT_ESTABLISHED, nullptr)) {
        if (err) *err = "rdma_connect failed";
        return nullptr;
    }
    return t;
}

}  // namespace plin::transport
