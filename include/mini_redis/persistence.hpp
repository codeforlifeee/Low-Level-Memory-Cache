#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "mini_redis/sharded_cache.hpp"

namespace mini_redis {

class CachePersistenceManager {
public:
    using Cache = ShardedCache<std::string, std::string, 64>;

    struct Config {
        bool enabled{true};
        std::string aof_path{"data/appendonly.aof"};
        std::string snapshot_path{"data/dump.rdb"};
        std::chrono::milliseconds aof_flush_interval{1000};
        std::size_t aof_flush_batch{256};
        std::chrono::seconds snapshot_interval{30};
    };

    CachePersistenceManager(Cache& cache, Config config);
    ~CachePersistenceManager();

    CachePersistenceManager(const CachePersistenceManager&) = delete;
    CachePersistenceManager& operator=(const CachePersistenceManager&) = delete;

    void record_set(
        const std::string& key,
        const std::string& value,
        std::optional<std::chrono::milliseconds> ttl);

    void record_del(const std::string& key);

    // Placeholder hook for future compaction support.
    void request_aof_rewrite();

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mini_redis
