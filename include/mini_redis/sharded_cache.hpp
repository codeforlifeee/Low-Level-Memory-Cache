#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <list>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mini_redis {

template <typename T, typename = void>
struct HasSizeMethod : std::false_type {};

template <typename T>
struct HasSizeMethod<T, std::void_t<decltype(std::declval<const T&>().size())>> : std::true_type {};

template <typename T>
struct DefaultObjectSizer {
    std::size_t operator()(const T& value) const noexcept {
        if constexpr (HasSizeMethod<T>::value) {
            return sizeof(T) + static_cast<std::size_t>(value.size());
        } else {
            return sizeof(T);
        }
    }
};

struct CacheStats {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t evictions{0};
    std::uint64_t expirations{0};
    std::size_t memory_bytes{0};
    std::size_t active_keys{0};

    [[nodiscard]] double hit_ratio() const noexcept {
        const auto total = hits + misses;
        if (total == 0) {
            return 0.0;
        }
        return static_cast<double>(hits) / static_cast<double>(total);
    }
};

template <
    typename Key,
    typename Value,
    std::size_t NumShards = 16,
    typename Hash = std::hash<Key>,
    typename KeySizer = DefaultObjectSizer<Key>,
    typename ValueSizer = DefaultObjectSizer<Value>>
class ShardedCache {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    static_assert(NumShards > 0, "NumShards must be greater than zero");

    explicit ShardedCache(std::size_t max_keys, bool enable_background_reaper = true)
        : max_keys_(max_keys), shards_(NumShards) {
        const auto base = max_keys_ / NumShards;
        const auto remainder = max_keys_ % NumShards;

        for (std::size_t i = 0; i < NumShards; ++i) {
            shards_[i].capacity = base + (i < remainder ? 1 : 0);
        }

        if (enable_background_reaper) {
            reaper_thread_ = std::thread([this]() { reaper_loop(); });
        }
    }

    ~ShardedCache() {
        stop_reaper_.store(true, std::memory_order_relaxed);
        if (reaper_thread_.joinable()) {
            reaper_thread_.join();
        }
    }

    ShardedCache(const ShardedCache&) = delete;
    ShardedCache& operator=(const ShardedCache&) = delete;

    bool set(
        const Key& key,
        const Value& value,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt) {
        return set_impl(key, value, ttl);
    }

    bool set(
        const Key& key,
        Value&& value,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt) {
        return set_impl(key, std::move(value), ttl);
    }

    [[nodiscard]] std::optional<Value> get(const Key& key) {
        auto& shard = shard_for_key(key);
        const auto now = Clock::now();

        {
            std::shared_lock read_lock(shard.mutex);
            const auto it = shard.map.find(key);
            if (it == shard.map.end()) {
                misses_.fetch_add(1, std::memory_order_relaxed);
                return std::nullopt;
            }

            if (!is_expired(it->second, now)) {
                // Escalate to writer lock to safely mutate the LRU order.
            }
        }

        std::unique_lock write_lock(shard.mutex);
        const auto it = shard.map.find(key);
        if (it == shard.map.end()) {
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        if (is_expired(it->second, now)) {
            erase_entry_locked(shard, it);
            expirations_.fetch_add(1, std::memory_order_relaxed);
            misses_.fetch_add(1, std::memory_order_relaxed);
            return std::nullopt;
        }

        touch_lru_locked(shard, it);
        hits_.fetch_add(1, std::memory_order_relaxed);
        return it->second.value;
    }

    bool del(const Key& key) {
        auto& shard = shard_for_key(key);
        std::unique_lock lock(shard.mutex);
        const auto it = shard.map.find(key);
        if (it == shard.map.end()) {
            return false;
        }

        erase_entry_locked(shard, it);
        return true;
    }

    template <typename Loader>
    Value get_or_compute(
        const Key& key,
        Loader&& loader,
        std::optional<std::chrono::milliseconds> ttl = std::nullopt) {
        if (auto cached = get(key); cached.has_value()) {
            return *cached;
        }

        auto& shard = shard_for_key(key);
        std::shared_ptr<InFlight> inflight;
        bool is_owner = false;

        {
            std::unique_lock lock(shard.mutex);
            const auto now = Clock::now();

            auto it = shard.map.find(key);
            if (it != shard.map.end()) {
                if (is_expired(it->second, now)) {
                    erase_entry_locked(shard, it);
                    expirations_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    touch_lru_locked(shard, it);
                    hits_.fetch_add(1, std::memory_order_relaxed);
                    return it->second.value;
                }
            }

            const auto waiting_it = shard.inflight.find(key);
            if (waiting_it != shard.inflight.end()) {
                inflight = waiting_it->second;
            } else {
                inflight = std::make_shared<InFlight>();
                shard.inflight.emplace(key, inflight);
                is_owner = true;
            }
        }

        if (is_owner) {
            try {
                Value computed = loader();
                set(key, computed, ttl);
                inflight->promise->set_value(computed);

                {
                    std::unique_lock lock(shard.mutex);
                    shard.inflight.erase(key);
                }
                return computed;
            } catch (...) {
                inflight->promise->set_exception(std::current_exception());
                {
                    std::unique_lock lock(shard.mutex);
                    shard.inflight.erase(key);
                }
                throw;
            }
        }

        Value waited = inflight->future.get();
        return waited;
    }

    void cleanup_expired() {
        const auto now = Clock::now();
        for (auto& shard : shards_) {
            std::unique_lock lock(shard.mutex);
            for (auto it = shard.map.begin(); it != shard.map.end();) {
                if (is_expired(it->second, now)) {
                    it = erase_entry_locked(shard, it);
                    expirations_.fetch_add(1, std::memory_order_relaxed);
                } else {
                    ++it;
                }
            }
        }
    }

    [[nodiscard]] std::size_t active_key_count() const {
        const auto now = Clock::now();
        std::size_t total = 0;
        for (const auto& shard : shards_) {
            std::shared_lock lock(shard.mutex);
            for (const auto& item : shard.map) {
                if (!is_expired(item.second, now)) {
                    ++total;
                }
            }
        }
        return total;
    }

    [[nodiscard]] std::size_t memory_usage_bytes() const {
        return memory_bytes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] CacheStats stats() const {
        CacheStats snapshot;
        snapshot.hits = hits_.load(std::memory_order_relaxed);
        snapshot.misses = misses_.load(std::memory_order_relaxed);
        snapshot.evictions = evictions_.load(std::memory_order_relaxed);
        snapshot.expirations = expirations_.load(std::memory_order_relaxed);
        snapshot.memory_bytes = memory_usage_bytes();
        snapshot.active_keys = active_key_count();
        return snapshot;
    }

    [[nodiscard]] std::size_t max_keys() const noexcept {
        return max_keys_;
    }

private:
    struct Entry {
        Value value;
        bool has_expiry{false};
        TimePoint expiry{};
        typename std::list<Key>::iterator lru_pos;
        std::size_t bytes{0};
    };

    struct InFlight {
        InFlight()
            : promise(std::make_shared<std::promise<Value>>()),
              future(promise->get_future().share()) {}

        std::shared_ptr<std::promise<Value>> promise;
        std::shared_future<Value> future;
    };

    struct Shard {
        mutable std::shared_mutex mutex;
        std::unordered_map<Key, Entry, Hash> map;
        std::list<Key> lru;
        std::unordered_map<Key, std::shared_ptr<InFlight>, Hash> inflight;
        std::size_t capacity{0};
    };

    template <typename V>
    bool set_impl(
        const Key& key,
        V&& value,
        std::optional<std::chrono::milliseconds> ttl) {
        auto& shard = shard_for_key(key);
        std::unique_lock lock(shard.mutex);

        if (shard.capacity == 0) {
            return false;
        }

        const auto now = Clock::now();
        auto it = shard.map.find(key);

        if (it != shard.map.end()) {
            memory_bytes_.fetch_sub(it->second.bytes, std::memory_order_relaxed);
            it->second.value = std::forward<V>(value);
            it->second.has_expiry = ttl.has_value();
            it->second.expiry = ttl.has_value() ? now + ttl.value() : TimePoint{};
            it->second.bytes = estimate_entry_bytes(key, it->second.value);
            touch_lru_locked(shard, it);
            memory_bytes_.fetch_add(it->second.bytes, std::memory_order_relaxed);
        } else {
            shard.lru.push_front(key);
            Entry entry;
            entry.value = std::forward<V>(value);
            entry.has_expiry = ttl.has_value();
            entry.expiry = ttl.has_value() ? now + ttl.value() : TimePoint{};
            entry.lru_pos = shard.lru.begin();
            entry.bytes = estimate_entry_bytes(key, entry.value);
            auto [inserted_it, inserted] = shard.map.emplace(key, std::move(entry));
            (void)inserted_it;
            (void)inserted;
            memory_bytes_.fetch_add(entry.bytes, std::memory_order_relaxed);
        }

        evict_if_needed_locked(shard);
        return true;
    }

    [[nodiscard]] bool is_expired(const Entry& entry, TimePoint now) const {
        return entry.has_expiry && now >= entry.expiry;
    }

    void touch_lru_locked(Shard& shard, typename std::unordered_map<Key, Entry, Hash>::iterator it) {
        shard.lru.splice(shard.lru.begin(), shard.lru, it->second.lru_pos);
        it->second.lru_pos = shard.lru.begin();
    }

    void evict_if_needed_locked(Shard& shard) {
        while (shard.map.size() > shard.capacity) {
            const Key& oldest_key = shard.lru.back();
            const auto it = shard.map.find(oldest_key);
            if (it != shard.map.end()) {
                erase_entry_locked(shard, it);
                evictions_.fetch_add(1, std::memory_order_relaxed);
            } else {
                shard.lru.pop_back();
            }
        }
    }

    typename std::unordered_map<Key, Entry, Hash>::iterator erase_entry_locked(
        Shard& shard,
        typename std::unordered_map<Key, Entry, Hash>::iterator it) {
        memory_bytes_.fetch_sub(it->second.bytes, std::memory_order_relaxed);
        shard.lru.erase(it->second.lru_pos);
        return shard.map.erase(it);
    }

    [[nodiscard]] std::size_t estimate_entry_bytes(const Key& key, const Value& value) const {
        return sizeof(Entry) + key_sizer_(key) + value_sizer_(value);
    }

    [[nodiscard]] Shard& shard_for_key(const Key& key) {
        const auto idx = hasher_(key) % NumShards;
        return shards_[idx];
    }

    [[nodiscard]] const Shard& shard_for_key(const Key& key) const {
        const auto idx = hasher_(key) % NumShards;
        return shards_[idx];
    }

    void reaper_loop() {
        using namespace std::chrono_literals;
        while (!stop_reaper_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(200ms);
            cleanup_expired();
        }
    }

    std::size_t max_keys_{0};
    std::vector<Shard> shards_;
    Hash hasher_{};
    KeySizer key_sizer_{};
    ValueSizer value_sizer_{};

    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> evictions_{0};
    std::atomic<std::uint64_t> expirations_{0};
    std::atomic<std::size_t> memory_bytes_{0};

    std::atomic<bool> stop_reaper_{false};
    std::thread reaper_thread_;
};

}  // namespace mini_redis
