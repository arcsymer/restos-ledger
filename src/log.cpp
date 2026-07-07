#include "restos/log.hpp"

#include <cstring>
#include <fstream>

namespace restos {

namespace {
template <typename T>
void put_pod(std::ofstream& os, const T& v) {
    os.write(reinterpret_cast<const char*>(&v), sizeof(T));
}
template <typename T>
bool get_pod(std::ifstream& is, T& v) {
    return static_cast<bool>(is.read(reinterpret_cast<char*>(&v), sizeof(T)));
}
}  // namespace

LogStore::LogStore(std::string path) : path_(std::move(path)) {
    if (!path_.empty()) meta_path_ = path_ + ".meta";
    load();
}

void LogStore::load() {
    entries_.clear();
    if (path_.empty()) return;

    std::ifstream is(path_, std::ios::binary);
    while (is) {
        LogEntry e;
        uint32_t len = 0;
        if (!get_pod(is, e.term)) break;
        if (!get_pod(is, e.index)) break;
        if (!get_pod(is, len)) break;
        e.command.resize(len);
        if (len && !is.read(e.command.data(), len)) break;
        entries_.push_back(std::move(e));
    }

    std::ifstream ms(meta_path_, std::ios::binary);
    if (ms) {
        get_pod(ms, current_term_);
        get_pod(ms, voted_for_);
        get_pod(ms, commit_index_);
    }
}

std::optional<LogEntry> LogStore::at(uint64_t index) const {
    for (const auto& e : entries_) {
        if (e.index == index) return e;
    }
    return std::nullopt;
}

void LogStore::append(const LogEntry& e) {
    entries_.push_back(e);
    if (path_.empty()) return;
    std::ofstream os(path_, std::ios::binary | std::ios::app);
    put_pod(os, e.term);
    put_pod(os, e.index);
    put_pod(os, static_cast<uint32_t>(e.command.size()));
    os.write(e.command.data(), static_cast<std::streamsize>(e.command.size()));
    os.flush();
}

void LogStore::truncate_from(uint64_t index) {
    while (!entries_.empty() && entries_.back().index >= index) entries_.pop_back();
    rewrite_all();
}

void LogStore::rewrite_all() {
    if (path_.empty()) return;
    std::ofstream os(path_, std::ios::binary | std::ios::trunc);
    for (const auto& e : entries_) {
        put_pod(os, e.term);
        put_pod(os, e.index);
        put_pod(os, static_cast<uint32_t>(e.command.size()));
        os.write(e.command.data(), static_cast<std::streamsize>(e.command.size()));
    }
    os.flush();
}

void LogStore::save_meta(uint64_t current_term, int64_t voted_for, uint64_t commit_index) {
    current_term_ = current_term;
    voted_for_ = voted_for;
    commit_index_ = commit_index;
    write_meta();
}

void LogStore::write_meta() {
    if (meta_path_.empty()) return;
    std::ofstream os(meta_path_, std::ios::binary | std::ios::trunc);
    put_pod(os, current_term_);
    put_pod(os, voted_for_);
    put_pod(os, commit_index_);
    os.flush();
}

}  // namespace restos
