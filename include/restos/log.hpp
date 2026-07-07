#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace restos {

struct LogEntry {
    uint64_t term = 0;
    uint64_t index = 0;  // 1-based; index 0 is the reserved sentinel
    std::string command; // e.g. "put k v", "del k"
};

// Durable, append-only Raft log + persisted (currentTerm, votedFor).
// If `path` is empty the store is in-memory only (used by the pure-logic tests).
class LogStore {
public:
    explicit LogStore(std::string path = "");

    // Load persisted state into memory (called on construction / restart).
    void load();

    const std::vector<LogEntry>& entries() const { return entries_; }
    uint64_t last_index() const { return entries_.empty() ? 0 : entries_.back().index; }
    uint64_t last_term() const { return entries_.empty() ? 0 : entries_.back().term; }
    std::optional<LogEntry> at(uint64_t index) const;

    void append(const LogEntry& e);          // durable append
    void truncate_from(uint64_t index);      // drop entries with index >= `index`

    uint64_t current_term() const { return current_term_; }
    int64_t voted_for() const { return voted_for_; }
    uint64_t commit_index() const { return commit_index_; }
    void save_meta(uint64_t current_term, int64_t voted_for, uint64_t commit_index);

private:
    void rewrite_all();  // used after truncation
    void write_meta();

    std::string path_;
    std::string meta_path_;
    std::vector<LogEntry> entries_;
    uint64_t current_term_ = 0;
    int64_t voted_for_ = -1;  // -1 == none
    uint64_t commit_index_ = 0;
};

}  // namespace restos
