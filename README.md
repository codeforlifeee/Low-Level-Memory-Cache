# Mini Redis: Thread-Safe In-Memory Cache (C++17)

A resume-grade, high-concurrency cache inspired by Redis fundamentals.

## Highlights

- **Fine-grained concurrency:** sharded architecture with per-shard `std::shared_mutex`.
- **Low-latency core ops:** expected O(1) `SET`, `GET`, `DEL` using hash map + doubly linked list per shard.
- **LRU eviction:** automatic least-recently-used eviction when shard capacity is reached.
- **TTL expiration:** user-defined key expiration with periodic background cleanup.
- **Thundering herd protection:** request collapsing via `std::shared_future` in `get_or_compute`.
- **Runtime monitoring:** hit/miss, hit ratio, memory usage estimate, active key count.
- **Interface layer:** Redis-like CLI and lightweight HTTP API for command simulation.
- **CI quality gates:** GitHub Actions matrix, Address/UB sanitizers, and ThreadSanitizer jobs.

## Project Structure

- `include/mini_redis/sharded_cache.hpp`: core generic cache implementation.
- `src/main.cpp`: demo usage with stats output.
- `src/cache_cli.cpp`: interactive Redis-like CLI simulator.
- `src/cache_http.cpp`: lightweight HTTP API wrapper around the cache.
- `tests/cache_tests.cpp`: Google Test unit + concurrency/stress tests.
- `benchmarks/cache_benchmark_tuned.cpp`: throughput/P99 benchmark with shard-count and workload-profile sweep.
- `docs/benchmark_report.md`: template + interpretation notes for benchmark output.
- `scripts/run_benchmark.ps1`: helper script to configure/build/run benchmark quickly.
- `.github/workflows/ci.yml`: CI pipeline for tests + sanitizers.
- `Dockerfile`, `docker-compose.yml`, `deploy/`: EC2 deployment package with rollback workflow.

## Build

### Windows (MSVC or Ninja)

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

### Run Demo

```powershell
./build/Release/cache_demo.exe
```

### Run CLI Simulator

```powershell
./build/Release/cache_cli.exe
```

Example commands:

```text
PING
SET user:1 Alice 3000
GET user:1
DEL user:1
STATS
QUIT
```

### Run HTTP Simulator

```powershell
./build/Release/cache_http.exe 8080 50000
```

Example calls:

```powershell
curl "http://localhost:8080/health"
curl -X POST "http://localhost:8080/set?key=user:1&value=Alice&ttl_ms=3000"
curl "http://localhost:8080/get?key=user:1"
curl -X DELETE "http://localhost:8080/del?key=user:1"
curl "http://localhost:8080/stats"
curl -X POST "http://localhost:8080/cmd" -d "SET k v 2000"
```

### Run Tests

```powershell
ctest --test-dir build -C Release --output-on-failure
```

### Run Benchmark

```powershell
./build/Release/cache_benchmark.exe
```

Or via helper script:

```powershell
./scripts/run_benchmark.ps1
```

## API (Core User Actions)

### `SET(key, value, ttl)`

```cpp
cache.set("user:1", "Alice", std::chrono::seconds(10));
cache.set("user:2", "Bob"); // no TTL
```

### `GET(key)`

```cpp
auto value = cache.get("user:1");
```

Returns `std::optional<Value>` and refreshes LRU on hit.

### `DEL(key)`

```cpp
bool removed = cache.del("user:1");
```

### `STATS()`

```cpp
auto stats = cache.stats();
double ratio = stats.hit_ratio();
```

## Architecture Mapping

- **API Layer:** `set`, `get`, `del`, `get_or_compute`, `stats`.
- **Concurrency Layer:** sharded map/list with per-shard reader-writer lock.
- **Logic Layer:** TTL checks, LRU movement, eviction, request collapsing.
- **Storage Layer:** `std::unordered_map` + `std::list` per shard.

## Resume-Ready Talking Points

- Implemented a C++17 sharded in-memory cache with request collapsing to eliminate thundering-herd effects.
- Achieved concurrent 100,000+ operation stress tests across 12 threads with LRU+TTL correctness.
- Added benchmark sweep across shard counts (8/16/32/64) and workload profiles (balanced/read-heavy/write-heavy), reporting throughput and P99 latency versus single-lock baseline.
- Built quality gates with Google Test and clear metrics for hit ratio, memory usage, and active key count.

## CI Pipeline

GitHub Actions workflow in `.github/workflows/ci.yml` runs:

- Ubuntu Debug/Release and Windows Release build+test matrix.
- ASan+UBSan job on Ubuntu with Clang.
- TSan job on Ubuntu with Clang.

## EC2 Deployment Package

The repo includes a production-style deployment path for AWS EC2:

- Containerized HTTP service (`Dockerfile`)
- Nginx reverse proxy (`deploy/nginx/default.conf`)
- Compose stack (`docker-compose.yml`)
- Release-based deploy+rollback script (`deploy/ec2/deploy.sh`)
- Bootstrap helper (`deploy/ec2/bootstrap.sh`)
- systemd unit (`deploy/systemd/mini-redis.service`)
- GitHub Actions deploy workflow (`.github/workflows/deploy-ec2.yml`)

Quick start guide: `deploy/README.md`

## Validation Guidance

### ThreadSanitizer (Linux/Clang)

```bash
cmake -S . -B build-tsan -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS="-fsanitize=thread -O1 -g"
cmake --build build-tsan
ctest --test-dir build-tsan --output-on-failure
```

### Valgrind (Linux)

```bash
valgrind --leak-check=full --show-leak-kinds=all ./build/cache_tests
```

## Notes

- Memory usage is an approximate tracked estimate suitable for operational monitoring.
- Global capacity is enforced by distributing key budget across shards; aggregate active keys never exceed configured max.
