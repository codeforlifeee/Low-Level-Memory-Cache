#include "mini_redis/persistence.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "mini_redis/tcp_command_parser.hpp"

namespace mini_redis {

class CachePersistenceManager::Impl {
public:
    Impl(Cache& cache, Config config)
        : cache_(cache), config_(std::move(config)) {
        if (!config_.enabled) {
            return;
        }

        ensure_parent_dir(config_.aof_path);
        ensure_parent_dir(config_.snapshot_path);

        recover_from_disk();

        if (config_.aof_flush_batch == 0) {
            config_.aof_flush_batch = 1;
        }
        if (config_.aof_flush_interval.count() <= 0) {
            config_.aof_flush_interval = std::chrono::milliseconds(1000);
        }
        if (config_.snapshot_interval.count() <= 0) {
            config_.snapshot_interval = std::chrono::seconds(30);
        }

        stop_.store(false, std::memory_order_relaxed);
        aof_worker_ = std::thread([this]() { aof_loop(); });
        snapshot_worker_ = std::thread([this]() { snapshot_loop(); });
    }

    ~Impl() {
        stop_.store(true, std::memory_order_relaxed);
        queue_cv_.notify_all();
        snapshot_cv_.notify_all();

        if (aof_worker_.joinable()) {
            aof_worker_.join();
        }
        if (snapshot_worker_.joinable()) {
            snapshot_worker_.join();
        }
    }

    void record_set(
        const std::string& key,
        const std::string& value,
        std::optional<std::chrono::milliseconds> ttl) {
        if (!config_.enabled) {
            return;
        }

        std::string line = "SET " + key + " " + value;
        if (ttl.has_value()) {
            line += " " + std::to_string(ttl->count());
        }
        enqueue_line(std::move(line));
    }

    void record_del(const std::string& key) {
        if (!config_.enabled) {
            return;
        }

        enqueue_line("DEL " + key);
    }

    void request_aof_rewrite() {
        rewrite_requested_.store(true, std::memory_order_relaxed);
    }

private:
    static void ensure_parent_dir(const std::string& file_path) {
        const std::filesystem::path path(file_path);
        const auto parent = path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }
    }

    static bool is_comment_or_empty(const std::string& line) {
        const auto trimmed = tcp_trim(line);
        return trimmed.empty() || trimmed[0] == '#';
    }

    void recover_from_disk() {
        replay_file(config_.snapshot_path);
        replay_file(config_.aof_path);
    }

    void replay_file(const std::string& file_path) {
        std::ifstream in(file_path, std::ios::in);
        if (!in.is_open()) {
            return;
        }

        std::string line;
        while (std::getline(in, line)) {
            if (is_comment_or_empty(line)) {
                continue;
            }

            const auto parsed = parse_tcp_command(line);
            if (parsed.type == TcpCommandType::Set) {
                std::optional<std::chrono::milliseconds> ttl = std::nullopt;
                if (parsed.ttl_ms.has_value()) {
                    ttl = std::chrono::milliseconds(parsed.ttl_ms.value());
                }
                (void)cache_.set(parsed.key, parsed.value, ttl);
            } else if (parsed.type == TcpCommandType::Del) {
                (void)cache_.del(parsed.key);
            }
        }
    }

    void enqueue_line(std::string line) {
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            queue_.push_back(std::move(line));
        }
        queue_cv_.notify_one();
    }

    void aof_loop() {
        std::ofstream out(config_.aof_path, std::ios::out | std::ios::app);
        if (!out.is_open()) {
            return;
        }

        std::vector<std::string> batch;
        batch.reserve(config_.aof_flush_batch * 2);

        while (!stop_.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                queue_cv_.wait_for(
                    lock,
                    config_.aof_flush_interval,
                    [this]() {
                        return stop_.load(std::memory_order_relaxed) ||
                            queue_.size() >= config_.aof_flush_batch ||
                            !queue_.empty();
                    });

                while (!queue_.empty()) {
                    batch.push_back(std::move(queue_.front()));
                    queue_.pop_front();
                }
            }

            if (!batch.empty()) {
                for (const auto& line : batch) {
                    out << line << '\n';
                }
                out.flush();
                batch.clear();
            }

            if (rewrite_requested_.exchange(false, std::memory_order_relaxed)) {
                out.close();
                std::ofstream truncate_file(config_.aof_path, std::ios::out | std::ios::trunc);
                truncate_file.close();
                out.open(config_.aof_path, std::ios::out | std::ios::app);
                if (!out.is_open()) {
                    return;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            while (!queue_.empty()) {
                batch.push_back(std::move(queue_.front()));
                queue_.pop_front();
            }
        }

        if (!batch.empty()) {
            for (const auto& line : batch) {
                out << line << '\n';
            }
            out.flush();
        }
    }

    void snapshot_loop() {
        while (true) {
            std::unique_lock<std::mutex> lock(snapshot_mutex_);
            snapshot_cv_.wait_for(
                lock,
                config_.snapshot_interval,
                [this]() { return stop_.load(std::memory_order_relaxed); });
            if (stop_.load(std::memory_order_relaxed)) {
                break;
            }
            lock.unlock();
            write_snapshot_file();
        }

        write_snapshot_file();
    }

    void write_snapshot_file() {
        const auto items = cache_.snapshot_items();

        const std::filesystem::path snapshot_path(config_.snapshot_path);
        const std::filesystem::path tmp_path = snapshot_path.string() + ".tmp";

        std::ofstream out(tmp_path.string(), std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            return;
        }

        const auto now_epoch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        out << "# LLMC_RDB_V1 " << now_epoch_ms << '\n';

        for (const auto& item : items) {
            out << "SET " << item.key << " " << item.value;
            if (item.ttl_remaining.has_value()) {
                out << " " << item.ttl_remaining->count();
            }
            out << '\n';
        }
        out.flush();
        out.close();

        std::error_code ec;
        std::filesystem::remove(snapshot_path, ec);
        std::filesystem::rename(tmp_path, snapshot_path, ec);
        if (ec) {
            std::filesystem::remove(tmp_path, ec);
        }
    }

    Cache& cache_;
    Config config_;

    std::atomic<bool> stop_{false};
    std::atomic<bool> rewrite_requested_{false};

    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<std::string> queue_;

    std::thread aof_worker_;
    std::thread snapshot_worker_;
    std::mutex snapshot_mutex_;
    std::condition_variable snapshot_cv_;
};

CachePersistenceManager::CachePersistenceManager(Cache& cache, Config config)
    : impl_(std::make_unique<Impl>(cache, std::move(config))) {}

CachePersistenceManager::~CachePersistenceManager() = default;

void CachePersistenceManager::record_set(
    const std::string& key,
    const std::string& value,
    std::optional<std::chrono::milliseconds> ttl) {
    impl_->record_set(key, value, ttl);
}

void CachePersistenceManager::record_del(const std::string& key) {
    impl_->record_del(key);
}

void CachePersistenceManager::request_aof_rewrite() {
    impl_->request_aof_rewrite();
}

}  // namespace mini_redis
