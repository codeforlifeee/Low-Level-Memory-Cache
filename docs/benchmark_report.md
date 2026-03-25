# Benchmark Report

## Objective

Quantify throughput and P99 latency for:

1. `single_lock` (global mutex baseline)
2. `sharded_8`, `sharded_16`, `sharded_32`, `sharded_64`

across workload profiles: `balanced`, `read_heavy`, `write_heavy`.

## Workload

- Threads: 12
- Operations per thread: 60,000
- Total operations: 720,000 per cache variant
- Key-space: 25,000 unique keys
- Capacity: 60,000 keys
- Profiles:
	- balanced: 25% `SET`, 35% `GET`, 15% `DEL`, 25% `GET_OR_COMPUTE`
	- read_heavy: 15% `SET`, 60% `GET`, 10% `DEL`, 15% `GET_OR_COMPUTE`
	- write_heavy: 45% `SET`, 25% `GET`, 15% `DEL`, 15% `GET_OR_COMPUTE`

## How To Run

```powershell
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
./build/Release/cache_benchmark.exe
```

## Result Template

Fill this after running on your machine:

| Profile | Variant | Throughput (ops/sec) | P99 Latency (us) | Active Keys | Memory (bytes) |
|---|---:|---:|---:|---:|---:|
| balanced | single_lock | TBD | TBD | TBD | TBD |
| balanced | sharded_8 | TBD | TBD | TBD | TBD |
| balanced | sharded_16 | TBD | TBD | TBD | TBD |
| balanced | sharded_32 | TBD | TBD | TBD | TBD |
| balanced | sharded_64 | TBD | TBD | TBD | TBD |

## Interpretation Checklist

- Throughput improvement (%) = `(sharded - baseline) / baseline * 100`
- Lower P99 latency indicates better tail behavior under contention.
- Active key count should stay within configured capacity.
- No crashes under repeated runs.
- Select the best shard count per profile by maximum throughput with acceptable P99 tail.

## Resume Bullet Example

- Improved multi-threaded cache throughput by **X%** and reduced tail latency (P99) by **Y%** versus a single-lock baseline by tuning shard count to **N** and applying request-collapsing.
