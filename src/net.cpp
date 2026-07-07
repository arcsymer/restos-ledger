#include "restos/net.hpp"

#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_t = int;
#define RESTOS_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#define RESTOS_CLOSE ::close
#endif

namespace restos {

namespace {
void put_u64(std::string& s, uint64_t v) {
    for (int i = 0; i < 8; ++i) s.push_back(static_cast<char>((v >> (i * 8)) & 0xff));
}
uint64_t get_u64(const char*& p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(static_cast<unsigned char>(*p++)) << (i * 8);
    return v;
}
void put_str(std::string& s, const std::string& v) {
    put_u64(s, v.size());
    s += v;
}
std::string get_str(const char*& p) {
    uint64_t n = get_u64(p);
    std::string v(p, p + n);
    p += n;
    return v;
}
}  // namespace

// Little-endian, explicit field order — same on every platform (unlike raw struct dumps).
std::string encode(const Message& m) {
    std::string body;
    body.push_back(static_cast<char>(m.type));
    put_u64(body, static_cast<uint64_t>(m.from));
    put_u64(body, static_cast<uint64_t>(m.to));
    put_u64(body, m.term);
    put_u64(body, m.last_log_index);
    put_u64(body, m.last_log_term);
    body.push_back(m.vote_granted ? 1 : 0);
    put_u64(body, m.prev_log_index);
    put_u64(body, m.prev_log_term);
    put_u64(body, m.leader_commit);
    body.push_back(m.success ? 1 : 0);
    put_u64(body, m.match_index);
    put_u64(body, m.entries.size());
    for (const auto& e : m.entries) {
        put_u64(body, e.term);
        put_u64(body, e.index);
        put_str(body, e.command);
    }
    std::string out;
    put_u64(out, body.size());  // length prefix
    out += body;
    return out;
}

bool decode(const std::string& buf, Message& out) {
    if (buf.size() < 8) return false;
    const char* p = buf.data();
    uint64_t len = get_u64(p);
    if (buf.size() < 8 + len) return false;
    out = Message{};
    out.type = static_cast<MsgType>(static_cast<unsigned char>(*p++));
    out.from = static_cast<int>(get_u64(p));
    out.to = static_cast<int>(get_u64(p));
    out.term = get_u64(p);
    out.last_log_index = get_u64(p);
    out.last_log_term = get_u64(p);
    out.vote_granted = *p++ != 0;
    out.prev_log_index = get_u64(p);
    out.prev_log_term = get_u64(p);
    out.leader_commit = get_u64(p);
    out.success = *p++ != 0;
    out.match_index = get_u64(p);
    uint64_t n = get_u64(p);
    for (uint64_t i = 0; i < n; ++i) {
        LogEntry e;
        e.term = get_u64(p);
        e.index = get_u64(p);
        e.command = get_str(p);
        out.entries.push_back(std::move(e));
    }
    return true;
}

std::string frame_client(const std::string& text) {
    std::string out;
    uint64_t len = 1 + text.size();
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((len >> (i * 8)) & 0xff));
    out.push_back(static_cast<char>(0xFF));  // client discriminator
    out += text;
    return out;
}

namespace net {

namespace {
void set_timeouts(sock_t s, int ms) {
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(ms);
#else
    timeval tv{ms / 1000, (ms % 1000) * 1000};
#endif
    ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv),
                 sizeof(tv));
    ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&tv),
                 sizeof(tv));
}

bool recv_n(sock_t s, size_t n, std::string& out) {
    out.clear();
    out.reserve(n);
    char buf[4096];
    while (out.size() < n) {
        size_t want = std::min(n - out.size(), sizeof(buf));
        int r = ::recv(static_cast<int>(s), buf, static_cast<int>(want), 0);
        if (r <= 0) return false;
        out.append(buf, static_cast<size_t>(r));
    }
    return true;
}
}  // namespace

void startup() {
#ifdef _WIN32
    static bool done = false;
    if (!done) {
        WSADATA w;
        WSAStartup(MAKEWORD(2, 2), &w);
        done = true;
    }
#endif
}

sock_t make_listener(uint16_t port) {
    sock_t s = static_cast<sock_t>(::socket(AF_INET, SOCK_STREAM, 0));
    if (s == kInvalid) return kInvalid;
    int yes = 1;
    ::setsockopt(static_cast<int>(s), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes),
                 sizeof(yes));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(static_cast<int>(s), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_sock(s);
        return kInvalid;
    }
    if (::listen(static_cast<int>(s), 16) != 0) {
        close_sock(s);
        return kInvalid;
    }
    return s;
}

sock_t accept_conn(sock_t listener) {
    sock_t c = static_cast<sock_t>(::accept(static_cast<int>(listener), nullptr, nullptr));
    if (c != kInvalid) set_timeouts(c, 1000);
    return c;
}

sock_t connect_to(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    std::string h = (host == "localhost") ? "127.0.0.1" : host;
    if (::getaddrinfo(h.c_str(), std::to_string(port).c_str(), &hints, &res) != 0 || !res)
        return kInvalid;
    sock_t s = static_cast<sock_t>(::socket(res->ai_family, res->ai_socktype, res->ai_protocol));
    if (s == kInvalid) {
        ::freeaddrinfo(res);
        return kInvalid;
    }
    if (::connect(static_cast<int>(s), res->ai_addr, static_cast<socklen_t>(res->ai_addrlen)) != 0) {
        close_sock(s);
        ::freeaddrinfo(res);
        return kInvalid;
    }
    ::freeaddrinfo(res);
    set_timeouts(s, 1000);
    return s;
}

bool send_all(sock_t s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int r = ::send(static_cast<int>(s), data.data() + sent,
                       static_cast<int>(data.size() - sent), 0);
        if (r <= 0) return false;
        sent += static_cast<size_t>(r);
    }
    return true;
}

bool recv_framed(sock_t s, std::string& out) {
    std::string len_buf;
    if (!recv_n(s, 8, len_buf)) return false;
    uint64_t len = 0;
    for (int i = 0; i < 8; ++i)
        len |= static_cast<uint64_t>(static_cast<unsigned char>(len_buf[i])) << (i * 8);
    if (len > (16u << 20)) return false;  // 16 MiB sanity cap
    std::string body;
    if (!recv_n(s, len, body)) return false;
    out = len_buf + body;
    return true;
}

void close_sock(sock_t s) {
    if (s != kInvalid) RESTOS_CLOSE(static_cast<int>(s));
}

bool wait_readable(sock_t s, int timeout_ms) {
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(static_cast<int>(s), &rf);
    timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int r = ::select(static_cast<int>(s) + 1, &rf, nullptr, nullptr, &tv);
    return r > 0;
}

}  // namespace net

}  // namespace restos
