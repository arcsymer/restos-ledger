#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "restos/raft.hpp"

namespace restos {

// Wire (de)serialization for a Message — length-prefixed binary, portable across nodes.
std::string encode(const Message& m);
bool decode(const std::string& buf, Message& out);

struct PeerAddr {
    int id = 0;
    std::string host;
    uint16_t port = 0;
};

// Framing helper for client (non-Raft) requests: [len:8][0xFF][text].
std::string frame_client(const std::string& text);

// Minimal cross-platform TCP (Winsock on Windows, POSIX sockets elsewhere).
namespace net {
using sock_t = std::intptr_t;
inline constexpr sock_t kInvalid = -1;

void startup();  // WSAStartup on Windows; no-op elsewhere
sock_t make_listener(uint16_t port);
sock_t accept_conn(sock_t listener);
sock_t connect_to(const std::string& host, uint16_t port);
bool send_all(sock_t s, const std::string& data);
bool recv_framed(sock_t s, std::string& out);  // reads [len:8]+payload, returns the whole buffer
void close_sock(sock_t s);
bool wait_readable(sock_t s, int timeout_ms);  // select() on one socket
}  // namespace net

}  // namespace restos
