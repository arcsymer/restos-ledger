#pragma once
#include <map>
#include <optional>
#include <string>

namespace restos {

// Deterministic key-value state machine. Commands: "put <k> <v>" and "del <k>".
class KVStateMachine {
public:
    void apply(const std::string& command);
    std::optional<std::string> get(const std::string& key) const;
    std::size_t size() const { return data_.size(); }

private:
    std::map<std::string, std::string> data_;
};

}  // namespace restos
