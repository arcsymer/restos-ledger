#pragma once
#include <deque>
#include <memory>
#include <set>
#include <vector>

#include "restos/raft.hpp"

namespace restos {

// Deterministic in-process cluster: an internal message bus and a manual clock. No sockets, no
// wall time, no threads — so Raft behaviour is fully reproducible in tests. Nodes can be marked
// "down" to simulate crashes/partitions (messages to or from them are dropped).
class SimCluster {
public:
    explicit SimCluster(int n, RaftConfig cfg = {}, const std::vector<std::string>& paths = {}) {
        for (int i = 0; i < n; ++i) {
            sms_.push_back(std::make_unique<KVStateMachine>());
            stores_.push_back(std::make_unique<LogStore>(i < (int)paths.size() ? paths[i] : ""));
        }
        for (int i = 0; i < n; ++i) {
            std::vector<int> peers;
            for (int j = 0; j < n; ++j)
                if (j != i) peers.push_back(j);
            auto send = [this](const Message& m) {
                if (down_.count(m.from) || down_.count(m.to)) return;
                bus_.push_back(m);
            };
            nodes_.push_back(std::make_unique<RaftNode>(i, peers, send, sms_[i].get(),
                                                        stores_[i].get(), cfg));
        }
    }

    RaftNode& node(int i) { return *nodes_[i]; }
    KVStateMachine& sm(int i) { return *sms_[i]; }
    LogStore& store(int i) { return *stores_[i]; }
    int size() const { return static_cast<int>(nodes_.size()); }

    void set_down(int i, bool down) {
        if (down)
            down_.insert(i);
        else
            down_.erase(i);
    }

    // Advance the clock by `ms` in `dt`-sized steps, ticking live nodes and draining the bus.
    void advance(uint64_t ms, uint64_t dt = 1) {
        for (uint64_t elapsed = 0; elapsed < ms; elapsed += dt) {
            clock_ += dt;
            for (int i = 0; i < size(); ++i)
                if (!down_.count(i)) nodes_[i]->tick(clock_);
            drain();
        }
    }

    void drain() {
        int guard = 0;
        while (!bus_.empty() && guard++ < 100000) {
            Message m = bus_.front();
            bus_.pop_front();
            if (down_.count(m.from) || down_.count(m.to)) continue;
            nodes_[m.to]->receive(m);
        }
    }

    int leader_count() const {
        int c = 0;
        for (auto& n : nodes_)
            if (n->role() == Role::Leader) ++c;
        return c;
    }
    int leader_id() const {
        for (auto& n : nodes_)
            if (n->role() == Role::Leader) return n->id();
        return -1;
    }

private:
    std::vector<std::unique_ptr<KVStateMachine>> sms_;
    std::vector<std::unique_ptr<LogStore>> stores_;
    std::vector<std::unique_ptr<RaftNode>> nodes_;
    std::deque<Message> bus_;
    std::set<int> down_;
    uint64_t clock_ = 0;
};

}  // namespace restos
