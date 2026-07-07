// restos-cli — put/get client. Knows the node addresses (id == list index) and follows the
// leader REDIRECT hint.
//
//   restos-cli --nodes 127.0.0.1:5000,127.0.0.1:5001,127.0.0.1:5002 put a hello
//   restos-cli --nodes 127.0.0.1:5000,127.0.0.1:5001,127.0.0.1:5002 get a
//
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "restos/net.hpp"

using namespace restos;

namespace {
struct Addr {
    std::string host;
    uint16_t port;
};

std::string send_once(const Addr& a, const std::string& cmd) {
    net::sock_t s = net::connect_to(a.host, a.port);
    if (s == net::kInvalid) return "ERR unreachable";
    net::send_all(s, frame_client(cmd));
    std::string buf;
    std::string reply = net::recv_framed(s, buf) && buf.size() > 8 ? buf.substr(9) : "ERR no reply";
    net::close_sock(s);
    return reply;
}
}  // namespace

int main(int argc, char** argv) {
    net::startup();
    std::vector<Addr> nodes;
    std::string cmd;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        if (s == "--nodes" && i + 1 < argc) {
            std::stringstream ss(argv[++i]);
            std::string tok;
            while (std::getline(ss, tok, ',')) {
                auto c = tok.find(':');
                nodes.push_back({tok.substr(0, c),
                                 static_cast<uint16_t>(std::atoi(tok.substr(c + 1).c_str()))});
            }
        } else {
            cmd += (cmd.empty() ? "" : " ") + s;
        }
    }
    if (nodes.empty() || cmd.empty()) {
        std::cerr << "usage: restos-cli --nodes h:p,h:p,... <put|del|get> ...\n";
        return 2;
    }

    // try each node; follow one redirect to the leader
    for (int attempt = 0; attempt < 2 * static_cast<int>(nodes.size()); ++attempt) {
        std::string reply = send_once(nodes[attempt % nodes.size()], cmd);
        if (reply.rfind("REDIRECT", 0) == 0) {
            std::istringstream in(reply);
            std::string kw;
            int leader = -1;
            in >> kw >> leader;
            if (leader >= 0 && leader < static_cast<int>(nodes.size())) {
                std::string r2 = send_once(nodes[leader], cmd);
                if (r2.rfind("REDIRECT", 0) != 0) {
                    std::cout << r2 << "\n";
                    return 0;
                }
            }
            continue;  // leader unknown yet — try another node
        }
        if (reply != "ERR unreachable") {
            std::cout << reply << "\n";
            return 0;
        }
    }
    std::cerr << "no leader reachable\n";
    return 1;
}
