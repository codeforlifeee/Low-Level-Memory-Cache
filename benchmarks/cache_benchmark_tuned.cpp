#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <iomanip>
#include <iostream>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mini_redis/sharded_cache.hpp"

namespace {

using Clock = std::chrono::steady_clock;

struct WorkloadProfile {
    std::string name;
    int set_pct;
    int get_pct;
    int del_pct;
    int get_or_compute_pct;
};

struct BenchmarkConfig {
    int threads{12};
    int ops_per_thread{60000};
    std::size_t capacity{60000};
    std::size_t key_space{25000};
    int sampling_rate{32};
};

struct BenchmarkResult {
    std::string variant;
    std::string profile;
    double throughput_ops_per_sec{0.0};
    double p99_us{0.0};
    std::size_t active_keys{0};
    std::size_t memory_bytes{0};
};

class SingleLockCache {
public:
    explicit SingleLockCache(std::size_t max_keys) : max_keys_(max_keys) {}

    bool set(const std::string& key, const std::string& value, std::optional<std::chrono::milliseconds> ttl = std::nullopt) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mu_);

        auto it = map_.find(key);
        if (it != map_.end()) {
            lru_.erase(it->second.lru_pos);
            it->second.value = value;
            it->second.has_expiry = ttl.has_value();
            it->second.expiry = ttl.has_value() ? now + ttl.value() : Clock::time_point{};
            lru_.push_front(key);
            it->second.lru_pos = lru_.begin();
        } else {
            lru_.push_front(key);
            map_[key] = Entry{value, ttl.has_value(), ttl.has_value() ? now + ttl.value() : Clock::time_point{}, lru_.begin()};
        }

        while (map_.size() > max_keys_) {
            const std::string old = lru_.back();
            lru_.pop_back();
            map_.erase(old);
        }
        return true;
    }

    std::optional<std::string> get(const std::string& key) {
        const auto now = Clock::now();
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }
        if (it->second.has_expiry && now >= it->second.expiry) {
            lru_.erase(it->second.lru_pos);
            map_.erase(it);
            return std::nullopt;
        }

        lru_.splice(lru_.begin(), lru_, it->second.lru_pos);
        it->second.lru_pos = lru_.begin();
        return it->second.value;
    }

    bool del(const std::string& key) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = map_.find(key);
        if (it == map_.end()) {
            return false;
        }
        lru_.erase(it->second.lru_pos);
        map_.erase(it);
        return true;
    }

    template <typename Loader>
    std::string get_or_compute(const std::string& key, Loader&& loader, std::optional<std::chrono::milliseconds> ttl = std::nullopt) {
        {
            auto value = get(key);
            if (value.has_value()) {
                return *value;
            }
        }

        std::shared_future<std::string> waiting_future;
        bool owner = false;
        std::shared_ptr<std::promise<std::string>> owner_promise;

        {
            std::unique_lock<std::mutex> lock(mu_);
            auto waiting = in_flight_.find(key);
            if (waiting != in_flight_.end()) {
                waiting_future = waiting->second;
            } else {
                owner_promise = std::make_shared<std::promise<std::string>>();
                waiting_future = owner_promise->get_future().share();
                in_flight_[key] = waiting_future;
                owner = true;
            }
        }

        if (owner) {
            try {
                std::string computed = loader();
                set(key, computed, ttl);
                owner_promise->set_value(computed);
            } catch (...) {
                owner_promise->set_exception(std::current_exception());
            }

            std::lock_guard<std::mutex> post_lock(mu_);
            in_flight_.erase(key);
        }

        return waiting_future.get();
    }

    std::size_t active_key_count() {
        std::lock_guard<std::mutex> lock(mu_);
        return map_.size();
    }

    std::size_t memory_usage_bytes() {
        std::lock_guard<std::mutex> lock(mu_);
        std::size_t bytes = 0;
        for (const auto& item : map_) {
            bytes += sizeof(item.second) + item.first.size() + item.second.value.size();
        }
        return bytes;
    }

private:
    struct Entry {
        std::string value;
        bool has_expiry{false};
        Clock::time_point expiry{};
        std::list<std::string>::iterator lru_pos;
    };

    std::size_t max_keys_;
    std::mutex mu_;
    std::unordered_map<std::string, Entry> map_;
    std::list<std::string> lru_;
    std::unordered_map<std::string, std::shared_future<std::string>> in_flight_;
};

template <typename Cache>
BenchmarkResult run_profile(
    const std::string& variant,
    const WorkloadProfile& profile,
    const BenchmarkConfig& cfg,
    Cache& cache) {
    std::atomic<bool> start{false};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(cfg.threads));

    std::vector<std::vector<double>> local_latencies(static_cast<std::size_t>(cfg.threads));

    const auto benchmark_start = Clock::now();

    for (int t = 0; t < cfg.threads; ++t) {
        workers.emplace_back([&, t] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }

            uint64_t state = 0x9e3779b97f4a7c15ULL ^ static_cast<uint64_t>(t + 11);
            auto rng = [&]() {
                state ^= state >> 12;
                state ^= state << 25;
                state ^= state >> 27;
                return state * 2685821657736338717ULL;
            };

            auto& latencies = local_latencies[static_cast<std::size_t>(t)];
            latencies.reserve(static_cast<std::size_t>(cfg.ops_per_thread / cfg.sampling_rate + 8));

            for (int i = 0; i < cfg.ops_per_thread; ++i) {
                const auto key_id = static_cast<std::size_t>(rng() % cfg.key_space);
                const std::string key = "k-" + std::to_string(key_id);
                const int selector = static_cast<int>(rng() % 100);

                const auto t0 = Clock::now();
                if (selector < profile.set_pct) {
                    cache.set(key, "v-" + std::to_string(rng() % 100000), std::chrono::milliseconds(2500));
                } else if (selector < profile.set_pct + profile.get_pct) {
                    (void)cache.get(key);
                } else if (selector < profile.set_pct + profile.get_pct + profile.del_pct) {
                    (void)cache.del(key);
                } else {
                    (void)cache.get_or_compute(
                        key,
                        [&]() {
                            return std::string{"computed-"} + std::to_string(rng() % 50000);
                        },
                        std::chrono::milliseconds(1800));
                }
                const auto t1 = Clock::now();

                if ((i % cfg.sampling_rate) == 0) {
                    const double us = std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(t1 - t0).count();
                    latencies.push_back(us);
                }
            }
        });
    }

    start.store(true, std::memory_order_release);

    for (auto& thread : workers) {
        thread.join();
    }

    const auto benchmark_end = Clock::now();

    std::vector<double> merged;
    for (const auto& local : local_latencies) {
        merged.insert(merged.end(), local.begin(), local.end());
    }

    double p99 = 0.0;
    if (!merged.empty()) {
        const auto p99_index = static_cast<std::size_t>(0.99 * static_cast<double>(merged.size() - 1));
        std::nth_element(merged.begin(), merged.begin() + static_cast<std::ptrdiff_t>(p99_index), merged.end());
        p99 = merged[p99_index];
    }

    const double total_ops = static_cast<double>(cfg.threads) * static_cast<double>(cfg.ops_per_thread);
    const double elapsed_sec = std::chrono::duration_cast<std::chrono::duration<double>>(benchmark_end - benchmark_start).count();

    BenchmarkResult result;
    result.variant = variant;
    result.profile = profile.name;
    result.throughput_ops_per_sec = total_ops / std::max(elapsed_sec, 1e-9);
    result.p99_us = p99;
    result.active_keys = cache.active_key_count();
    result.memory_bytes = cache.memory_usage_bytes();

    return result;
}

template <std::size_t Shards>
BenchmarkResult run_sharded_candidate(const WorkloadProfile& profile, const BenchmarkConfig& cfg) {
    mini_redis::ShardedCache<std::string, std::string, Shards> cache(cfg.capacity, true);
    return run_profile("sharded_" + std::to_string(Shards), profile, cfg, cache);
}

void print_result(const BenchmarkResult& r) {
    std::cout << "\n[" << r.profile << "] " << r.variant << "\n";
    std::cout << "  throughput ops/sec : " << std::fixed << std::setprecision(2) << r.throughput_ops_per_sec << "\n";
    std::cout << "  p99 latency (us)   : " << std::fixed << std::setprecision(2) << r.p99_us << "\n";
    std::cout << "  active keys        : " << r.active_keys << "\n";
    std::cout << "  memory bytes       : " << r.memory_bytes << "\n";
}

void print_resume_line(const WorkloadProfile& profile, const BenchmarkResult& base, const BenchmarkResult& best) {
    const double throughput_gain_pct =
        ((best.throughput_ops_per_sec - base.throughput_ops_per_sec) / std::max(base.throughput_ops_per_sec, 1e-9)) * 100.0;
    const double p99_reduction_pct =
        ((base.p99_us - best.p99_us) / std::max(base.p99_us, 1e-9)) * 100.0;

    std::cout << "\nResume bullet for profile '" << profile.name << "':\n";
    std::cout << "  Improved cache throughput by " << std::fixed << std::setprecision(1) << throughput_gain_pct
              << "% and reduced P99 latency by " << p99_reduction_pct
              << "% versus single-lock baseline by tuning shard count to " << best.variant << ".\n";
}

}  // namespace

int main() {
    const BenchmarkConfig cfg{};

    const std::vector<WorkloadProfile> profiles = {
        {"balanced", 25, 35, 15, 25},
        {"read_heavy", 15, 60, 10, 15},
        {"write_heavy", 45, 25, 15, 15}
    };

    std::cout << "Benchmark sweep: threads=" << cfg.threads
              << " ops_per_thread=" << cfg.ops_per_thread
              << " capacity=" << cfg.capacity
              << " key_space=" << cfg.key_space << "\n";

    for (const auto& profile : profiles) {
        SingleLockCache baseline(cfg.capacity);
        const auto base = run_profile("single_lock", profile, cfg, baseline);
        print_result(base);

        std::vector<BenchmarkResult> sharded_results;
        sharded_results.push_back(run_sharded_candidate<8>(profile, cfg));
        sharded_results.push_back(run_sharded_candidate<16>(profile, cfg));
        sharded_results.push_back(run_sharded_candidate<32>(profile, cfg));
        sharded_results.push_back(run_sharded_candidate<64>(profile, cfg));

        for (const auto& r : sharded_results) {
            print_result(r);
        }

        const auto best_it = std::max_element(
            sharded_results.begin(),
            sharded_results.end(),
            [](const BenchmarkResult& a, const BenchmarkResult& b) {
                return a.throughput_ops_per_sec < b.throughput_ops_per_sec;
            });

        if (best_it != sharded_results.end()) {
            print_resume_line(profile, base, *best_it);
        }
    }

    return 0;
}
