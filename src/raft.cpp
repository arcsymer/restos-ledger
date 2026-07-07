#include "restos/raft.hpp"

#include <algorithm>

namespace restos {

RaftNode::RaftNode(int id, std::vector<int> peers, SendFn send, KVStateMachine* sm, LogStore* store,
                   RaftConfig cfg)
    : id_(id),
      peers_(std::move(peers)),
      send_(std::move(send)),
      sm_(sm),
      store_(store),
      cfg_(cfg) {
    current_term_ = store_->current_term();
    voted_for_ = store_->voted_for();
    commit_index_ = store_->commit_index();
    rng_state_ = static_cast<uint64_t>(id_) * 2654435761u + 1;
    // rebuild the state machine from the durably-committed prefix
    for (uint64_t i = 1; i <= commit_index_; ++i) {
        if (auto e = store_->at(i)) sm_->apply(e->command);
    }
    last_applied_ = commit_index_;
}

void RaftNode::persist() { store_->save_meta(current_term_, voted_for_, commit_index_); }

uint64_t RaftNode::rand_timeout() {
    rng_state_ = rng_state_ * 6364136223846793005ULL + 1442695040888963407ULL;
    uint64_t span = cfg_.election_max_ms - cfg_.election_min_ms + 1;
    return cfg_.election_min_ms + (rng_state_ >> 33) % span;
}

void RaftNode::reset_election_timer() {
    uint64_t t = election_timeout_ms_ ? election_timeout_ms_ : rand_timeout();
    election_deadline_ = now_ms_ + t;
}

bool RaftNode::log_up_to_date(uint64_t cand_last_index, uint64_t cand_last_term) const {
    uint64_t my_index = store_->last_index();
    uint64_t my_term = store_->last_term();
    if (cand_last_term != my_term) return cand_last_term > my_term;
    return cand_last_index >= my_index;
}

void RaftNode::become_follower(uint64_t term) {
    role_ = Role::Follower;
    if (term > current_term_) {
        current_term_ = term;
        voted_for_ = -1;
    }
    persist();
}

void RaftNode::become_candidate() { role_ = Role::Candidate; }

void RaftNode::become_leader() {
    role_ = Role::Leader;
    leader_id_ = id_;
    for (int p : peers_) {
        next_index_[p] = store_->last_index() + 1;
        match_index_[p] = 0;
    }
    last_heartbeat_ = 0;  // force an immediate heartbeat on next tick/append
    send_heartbeats();
}

void RaftNode::start_election() {
    become_candidate();
    ++current_term_;
    voted_for_ = id_;
    persist();
    votes_.clear();
    votes_[id_] = true;
    reset_election_timer();
    for (int p : peers_) {
        send_(Message{MsgType::RequestVote, id_, p, current_term_, store_->last_index(),
                      store_->last_term()});
    }
    if (peers_.empty()) become_leader();  // single-node cluster elects itself
}

void RaftNode::send_append_to(int peer) {
    uint64_t next = next_index_.count(peer) ? next_index_[peer] : store_->last_index() + 1;
    uint64_t prev_index = next - 1;
    uint64_t prev_term = 0;
    if (prev_index > 0) {
        if (auto e = store_->at(prev_index)) prev_term = e->term;
    }
    Message m{MsgType::AppendEntries, id_, peer, current_term_};
    m.prev_log_index = prev_index;
    m.prev_log_term = prev_term;
    m.leader_commit = commit_index_;
    for (const auto& e : store_->entries()) {
        if (e.index >= next) m.entries.push_back(e);
    }
    send_(m);
}

void RaftNode::send_heartbeats() {
    for (int p : peers_) send_append_to(p);
    last_heartbeat_ = now_ms_;
}

void RaftNode::tick(uint64_t now_ms) {
    now_ms_ = now_ms;
    if (election_deadline_ == 0) reset_election_timer();

    if (role_ == Role::Leader) {
        if (now_ms_ - last_heartbeat_ >= cfg_.heartbeat_ms) send_heartbeats();
        return;
    }
    if (now_ms_ >= election_deadline_) start_election();
}

void RaftNode::receive(const Message& m) {
    now_ms_ = std::max(now_ms_, now_ms_);
    if (m.term > current_term_ && m.type != MsgType::RequestVoteReply) become_follower(m.term);
    switch (m.type) {
        case MsgType::RequestVote: on_request_vote(m); break;
        case MsgType::RequestVoteReply: on_request_vote_reply(m); break;
        case MsgType::AppendEntries: on_append_entries(m); break;
        case MsgType::AppendEntriesReply: on_append_entries_reply(m); break;
    }
}

void RaftNode::on_request_vote(const Message& m) {
    Message reply{MsgType::RequestVoteReply, id_, m.from, current_term_};
    reply.vote_granted = false;
    if (m.term == current_term_ && (voted_for_ == -1 || voted_for_ == m.from) &&
        log_up_to_date(m.last_log_index, m.last_log_term)) {
        voted_for_ = m.from;
        persist();
        reply.vote_granted = true;
        reset_election_timer();
    }
    send_(reply);
}

void RaftNode::on_request_vote_reply(const Message& m) {
    if (m.term > current_term_) {
        become_follower(m.term);
        return;
    }
    if (role_ != Role::Candidate || m.term != current_term_) return;
    votes_[m.from] = m.vote_granted;
    int granted = 0;
    for (auto& [_, v] : votes_) if (v) ++granted;
    int cluster = static_cast<int>(peers_.size()) + 1;
    if (granted > cluster / 2) become_leader();
}

void RaftNode::on_append_entries(const Message& m) {
    Message reply{MsgType::AppendEntriesReply, id_, m.from, current_term_};
    reply.success = false;
    reply.match_index = 0;
    if (m.term < current_term_) {
        send_(reply);
        return;
    }
    role_ = Role::Follower;
    leader_id_ = m.from;
    reset_election_timer();

    if (m.prev_log_index > 0) {
        auto e = store_->at(m.prev_log_index);
        if (!e || e->term != m.prev_log_term) {
            send_(reply);  // log mismatch — leader will back off
            return;
        }
    }
    for (const auto& e : m.entries) {
        auto existing = store_->at(e.index);
        if (existing && existing->term == e.term) continue;
        if (existing) store_->truncate_from(e.index);
        store_->append(e);
    }
    if (m.leader_commit > commit_index_) {
        commit_index_ = std::min(m.leader_commit, store_->last_index());
        persist();
        apply_committed();
    }
    reply.success = true;
    reply.match_index = m.prev_log_index + m.entries.size();
    reply.term = current_term_;
    send_(reply);
}

void RaftNode::on_append_entries_reply(const Message& m) {
    if (m.term > current_term_) {
        become_follower(m.term);
        return;
    }
    if (role_ != Role::Leader || m.term != current_term_) return;
    if (m.success) {
        match_index_[m.from] = std::max(match_index_[m.from], m.match_index);
        next_index_[m.from] = m.match_index + 1;
        advance_commit();
    } else {
        uint64_t next = next_index_.count(m.from) ? next_index_[m.from] : 1;
        next_index_[m.from] = next > 1 ? next - 1 : 1;
        send_append_to(m.from);
    }
}

void RaftNode::advance_commit() {
    uint64_t last = store_->last_index();
    for (uint64_t n = last; n > commit_index_; --n) {
        auto e = store_->at(n);
        if (!e || e->term != current_term_) continue;  // only commit current-term entries directly
        int count = 1;  // self
        for (int p : peers_) {
            if (match_index_.count(p) && match_index_[p] >= n) ++count;
        }
        int cluster = static_cast<int>(peers_.size()) + 1;
        if (count > cluster / 2) {
            commit_index_ = n;
            persist();
            apply_committed();
            break;
        }
    }
}

void RaftNode::apply_committed() {
    while (last_applied_ < commit_index_) {
        ++last_applied_;
        if (auto e = store_->at(last_applied_)) sm_->apply(e->command);
    }
}

bool RaftNode::client_append(const std::string& command, uint64_t* out_index) {
    if (role_ != Role::Leader) return false;
    LogEntry e{current_term_, store_->last_index() + 1, command};
    store_->append(e);
    if (out_index) *out_index = e.index;
    advance_commit();     // commits immediately in a single-node cluster
    send_heartbeats();    // replicate to peers
    return true;
}

}  // namespace restos
