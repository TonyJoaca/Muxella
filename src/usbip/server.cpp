#include "metvoice/usbip/server.hpp"
#include "metvoice/core/mmcss.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace metvoice::usbip {

namespace {

bool parse_listen_address(const std::string& addr, std::string& host, uint16_t& port) {
    const size_t colon = addr.rfind(':');
    if (colon == std::string::npos) return false;
    host = addr.substr(0, colon);
    if (host.empty()) host = "127.0.0.1";
    const int parsed = std::atoi(addr.substr(colon + 1).c_str());
    if (parsed < 0 || parsed > 65535) return false;
    port = static_cast<uint16_t>(parsed);
    return true;
}

std::string format_remote(const sockaddr_storage& ss) {
    if (ss.ss_family == AF_INET) {
        const auto* s4 = reinterpret_cast<const sockaddr_in*>(&ss);
        char ip[INET_ADDRSTRLEN]{};
        inet_ntop(AF_INET, &s4->sin_addr, ip, sizeof(ip));
        return std::string(ip) + ":" + std::to_string(ntohs(s4->sin_port));
    }
    return "client";
}

void write_usb_device(std::vector<uint8_t>& out, const Uac1Device& dev) {
    const size_t base = out.size();
    out.resize(base + 312, 0);
    uint8_t* p = out.data() + base;

    char path_buf[256]{};
    std::snprintf(path_buf, sizeof(path_buf), "/sys/devices/platform/virtual-cables/%s", dev.bus_id().c_str());
    std::memcpy(p, path_buf, std::min<size_t>(std::strlen(path_buf), 255));
    std::memcpy(p + 256, dev.bus_id().c_str(), std::min<size_t>(dev.bus_id().size(), 31));

    uint8_t* meta = p + 288;
    const auto& dd = dev.descriptors().device;
    write_be32(meta, 1);                                            // bus number
    write_be32(meta + 4, static_cast<uint32_t>(dev.number()));      // device number
    write_be32(meta + 8, kSpeedFull);                               // full speed
    write_be16(meta + 12, read_le16(dd.data() + 8));                // idVendor
    write_be16(meta + 14, read_le16(dd.data() + 10));               // idProduct
    write_be16(meta + 16, read_le16(dd.data() + 12));               // bcdDevice
    meta[18] = dd[4];                                               // bDeviceClass
    meta[19] = dd[5];                                               // bDeviceSubClass
    meta[20] = dd[6];                                               // bDeviceProtocol
    meta[21] = 1;                                                   // bConfigurationValue
    meta[22] = dd[17];                                              // bNumConfigurations
    meta[23] = 3;                                                   // bNumInterfaces
}

struct TransferSlot {
    SubmitRequest req;
    std::vector<uint8_t> out;
    std::vector<IsoPacket> packets;
    Clock::time_point complete_at{};

    std::atomic<bool> cancelled{false};
    std::mutex waiter_mu;
    IsoWaiter* active_waiter{nullptr};

    TransferSlot() {
        out.reserve(64 * 1024);
        packets.reserve(32);
    }

    void reset_for_reuse() noexcept {
        cancelled.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(waiter_mu);
        active_waiter = nullptr;
    }

    void cancel() noexcept {
        IsoWaiter* w = nullptr;
        {
            std::lock_guard<std::mutex> lock(waiter_mu);
            cancelled.store(true, std::memory_order_release);
            w = active_waiter;
        }
        if (w) w->abort();
    }

    bool attach_waiter(IsoWaiter* w) noexcept {
        std::lock_guard<std::mutex> lock(waiter_mu);
        if (cancelled.load(std::memory_order_acquire)) return false;
        active_waiter = w;
        return true;
    }

    void detach_waiter() noexcept {
        std::lock_guard<std::mutex> lock(waiter_mu);
        active_waiter = nullptr;
    }
};

class TransferPool {
public:
    TransferSlot* acquire() {
        std::lock_guard<std::mutex> lock(mu_);
        if (!free_.empty()) {
            TransferSlot* slot = free_.back().release();
            free_.pop_back();
            slot->reset_for_reuse();
            return slot;
        }
        return new TransferSlot();
    }

    void release(TransferSlot* slot) {
        if (!slot) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (free_.size() < 256) {
            free_.emplace_back(slot);
        } else {
            delete slot;
        }
    }

private:
    std::mutex mu_;
    std::vector<std::unique_ptr<TransferSlot>> free_;
};

class IsoQueue {
public:
    void push(TransferSlot* slot) {
        std::lock_guard<std::mutex> lock(mu_);
        items_.push_back(slot);
        cv_.notify_one();
    }

    void close() {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        cv_.notify_all();
    }

    TransferSlot* pop(std::vector<TransferSlot*>& drain_buf) {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [&]() { return closed_ || !items_.empty(); });
        if (closed_) {
            while (!items_.empty()) {
                drain_buf.push_back(items_.front());
                items_.pop_front();
            }
            return nullptr;
        }
        TransferSlot* slot = items_.front();
        items_.pop_front();
        return slot;
    }

    void drain_remaining(std::vector<TransferSlot*>& out) {
        std::lock_guard<std::mutex> lock(mu_);
        while (!items_.empty()) {
            out.push_back(items_.front());
            items_.pop_front();
        }
    }

private:
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<TransferSlot*> items_;
    bool closed_{false};
};

struct ConnectionContext {
    RtTcpSocket& sock;
    Uac1Device& device;
    const std::function<void(const std::string&)>& log_fn;
    std::atomic<uint64_t>& urb_count;

    void log(const std::string& msg) const {
        if (log_fn) log_fn(msg);
    }

    std::atomic<bool> closed{false};
    IsoScheduler scheduler;
    TransferPool pool;

    std::mutex pending_mu;
    std::unordered_map<uint32_t, TransferSlot*> pending;
    std::unordered_set<IsoWaiter*> waiters;

    IsoQueue ep1_queue;
    IsoQueue ep2_queue;

    std::mutex frame_mu;
    std::vector<uint8_t> write_frame;

    ConnectionContext(RtTcpSocket& s, Uac1Device& d, const std::function<void(const std::string&)>& l,
                      std::atomic<uint64_t>& uc)
        : sock(s), device(d), log_fn(l), urb_count(uc) {
        write_frame.reserve(64 * 1024);
    }

    void register_waiter(IsoWaiter* w) {
        std::lock_guard<std::mutex> lock(pending_mu);
        waiters.insert(w);
    }

    void unregister_waiter(IsoWaiter* w) {
        std::lock_guard<std::mutex> lock(pending_mu);
        waiters.erase(w);
    }

    void add_pending(uint32_t seq, TransferSlot* slot) {
        TransferSlot* prev = nullptr;
        {
            std::lock_guard<std::mutex> lock(pending_mu);
            auto it = pending.find(seq);
            if (it != pending.end()) prev = it->second;
            pending[seq] = slot;
        }
        if (prev) prev->cancel();
    }

    void remove_pending(uint32_t seq) {
        std::lock_guard<std::mutex> lock(pending_mu);
        pending.erase(seq);
    }

    bool cancel_pending(uint32_t seq) {
        TransferSlot* slot = nullptr;
        {
            std::lock_guard<std::mutex> lock(pending_mu);
            auto it = pending.find(seq);
            if (it != pending.end()) slot = it->second;
        }
        if (!slot) return false;
        slot->cancel();
        return true;
    }

    void cancel_all() {
        closed.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(pending_mu);
            for (auto& kv : pending) {
                if (kv.second) kv.second->cancel();
            }
            for (auto* w : waiters) {
                if (w) w->abort();
            }
        }
        ep1_queue.close();
        ep2_queue.close();
        sock.abort();
    }

    bool write_submit(const SubmitRequest& req, int32_t status, uint32_t actual,
                      const uint8_t* data, size_t data_len,
                      const std::vector<IsoPacket>& packets, uint32_t err_count) {
        std::lock_guard<std::mutex> lock(frame_mu);
        encode_ret_submit(write_frame, req, status, actual, data, data_len,
                          packets.data(), packets.size(), err_count);
        return sock.write_all(write_frame.data(), write_frame.size());
    }

    bool write_unlink(const BasicHeader& req, int32_t status) {
        std::lock_guard<std::mutex> lock(frame_mu);
        encode_ret_unlink(write_frame, req, status);
        return sock.write_all(write_frame.data(), write_frame.size());
    }
};

bool process_submit(ConnectionContext& ctx, TransferSlot& slot, IsoWaiter* waiter,
                    std::vector<uint8_t>& scratch, std::vector<uint8_t>& ctrl_resp) {
    const auto& req = slot.req;

    if (req.basic.endpoint == 0) {
        SetupPacket setup;
        if (!parse_setup(req.setup.data(), req.setup.size(), setup)) {
            return ctx.write_submit(req, kStatusInvalid, 0, nullptr, 0, {}, 0);
        }
        if ((setup.request == kReqClearFeature && (setup.request_type & 0x1F) == 0x02) ||
            setup.request == kReqSetInterface) {
            ctx.scheduler.reset(static_cast<uint32_t>(setup.index & 0x0F));
        }
        const int32_t status = ctx.device.handle_control(
            setup, slot.out.data(), slot.req.basic.direction == kDirectionOut ? slot.req.transfer_buffer_length : 0, ctrl_resp);
        if (ctrl_resp.size() > req.transfer_buffer_length) {
            ctrl_resp.resize(req.transfer_buffer_length);
        }
        uint32_t actual = static_cast<uint32_t>(ctrl_resp.size());
        const uint8_t* data_ptr = ctrl_resp.data();
        size_t data_len = ctrl_resp.size();
        if (req.basic.direction == kDirectionOut && status == kStatusOK) {
            actual = req.transfer_buffer_length;
            data_ptr = nullptr;
            data_len = 0;
        }
        return ctx.write_submit(req, status, actual, data_ptr, data_len, {}, 0);
    }

    if (req.is_isochronous()) {
        if (ctx.closed.load(std::memory_order_acquire) || slot.cancelled.load(std::memory_order_acquire)) {
            mark_iso_error(slot.packets, kStatusConnReset);
            return ctx.write_submit(req, kStatusConnReset, 0, nullptr, 0,
                                    slot.packets, static_cast<uint32_t>(slot.packets.size()));
        }
    }

    auto wait_slot = [&]() -> bool {
        if (!req.is_isochronous() || !waiter) return true;
        waiter->reset_abort();
        if (!slot.attach_waiter(waiter)) return false;
        const bool ok = waiter->wait_until(slot.complete_at, ctx.closed, slot.cancelled);
        slot.detach_waiter();
        if (ok) {
            const auto late_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - slot.complete_at).count();
            if (late_us > 15000) {
                char buf[128]{};
                std::snprintf(buf, sizeof(buf), "%s STALL_WAKE ep=%u late=%.3fms",
                              ctx.device.bus_id().c_str(), req.basic.endpoint, static_cast<double>(late_us) / 1000.0);
                ctx.log(buf);
            }
        }
        return ok;
    };

    if (req.basic.endpoint == 1 && req.basic.direction == kDirectionOut) {
        size_t actual = req.transfer_buffer_length;
        if (ctx.device.playback_active()) {
            if (slot.packets.empty()) {
                actual = ctx.device.write_playback(slot.out.data(), actual);
            } else {
                actual = 0;
                for (const auto& pkt : slot.packets) {
                    actual += ctx.device.write_playback(slot.out.data() + pkt.offset, pkt.length);
                }
            }
        }
        mark_iso_packets(slot.packets, static_cast<int>(actual));
        if (!wait_slot()) {
            mark_iso_error(slot.packets, kStatusConnReset);
            return ctx.write_submit(req, kStatusConnReset, 0, nullptr, 0,
                                    slot.packets, static_cast<uint32_t>(slot.packets.size()));
        }
        const auto w0 = Clock::now();
        const bool ok = ctx.write_submit(req, kStatusOK, static_cast<uint32_t>(actual), nullptr, 0, slot.packets, 0);
        const auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - w0).count();
        if (dur_us > 15000) {
            char buf[128]{};
            std::snprintf(buf, sizeof(buf), "%s STALL_WRITE ep=%u dur=%.3fms",
                          ctx.device.bus_id().c_str(), req.basic.endpoint, static_cast<double>(dur_us) / 1000.0);
            ctx.log(buf);
        }
        return ok;
    }

    if (req.basic.endpoint == 2 && req.basic.direction == kDirectionIn) {
        if (!wait_slot()) {
            mark_iso_error(slot.packets, kStatusConnReset);
            return ctx.write_submit(req, kStatusConnReset, 0, nullptr, 0,
                                    slot.packets, static_cast<uint32_t>(slot.packets.size()));
        }
        const int frame_bytes = static_cast<int>(kUacChannels) * ctx.device.pcm_bytes();
        const int packet_bytes = static_cast<int>(kUacSampleRate / 1000) * frame_bytes;
        int length = 0;
        if (slot.packets.empty()) {
            length = static_cast<int>(req.transfer_buffer_length) / frame_bytes * frame_bytes;
        } else {
            for (auto& pkt : slot.packets) {
                int wanted = std::min(static_cast<int>(pkt.length), packet_bytes);
                wanted -= wanted % frame_bytes;
                pkt.actual_length = static_cast<uint32_t>(wanted);
                pkt.status = kStatusOK;
                length += wanted;
            }
        }
        scratch.resize(static_cast<size_t>(length));
        std::memset(scratch.data(), 0, static_cast<size_t>(length));
        if (ctx.device.capture_active() && length > 0) {
            ctx.device.read_capture(scratch.data(), static_cast<size_t>(length));
        }
        const auto w0 = Clock::now();
        const bool ok = ctx.write_submit(req, kStatusOK, static_cast<uint32_t>(length),
                                         scratch.data(), static_cast<size_t>(length), slot.packets, 0);
        const auto dur_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - w0).count();
        if (dur_us > 15000) {
            char buf[128]{};
            std::snprintf(buf, sizeof(buf), "%s STALL_WRITE ep=%u dur=%.3fms",
                          ctx.device.bus_id().c_str(), req.basic.endpoint, static_cast<double>(dur_us) / 1000.0);
            ctx.log(buf);
        }
        return ok;
    }

    mark_iso_error(slot.packets, kStatusPipe);
    return ctx.write_submit(req, kStatusPipe, 0, nullptr, 0,
                            slot.packets, static_cast<uint32_t>(slot.packets.size()));
}

void run_endpoint_worker(ConnectionContext& ctx, IsoQueue& queue) {
    std::unique_ptr<core::ScopedMmcss> mmcss;
    IsoWaiter waiter;
    ctx.register_waiter(&waiter);

    std::vector<uint8_t> scratch(64 * 1024);
    std::vector<uint8_t> ctrl_resp;
    std::vector<TransferSlot*> drained;

    while (TransferSlot* slot = queue.pop(drained)) {
        if (!mmcss) {
            mmcss = std::make_unique<core::ScopedMmcss>(L"Pro Audio");
        }
        const bool ok = process_submit(ctx, *slot, &waiter, scratch, ctrl_resp);
        ctx.remove_pending(slot->req.basic.sequence);
        ctx.pool.release(slot);
        if (!ok && !ctx.closed.load(std::memory_order_acquire)) {
            ctx.log(ctx.device.bus_id() + " endpoint worker failed");
            ctx.cancel_all();
            break;
        }
    }

    for (TransferSlot* s : drained) {
        s->cancel();
        ctx.remove_pending(s->req.basic.sequence);
        ctx.pool.release(s);
    }
    drained.clear();
    queue.drain_remaining(drained);
    for (TransferSlot* s : drained) {
        s->cancel();
        ctx.remove_pending(s->req.basic.sequence);
        ctx.pool.release(s);
    }

    ctx.unregister_waiter(&waiter);
}

void trace_submit(ConnectionContext& ctx, const SubmitRequest& req) {
    if (req.basic.endpoint == 0) {
        SetupPacket setup;
        if (parse_setup(req.setup.data(), req.setup.size(), setup)) {
            if (setup.request == kReqClearFeature && (setup.request_type & 0x1F) == 0x02) {
                return;
            }
            char buf[160]{};
            std::snprintf(buf, sizeof(buf),
                          "%s CTRL seq=%u dir=%s bmRequestType=0x%02X bRequest=0x%02X wValue=0x%04X wIndex=0x%04X wLength=%u",
                          ctx.device.bus_id().c_str(), req.basic.sequence,
                          (req.basic.direction == kDirectionIn ? "IN" : "OUT"),
                          setup.request_type, setup.request, setup.value, setup.index, setup.length);
            ctx.log(buf);
        }
        return;
    }
    const uint64_t count = ctx.urb_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (count <= 20) {
        char buf[160]{};
        std::snprintf(buf, sizeof(buf), "%s URB #%llu seq=%u ep=%u dir=%s bytes=%u iso_packets=%u",
                      ctx.device.bus_id().c_str(), static_cast<unsigned long long>(count),
                      req.basic.sequence, req.basic.endpoint,
                      (req.basic.direction == kDirectionIn ? "IN" : "OUT"),
                      req.transfer_buffer_length, req.is_isochronous() ? req.number_of_packets : 0u);
        ctx.log(buf);
    }
}

} // namespace

Clock::time_point IsoScheduler::reserve(uint32_t endpoint, uint32_t packets, Clock::time_point now) noexcept {
    if (packets == 0) return now;
    const auto duration = std::chrono::milliseconds(std::min<uint32_t>(packets, 250));
    std::lock_guard<std::mutex> lock(mu_);
    auto it = iso_next_.find(endpoint);
    Clock::time_point base = now;
    if (it != iso_next_.end() && it->second >= now - std::chrono::milliseconds(25)) {
        base = it->second;
    }
    const Clock::time_point deadline = base + duration;
    iso_next_[endpoint] = deadline;
    return deadline;
}

void IsoScheduler::reset(uint32_t endpoint) noexcept {
    std::lock_guard<std::mutex> lock(mu_);
    iso_next_.erase(endpoint);
}

UsbipServer::UsbipServer(std::string address, std::vector<std::unique_ptr<Uac1Device>> devices, LogFn logger)
    : address_(std::move(address)), devices_(std::move(devices)), logger_(std::move(logger)) {}

UsbipServer::~UsbipServer() {
    stop();
}

void UsbipServer::log(const std::string& msg) const {
    if (logger_) logger_(msg);
}

bool UsbipServer::start(std::string* err) {
    enable_process_realtime();
    std::string host;
    uint16_t port = 3241;
    if (!parse_listen_address(address_, host, port)) {
        if (err) *err = "invalid USB/IP listen address: " + address_;
        return false;
    }

    SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
    if (s == INVALID_SOCKET) {
        if (err) *err = "WSASocketW failed: " + std::to_string(WSAGetLastError());
        return false;
    }

    const BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        closesocket(s);
        if (err) *err = "invalid IPv4 bind address: " + host;
        return false;
    }
    if (bind(s, reinterpret_cast<const sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
        const int werr = WSAGetLastError();
        closesocket(s);
        if (err) *err = "bind(" + address_ + ") failed: WSA error " + std::to_string(werr);
        return false;
    }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        const int werr = WSAGetLastError();
        closesocket(s);
        if (err) *err = "listen failed: WSA error " + std::to_string(werr);
        return false;
    }

    sockaddr_in bound{};
    int bound_len = sizeof(bound);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        bound_port_.store(ntohs(bound.sin_port), std::memory_order_release);
    }

    {
        std::lock_guard<std::mutex> lock(listen_mu_);
        listen_sock_ = s;
        stop_requested_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
    }

    log("USB/IP server listening on " + host + ":" + std::to_string(bound_port_.load()) +
        " with " + std::to_string(devices_.size()) + " device(s)");
    accept_thread_ = std::thread(&UsbipServer::accept_loop, this);
    return true;
}

void UsbipServer::stop() {
    stop_requested_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(listen_mu_);
        if (listen_sock_ != INVALID_SOCKET) {
            closesocket(listen_sock_);
            listen_sock_ = INVALID_SOCKET;
        }
    }
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(conn_mu_);
        for (RtTcpSocket* sock : active_sockets_) {
            if (sock) sock->abort();
        }
    }
    std::vector<std::thread> to_join;
    {
        std::lock_guard<std::mutex> lock(conn_mu_);
        to_join.swap(conn_threads_);
    }
    for (auto& t : to_join) {
        if (t.joinable()) t.join();
    }
    running_.store(false, std::memory_order_release);
}

void UsbipServer::accept_loop() {
    while (!stop_requested_.load(std::memory_order_acquire)) {
        SOCKET s = INVALID_SOCKET;
        {
            std::lock_guard<std::mutex> lock(listen_mu_);
            s = listen_sock_;
        }
        if (s == INVALID_SOCKET) break;

        sockaddr_storage ss{};
        int ss_len = sizeof(ss);
        SOCKET client = accept(s, reinterpret_cast<sockaddr*>(&ss), &ss_len);
        if (client == INVALID_SOCKET) {
            if (stop_requested_.load(std::memory_order_acquire)) break;
            Sleep(5);
            continue;
        }
        std::string remote = format_remote(ss);
        std::lock_guard<std::mutex> lock(conn_mu_);
        conn_threads_.emplace_back(&UsbipServer::handle_connection, this, client, std::move(remote));
    }
}

Uac1Device* UsbipServer::find_device(const std::string& bus_id) const noexcept {
    for (const auto& d : devices_) {
        if (d && d->bus_id() == bus_id) return d.get();
    }
    return nullptr;
}

bool UsbipServer::write_device_list(RtTcpSocket& sock) {
    std::vector<uint8_t> buf;
    buf.reserve(64 + devices_.size() * 324);
    encode_op_header(buf, kOpRepDevlist, 0);
    const size_t cnt_pos = buf.size();
    buf.resize(cnt_pos + 4);
    write_be32(buf.data() + cnt_pos, static_cast<uint32_t>(devices_.size()));

    static constexpr uint8_t kIfaces[3][4] = {
        {0x01, 0x01, 0x00, 0},
        {0x01, 0x02, 0x00, 0},
        {0x01, 0x02, 0x00, 0},
    };
    for (const auto& d : devices_) {
        write_usb_device(buf, *d);
        for (const auto& iface : kIfaces) {
            buf.insert(buf.end(), iface, iface + 4);
        }
    }
    return sock.write_all(buf.data(), buf.size());
}

void UsbipServer::handle_connection(SOCKET client_sock, std::string remote_addr) {
    RtTcpSocket sock(client_sock);
    sock.configure_realtime();
    sock.set_io_timeout_ms(30000);
    {
        std::lock_guard<std::mutex> lock(conn_mu_);
        active_sockets_.insert(&sock);
    }
    struct SocketGuard {
        UsbipServer* srv;
        RtTcpSocket* s;
        ~SocketGuard() {
            std::lock_guard<std::mutex> lock(srv->conn_mu_);
            srv->active_sockets_.erase(s);
        }
    } sock_guard{this, &sock};

    uint8_t hdr_wire[8]{};
    if (!sock.read_exact(hdr_wire, sizeof(hdr_wire))) {
        log("management handshake from " + remote_addr + " failed");
        return;
    }
    const OpHeader hdr = decode_op_header(hdr_wire);
    if (hdr.version != kProtocolVersion) {
        log("unsupported USB/IP version from " + remote_addr);
        return;
    }

    if (hdr.code == kOpReqDevlist) {
        log("device-list request from " + remote_addr);
        if (!write_device_list(sock)) {
            log("device-list reply failed");
        }
        return;
    }

    if (hdr.code != kOpReqImport) {
        log("unsupported management opcode from " + remote_addr);
        return;
    }

    char bus_raw[32]{};
    if (!sock.read_exact(reinterpret_cast<uint8_t*>(bus_raw), sizeof(bus_raw))) {
        log("import request failed from " + remote_addr);
        return;
    }
    const size_t nlen = ::strnlen(bus_raw, sizeof(bus_raw));
    const std::string bus_id(bus_raw, nlen);
    Uac1Device* device = find_device(bus_id);
    if (!device) {
        std::vector<uint8_t> rep;
        encode_op_header(rep, kOpRepImport, 1);
        sock.write_all(rep.data(), rep.size());
        log("client requested unknown bus ID " + bus_id);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(conn_mu_);
        if (imported_[bus_id]) {
            std::vector<uint8_t> rep;
            encode_op_header(rep, kOpRepImport, 1);
            sock.write_all(rep.data(), rep.size());
            return;
        }
        imported_[bus_id] = true;
    }

    struct ImportGuard {
        UsbipServer* srv;
        Uac1Device* dev;
        std::string id;
        ~ImportGuard() {
            std::vector<uint8_t> dummy;
            SetupPacket reset_cfg{0, kReqSetConfiguration, 0, 0, 0};
            dev->handle_control(reset_cfg, nullptr, 0, dummy);
            std::lock_guard<std::mutex> lock(srv->conn_mu_);
            srv->imported_.erase(id);
        }
    } import_guard{this, device, bus_id};

    {
        std::vector<uint8_t> rep;
        rep.reserve(320);
        encode_op_header(rep, kOpRepImport, 0);
        write_usb_device(rep, *device);
        if (!sock.write_all(rep.data(), rep.size())) {
            return;
        }
    }

    sock.set_io_timeout_ms(INFINITE);
    log("import accepted: " + device->bus_id() + " (" + device->product() + "), client " + remote_addr);

    core::ScopedMmcss reader_mmcss(L"Pro Audio");
    ConnectionContext ctx(sock, *device, logger_, urb_count_);
    std::thread ep1_worker(run_endpoint_worker, std::ref(ctx), std::ref(ctx.ep1_queue));
    std::thread ep2_worker(run_endpoint_worker, std::ref(ctx), std::ref(ctx.ep2_queue));

    std::vector<uint8_t> iso_raw(4096);
    std::vector<uint8_t> ctrl_resp;
    Clock::time_point last_arrival[3]{};
    Clock::time_point last_arr_log[3]{};

    while (!stop_requested_.load(std::memory_order_acquire)) {
        uint8_t basic_wire[20]{};
        if (!sock.read_exact(basic_wire, sizeof(basic_wire))) {
            if (!sock.last_read_eof() && !stop_requested_.load(std::memory_order_acquire)) {
                log(device->bus_id() + " disconnected");
            }
            break;
        }
        const BasicHeader basic = decode_basic_header(basic_wire);

        if (basic.command == kCmdSubmit) {
            uint8_t body_wire[28]{};
            if (!sock.read_exact(body_wire, sizeof(body_wire))) {
                break;
            }
            SubmitRequest req = decode_submit_body(basic, body_wire);
            if (req.transfer_buffer_length > kMaxTransferLength) {
                log(device->bus_id() + " invalid transfer length");
                break;
            }

            TransferSlot* slot = ctx.pool.acquire();
            slot->req = req;
            if (basic.direction == kDirectionOut && req.transfer_buffer_length > 0) {
                slot->out.resize(req.transfer_buffer_length);
                if (!sock.read_exact(slot->out.data(), req.transfer_buffer_length)) {
                    ctx.pool.release(slot);
                    break;
                }
            }
            if (req.is_isochronous()) {
                if (req.number_of_packets > 4096) {
                    ctx.pool.release(slot);
                    break;
                }
                const size_t iso_bytes = static_cast<size_t>(req.number_of_packets) * 16;
                if (iso_raw.size() < iso_bytes) iso_raw.resize(iso_bytes);
                if (!sock.read_exact(iso_raw.data(), iso_bytes)) {
                    ctx.pool.release(slot);
                    break;
                }
                decode_iso_packets(iso_raw.data(), req.number_of_packets, slot->packets);
                bool valid_packets = true;
                for (const auto& pkt : slot->packets) {
                    if (static_cast<uint64_t>(pkt.offset) + pkt.length > req.transfer_buffer_length) {
                        valid_packets = false;
                        break;
                    }
                }
                if (!valid_packets) {
                    ctx.pool.release(slot);
                    break;
                }
            } else {
                slot->packets.clear();
            }

            trace_submit(ctx, slot->req);

            if (basic.endpoint == 0 || !slot->req.is_isochronous()) {
                const bool ok = process_submit(ctx, *slot, nullptr, iso_raw, ctrl_resp);
                ctx.pool.release(slot);
                if (!ok) break;
                continue;
            }

            if (basic.endpoint != 1 && basic.endpoint != 2) {
                ctx.pool.release(slot);
                break;
            }

            const Clock::time_point now = Clock::now();
            const Clock::time_point prev = last_arrival[basic.endpoint];
            if (prev != Clock::time_point{}) {
                const auto gap_us = std::chrono::duration_cast<std::chrono::microseconds>(now - prev).count();
                if (gap_us > 25000 && (now - last_arr_log[basic.endpoint]) > std::chrono::milliseconds(500)) {
                    last_arr_log[basic.endpoint] = now;
                    char msg[128]{};
                    std::snprintf(msg, sizeof(msg), "%s STALL_ARRIVAL ep=%u gap=%.3fms",
                                  device->bus_id().c_str(), basic.endpoint, static_cast<double>(gap_us) / 1000.0);
                    log(msg);
                }
            }
            last_arrival[basic.endpoint] = now;

            slot->complete_at = ctx.scheduler.reserve(basic.endpoint, slot->req.number_of_packets, now);
            ctx.add_pending(basic.sequence, slot);
            if (basic.endpoint == 1) {
                ctx.ep1_queue.push(slot);
            } else {
                ctx.ep2_queue.push(slot);
            }
        } else if (basic.command == kCmdUnlink) {
            uint8_t unlink_wire[28]{};
            if (!sock.read_exact(unlink_wire, sizeof(unlink_wire))) {
                break;
            }
            const uint32_t target = read_be32(unlink_wire);
            const bool found = ctx.cancel_pending(target);
            const int32_t status = found ? kStatusConnReset : kStatusOK;
            char msg[128]{};
            std::snprintf(msg, sizeof(msg), "%s UNLINK seq=%u target=%u found=%s",
                          device->bus_id().c_str(), basic.sequence, target, found ? "true" : "false");
            log(msg);
            if (!ctx.write_unlink(basic, status)) {
                break;
            }
        } else {
            log(device->bus_id() + " unsupported URB command");
            break;
        }
    }

    ctx.cancel_all();
    if (ep1_worker.joinable()) ep1_worker.join();
    if (ep2_worker.joinable()) ep2_worker.join();
    log("USB/IP session closed for " + device->bus_id());
}

} // namespace metvoice::usbip
