#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "mini_redis/sharded_cache.hpp"

using Cache = mini_redis::ShardedCache<std::string, std::string, 16>;

TEST(CacheBasicTest, SetGetDeleteWorks) {
    Cache cache(32, false);

    EXPECT_TRUE(cache.set("k1", "v1"));
    auto v = cache.get("k1");
    ASSERT_TRUE(v.has_value());
    EXPECT_EQ(*v, "v1");

    EXPECT_TRUE(cache.del("k1"));
    EXPECT_FALSE(cache.get("k1").has_value());
}

TEST(CacheBasicTest, TTLMakesKeyInaccessible) {
    using namespace std::chrono_literals;
    Cache cache(32, true);

    EXPECT_TRUE(cache.set("session", "token", 300ms));
    std::this_thread::sleep_for(1300ms);

    EXPECT_FALSE(cache.get("session").has_value());
}

TEST(CacheBasicTest, CapacityNeverExceedsConfiguredMaximum) {
    Cache cache(10, false);

    for (int i = 0; i < 100; ++i) {
        cache.set("key-" + std::to_string(i), "value-" + std::to_string(i));
    }

    EXPECT_LE(cache.active_key_count(), cache.max_keys());
}

TEST(CacheConcurrencyTest, RequestCollapsingPreventsThunderingHerd) {
    using namespace std::chrono_literals;

    Cache cache(128, false);
    cache.set("shared", "old", 50ms);
    std::this_thread::sleep_for(70ms);

    constexpr int kThreads = 64;
    std::atomic<int> loader_calls{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> workers;
    std::vector<std::string> results(kThreads);
    workers.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        workers.emplace_back([&, i] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            results[i] = cache.get_or_compute(
                "shared",
                [&]() {
                    loader_calls.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(80ms);
                    return std::string{"new-value"};
                },
                2s);
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& t : workers) {
        t.join();
    }

    EXPECT_EQ(loader_calls.load(std::memory_order_relaxed), 1);
    for (const auto& r : results) {
        EXPECT_EQ(r, "new-value");
    }
}

TEST(CacheConcurrencyTest, Handles100KOpsAcross10PlusThreads) {
    using namespace std::chrono_literals;

    constexpr int kThreads = 12;
    constexpr int kOpsPerThread = 10000;  // 120,000 ops total.

    Cache cache(20000, true);
    std::atomic<bool> start{false};

    std::vector<std::thread> workers;
    workers.reserve(kThreads);

    for (int thread_id = 0; thread_id < kThreads; ++thread_id) {
        workers.emplace_back([&, thread_id] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            uint64_t state = 0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(thread_id);
            auto next_rand = [&]() {
                state ^= state >> 12;
                state ^= state << 25;
                state ^= state >> 27;
                return state * 2685821657736338717ULL;
            };

            for (int i = 0; i < kOpsPerThread; ++i) {
                const auto key_id = static_cast<int>(next_rand() % 5000);
                const std::string key = "k-" + std::to_string(key_id);
                const int op = static_cast<int>(next_rand() % 4);

                if (op == 0) {
                    cache.set(key, "v-" + std::to_string(next_rand() % 100000), 1500ms);
                } else if (op == 1) {
                    (void)cache.get(key);
                } else if (op == 2) {
                    (void)cache.del(key);
                } else {
                    (void)cache.get_or_compute(
                        key,
                        [&]() {
                            return std::string{"computed-"} + std::to_string(next_rand() % 99999);
                        },
                        1000ms);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_LE(cache.active_key_count(), cache.max_keys());
    const auto stats = cache.stats();
    EXPECT_GT(stats.hits + stats.misses, 0);
}
