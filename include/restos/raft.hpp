#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "restos/kv.hpp"
#include "restos/log.hpp"

namespace restos {

enum class Role { Follower, Candidate, Leader };

enum class MsgType : uint8_t {
    RequestVote,
    RequestVoteReply,
    AppendEntries,
    AppendEntriesReply,
};

struct Message {
    MsgType type{};
    int from = 0;
    int to = 0;
    uint64_t term = 0;

    // RequestVote
    uint64_t last_log_index = 0;
    uint64_t last_log_term = 0;
    // RequestVoteReply
    bool vote_granted = false;
    // AppendEntries
    uint64_t prev_log_index = 0;
    uint64_t prev_log_term = 0;
    std::vector<LogEntry> entries;
    uint64_t leader_commit = 0;
    // AppendEntriesReply
    bool success = false;
    uint64_t match_index = 0;  // follower's last index that matches the leader on success
};

using SendFn = std::function<void(const Message&)>;

struct RaftConfig {
    uint64_t election_min_ms = 150;
    uint64_t election_max_ms = 300;
    uint64_t heartbeat_ms = 50;
};

// Pure Raft logic. No sockets, no wall clock — time is injected via tick(now_ms) and messages
// via receive(). Outgoing messages go through the SendFn. Deterministically testable.
class RaftNode {
public:
    RaftNode(int id, std::vector<int> peers, SendFn send, KVStateMachine* sm, LogStore* store,
             RaftConfig cfg = {});

    void tick(uint64_t now_ms);
    void receive(const Message& m);

    // Leader-only: append a client command. Returns false (and does nothing) if not leader.
    bool client_append(const std::string& command, uint64_t* out_index = nullptr);

    Role role() const { return role_; }
    int id() const { return id_; }
    uint64_t term() const { return current_term_; }
    uint64_t commit_index() const { return commit_index_; }
    int leader_hint() const { return leader_id_; }

    // Test hook: force the next election timeout (deterministic, bypasses randomization).
    void set_election_timeout(uint64_t ms) { election_timeout_ms_ = ms; }

private:
    void persist();
    void become_follower(uint64_t term);
    void become_candidate();
    void become_leader();
    void start_election();
    void send_heartbeats();
    void send_append_to(int peer);
    void advance_commit();
    void apply_committed();
    void reset_election_timer();
    uint64_t rand_timeout();
    bool log_up_to_date(uint64_t cand_last_index, uint64_t cand_last_term) const;

    void on_request_vote(const Message& m);
    void on_request_vote_reply(const Message& m);
    void on_append_entries(const Message& m);
    void on_append_entries_reply(const Message& m);

    int id_;
    std::vector<int> peers_;
    SendFn send_;
    KVStateMachine* sm_;
    LogStore* store_;
    RaftConfig cfg_;

    Role role_ = Role::Follower;
    uint64_t current_term_ = 0;
    int64_t voted_for_ = -1;
    int leader_id_ = -1;

    uint64_t commit_index_ = 0;
    uint64_t last_applied_ = 0;

    // leader state
    std::map<int, uint64_t> next_index_;
    std::map<int, uint64_t> match_index_;

    // election state
    std::map<int, bool> votes_;

    // timing
    uint64_t now_ms_ = 0;
    uint64_t election_deadline_ = 0;
    uint64_t last_heartbeat_ = 0;
    uint64_t election_timeout_ms_ = 0;  // 0 => randomize
    uint64_t rng_state_ = 0;
};

}  // namespace restos
