#pragma once

#include <chrono>
#include <cctype>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "mini_redis/sharded_cache.hpp"

namespace mini_redis {

struct CommandResponse {
    bool ok{true};
    bool should_exit{false};
    std::string payload;
};

inline std::string trim(const std::string& input) {
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])) != 0) {
        ++start;
    }

    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])) != 0) {
        --end;
    }

    return input.substr(start, end - start);
}

inline std::string to_upper(std::string value) {
    for (char& c : value) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return value;
}

inline std::vector<std::string> split_ws(const std::string& input) {
    std::istringstream iss(input);
    std::vector<std::string> parts;
    std::string token;
    while (iss >> token) {
        parts.push_back(token);
    }
    return parts;
}

class RedisCommandProcessor {
public:
    using Cache = ShardedCache<std::string, std::string, 64>;

    explicit RedisCommandProcessor(std::size_t capacity)
        : cache_(capacity, true) {}

    [[nodiscard]] Cache& cache() noexcept {
        return cache_;
    }

    [[nodiscard]] const Cache& cache() const noexcept {
        return cache_;
    }

    [[nodiscard]] CommandResponse execute_line(const std::string& raw_line) {
        const auto line = trim(raw_line);
        if (line.empty()) {
            return {true, false, "(empty command)"};
        }

        const auto parts = split_ws(line);
        if (parts.empty()) {
            return {true, false, "(empty command)"};
        }

        const auto cmd = to_upper(parts[0]);

        if (cmd == "PING") {
            return {true, false, "PONG"};
        }

        if (cmd == "SET") {
            if (parts.size() < 3 || parts.size() > 4) {
                return {false, false, "ERR usage: SET <key> <value> [ttl_ms]"};
            }

            std::optional<std::chrono::milliseconds> ttl = std::nullopt;
            if (parts.size() == 4) {
                std::size_t pos = 0;
                const long long ttl_ms = std::stoll(parts[3], &pos);
                if (pos != parts[3].size() || ttl_ms < 0) {
                    return {false, false, "ERR ttl_ms must be a non-negative integer"};
                }
                ttl = std::chrono::milliseconds(ttl_ms);
            }

            const bool ok = cache_.set(parts[1], parts[2], ttl);
            return {ok, false, ok ? "OK" : "ERR set failed"};
        }

        if (cmd == "GET") {
            if (parts.size() != 2) {
                return {false, false, "ERR usage: GET <key>"};
            }

            const auto value = cache_.get(parts[1]);
            if (!value.has_value()) {
                return {true, false, "(nil)"};
            }
            return {true, false, *value};
        }

        if (cmd == "DEL") {
            if (parts.size() != 2) {
                return {false, false, "ERR usage: DEL <key>"};
            }
            const bool deleted = cache_.del(parts[1]);
            return {true, false, deleted ? "1" : "0"};
        }

        if (cmd == "STATS") {
            const auto stats = cache_.stats();
            std::ostringstream oss;
            oss << "hits=" << stats.hits << ' '
                << "misses=" << stats.misses << ' '
                << "hit_ratio=" << stats.hit_ratio() << ' '
                << "evictions=" << stats.evictions << ' '
                << "expirations=" << stats.expirations << ' '
                << "active_keys=" << stats.active_keys << ' '
                << "memory_bytes=" << stats.memory_bytes;
            return {true, false, oss.str()};
        }

        if (cmd == "HELP") {
            return {
                true,
                false,
                "Commands: PING | SET <key> <value> [ttl_ms] | GET <key> | DEL <key> | STATS | HELP | QUIT"
            };
        }

        if (cmd == "QUIT" || cmd == "EXIT") {
            return {true, true, "BYE"};
        }

        return {false, false, "ERR unknown command"};
    }

private:
    Cache cache_;
};

}  // namespace mini_redis
