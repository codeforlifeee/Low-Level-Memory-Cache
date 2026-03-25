#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <sstream>
#include <string>

#include "mini_redis/persistence.hpp"
#include "mini_redis/sharded_cache.hpp"
#include "mini_redis/tcp_command_parser.hpp"

namespace mini_redis {

struct CommandResponse {
    bool ok{true};
    bool should_exit{false};
    std::string payload;
};

struct RedisProcessorOptions {
    bool enable_persistence{true};
    std::string aof_path{"data/appendonly.aof"};
    std::string snapshot_path{"data/dump.rdb"};
    std::chrono::milliseconds aof_flush_interval{1000};
    std::size_t aof_flush_batch{256};
    std::chrono::seconds snapshot_interval{30};
};

class RedisCommandProcessor {
public:
    using Cache = ShardedCache<std::string, std::string, 64>;

    explicit RedisCommandProcessor(
        std::size_t capacity,
        RedisProcessorOptions options = RedisProcessorOptions{})
        : cache_(capacity, true), options_(std::move(options)) {
        if (options_.enable_persistence) {
            CachePersistenceManager::Config cfg;
            cfg.enabled = true;
            cfg.aof_path = options_.aof_path;
            cfg.snapshot_path = options_.snapshot_path;
            cfg.aof_flush_interval = options_.aof_flush_interval;
            cfg.aof_flush_batch = options_.aof_flush_batch;
            cfg.snapshot_interval = options_.snapshot_interval;
            persistence_ = std::make_unique<CachePersistenceManager>(cache_, std::move(cfg));
        }
    }

    [[nodiscard]] Cache& cache() noexcept {
        return cache_;
    }

    [[nodiscard]] const Cache& cache() const noexcept {
        return cache_;
    }

    bool set(
        const std::string& key,
        const std::string& value,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt) {
        const bool ok = cache_.set(key, value, ttl);
        if (persistence_ != nullptr) {
            persistence_->record_set(key, value, ttl);
        }
        return ok;
    }

    [[nodiscard]] std::optional<std::string> get(const std::string& key) {
        return cache_.get(key);
    }

    bool del(const std::string& key) {
        const bool deleted = cache_.del(key);
        if (persistence_ != nullptr) {
            persistence_->record_del(key);
        }
        return deleted;
    }

    [[nodiscard]] CacheStats stats() const {
        return cache_.stats();
    }

    [[nodiscard]] CommandResponse execute_line(const std::string& raw_line) {
        const auto parsed = parse_tcp_command(raw_line);
        if (parsed.type == TcpCommandType::Invalid) {
            return {false, false, "ERR " + parsed.error};
        }
        return execute_parsed(parsed, true);
    }

private:
    [[nodiscard]] CommandResponse execute_parsed(
        const ParsedTcpCommand& parsed,
        bool persist_write) {
        switch (parsed.type) {
            case TcpCommandType::Ping:
                return {true, false, "PONG"};
            case TcpCommandType::Help:
                return {
                    true,
                    false,
                    "Commands: PING | SET <key> <value> [ttl_ms] | GET <key> | DEL <key> | STATS | HELP | QUIT"
                };
            case TcpCommandType::Quit:
                return {true, true, "BYE"};
            case TcpCommandType::Set: {
                std::optional<std::chrono::milliseconds> ttl = std::nullopt;
                if (parsed.ttl_ms.has_value()) {
                    ttl = std::chrono::milliseconds(parsed.ttl_ms.value());
                }

                const bool ok = cache_.set(parsed.key, parsed.value, ttl);
                if (persist_write && persistence_ != nullptr) {
                    persistence_->record_set(parsed.key, parsed.value, ttl);
                }
                return {ok, false, ok ? "OK" : "ERR set failed"};
            }
            case TcpCommandType::Get: {
                const auto value = cache_.get(parsed.key);
                if (!value.has_value()) {
                    return {true, false, "(nil)"};
                }
                return {true, false, *value};
            }
            case TcpCommandType::Del: {
                const bool deleted = cache_.del(parsed.key);
                if (persist_write && persistence_ != nullptr) {
                    persistence_->record_del(parsed.key);
                }
                return {true, false, deleted ? "1" : "0"};
            }
            case TcpCommandType::Stats: {
                const auto s = cache_.stats();
                std::ostringstream oss;
                oss << "hits=" << s.hits << ' '
                    << "misses=" << s.misses << ' '
                    << "hit_ratio=" << s.hit_ratio() << ' '
                    << "evictions=" << s.evictions << ' '
                    << "expirations=" << s.expirations << ' '
                    << "active_keys=" << s.active_keys << ' '
                    << "memory_bytes=" << s.memory_bytes;
                return {true, false, oss.str()};
            }
            case TcpCommandType::Invalid:
            default:
                return {false, false, "ERR unknown command"};
        }
    }

    Cache cache_;
    RedisProcessorOptions options_;
    std::unique_ptr<CachePersistenceManager> persistence_;
};

}  // namespace mini_redis
