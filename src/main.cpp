#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "mini_redis/sharded_cache.hpp"

int main() {
    using namespace std::chrono_literals;
    mini_redis::ShardedCache<std::string, std::string, 16> cache(1000);

    cache.set("user:1", "Alice", 5s);
    cache.set("user:2", "Bob");

    if (auto value = cache.get("user:1")) {
        std::cout << "GET user:1 -> " << *value << '\n';
    }

    const auto computed = cache.get_or_compute(
        "expensive:key",
        [] {
            std::this_thread::sleep_for(50ms);
            return std::string{"generated-value"};
        },
        10s);

    std::cout << "GET_OR_COMPUTE expensive:key -> " << computed << '\n';

    const auto stats = cache.stats();
    std::cout << "Hits: " << stats.hits << " Misses: " << stats.misses
              << " HitRatio: " << stats.hit_ratio() << " ActiveKeys: " << stats.active_keys
              << " Memory(bytes): " << stats.memory_bytes << '\n';

    return 0;
}
