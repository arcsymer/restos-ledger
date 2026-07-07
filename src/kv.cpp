#include "restos/kv.hpp"

#include <sstream>

namespace restos {

void KVStateMachine::apply(const std::string& command) {
    std::istringstream in(command);
    std::string op, key;
    in >> op >> key;
    if (op == "put") {
        std::string value;
        std::getline(in, value);
        if (!value.empty() && value.front() == ' ') value.erase(0, 1);
        data_[key] = value;
    } else if (op == "del") {
        data_.erase(key);
    }
}

std::optional<std::string> KVStateMachine::get(const std::string& key) const {
    auto it = data_.find(key);
    if (it == data_.end()) return std::nullopt;
    return it->second;
}

}  // namespace restos
