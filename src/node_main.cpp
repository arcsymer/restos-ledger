// restos-node — hosts one Raft node over TCP. Single-threaded select() loop, so no data races.
//
//   restos-node --id 0 --port 5000 --peers 1:127.0.0.1:5001,2:127.0.0.1:5002 --data ./data
//
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <sstream>
#include <string>

#include "restos/net.hpp"

using namespace restos;

namespace {
struct Addr {
    std::string host;
    uint16_t port;
};

std::string arg(int argc, char** argv, const std::string& key, const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i)
        if (key == argv[i]) return argv[i + 1];
    return def;
}

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string handle_client(RaftNode& node, KVStateMachine& sm, const std::string& cmd) {
    std::istringstream in(cmd);
    std::string op;
    in >> op;
    if (op == "put" || op == "del") {
        if (node.role() != Role::Leader)
            return "REDIRECT " + std::to_string(node.leader_hint());
        uint64_t idx = 0;
        node.client_append(cmd, &idx);
        return "OK " + std::to_string(idx);
    }
    if (op == "get") {
        if (node.role() != Role::Leader)
            return "REDIRECT " + std::to_string(node.leader_hint());
        std::string key;
        in >> key;
        auto v = sm.get(key);
        return v ? "VALUE " + *v : "NULL";
    }
    return "ERR unknown command";
}
}  // namespace

int main(int argc, char** argv) {
    net::startup();
    int id = std::atoi(arg(argc, argv, "--id", "0").c_str());
    uint16_t port = static_cast<uint16_t>(std::atoi(arg(argc, argv, "--port", "5000").c_str()));
    std::string data = arg(argc, argv, "--data", ".");
    std::string peers_arg = arg(argc, argv, "--peers", "");

    std::map<int, Addr> addrs;
    std::vector<int> peer_ids;
    std::stringstream ps(peers_arg);
    std::string tok;
    while (std::getline(ps, tok, ',')) {
        if (tok.empty()) continue;
        auto c1 = tok.find(':');
        auto c2 = tok.find(':', c1 + 1);
        int pid = std::atoi(tok.substr(0, c1).c_str());
        Addr a{tok.substr(c1 + 1, c2 - c1 - 1),
               static_cast<uint16_t>(std::atoi(tok.substr(c2 + 1).c_str()))};
        addrs[pid] = a;
        peer_ids.push_back(pid);
    }

    KVStateMachine sm;
    LogStore store(data + "/node_" + std::to_string(id) + ".log");
    auto send = [&addrs](const Message& m) {
        auto it = addrs.find(m.to);
        if (it == addrs.end()) return;
        net::sock_t s = net::connect_to(it->second.host, it->second.port);
        if (s == net::kInvalid) return;  // peer unreachable — Raft tolerates lost messages
        net::send_all(s, encode(m));
        net::close_sock(s);
    };
    RaftNode node(id, peer_ids, send, &sm, &store);

    net::sock_t listener = net::make_listener(port);
    if (listener == net::kInvalid) {
        std::cerr << "failed to listen on port " << port << "\n";
        return 1;
    }
    std::cout << "restos-node " << id << " listening on " << port << " with " << peer_ids.size()
              << " peers\n";

    uint64_t start = now_ms();
    for (;;) {
        if (net::wait_readable(listener, 5)) {
            net::sock_t c = net::accept_conn(listener);
            if (c != net::kInvalid) {
                std::string buf;
                if (net::recv_framed(c, buf) && buf.size() > 8) {
                    if (static_cast<unsigned char>(buf[8]) == 0xFF) {
                        std::string reply = handle_client(node, sm, buf.substr(9));
                        net::send_all(c, frame_client(reply));
                    } else {
                        Message m;
                        if (decode(buf, m)) node.receive(m);
                    }
                }
                net::close_sock(c);
            }
        }
        node.tick(now_ms() - start);
    }
}
