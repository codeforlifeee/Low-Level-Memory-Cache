import argparse
import random
import socket
import statistics
import threading
import time
from typing import List, Tuple


def percentile(sorted_values: List[float], p: float) -> float:
    if not sorted_values:
        return 0.0
    idx = int((len(sorted_values) - 1) * p)
    return sorted_values[idx]


def recv_line(sock: socket.socket) -> str:
    data = bytearray()
    while True:
        chunk = sock.recv(1)
        if not chunk:
            break
        if chunk == b"\n":
            break
        data.extend(chunk)
    return data.decode("utf-8", errors="replace").rstrip("\r")


def send_command(sock: socket.socket, command: str) -> str:
    sock.sendall((command + "\n").encode("utf-8"))
    return recv_line(sock)


def make_operation(op_index: int, mode: str, key_space: int, set_ratio: float, get_ratio: float) -> str:
    if mode == "ping":
        return "PING"

    key = f"key:{random.randint(1, key_space)}"

    if mode == "kv":
        # Fixed SET -> GET -> DEL sequence pattern by operation index.
        phase = op_index % 3
        if phase == 0:
            return f"SET {key} value:{op_index} 5000"
        if phase == 1:
            return f"GET {key}"
        return f"DEL {key}"

    # mode == mixed: ratio-based realistic workload
    r = random.random()
    if r < get_ratio:
        return f"GET {key}"
    if r < get_ratio + set_ratio:
        return f"SET {key} value:{op_index} 5000"
    return f"DEL {key}"


def worker(
    host: str,
    port: int,
    num_requests: int,
    mode: str,
    key_space: int,
    set_ratio: float,
    get_ratio: float,
    latencies_out: List[float],
    counters: dict,
    counters_lock: threading.Lock,
) -> None:
    local_latencies: List[float] = []
    local_success = 0
    local_errors = 0

    try:
        with socket.create_connection((host, port), timeout=5.0) as sock:
            sock.settimeout(5.0)

            # Read greeting from server: "OK mini-redis tcp server ready"
            _ = recv_line(sock)

            for i in range(num_requests):
                cmd = make_operation(i, mode, key_space, set_ratio, get_ratio)

                start_ns = time.perf_counter_ns()
                try:
                    response = send_command(sock, cmd)
                    end_ns = time.perf_counter_ns()

                    local_latencies.append((end_ns - start_ns) / 1000.0)

                    # Treat non-transport response as a completed operation.
                    # We still count protocol ERROR responses as success from load perspective.
                    if response:
                        local_success += 1
                    else:
                        local_errors += 1
                except Exception:
                    end_ns = time.perf_counter_ns()
                    local_latencies.append((end_ns - start_ns) / 1000.0)
                    local_errors += 1

            try:
                send_command(sock, "QUIT")
            except Exception:
                pass

    except Exception:
        # Connection-level failure: mark all as errors.
        local_errors += num_requests

    latencies_out.extend(local_latencies)
    with counters_lock:
        counters["success"] += local_success
        counters["errors"] += local_errors


def run_benchmark(total_requests: int, concurrency: int, host: str, port: int, mode: str, key_space: int, set_ratio: float, get_ratio: float) -> None:
    if concurrency <= 0:
        raise ValueError("concurrency must be > 0")
    if total_requests <= 0:
        raise ValueError("total_requests must be > 0")

    per_thread = total_requests // concurrency
    remainder = total_requests % concurrency

    threads: List[threading.Thread] = []
    all_latencies: List[List[float]] = [[] for _ in range(concurrency)]
    counters = {"success": 0, "errors": 0}
    counters_lock = threading.Lock()

    start = time.perf_counter()

    for t in range(concurrency):
        n = per_thread + (1 if t < remainder else 0)
        th = threading.Thread(
            target=worker,
            args=(
                host,
                port,
                n,
                mode,
                key_space,
                set_ratio,
                get_ratio,
                all_latencies[t],
                counters,
                counters_lock,
            ),
            daemon=True,
        )
        th.start()
        threads.append(th)

    for th in threads:
        th.join()

    end = time.perf_counter()

    flat_latencies = [x for sub in all_latencies for x in sub]
    flat_latencies.sort()

    elapsed = max(end - start, 1e-9)
    achieved_requests = counters["success"] + counters["errors"]
    throughput = achieved_requests / elapsed

    p50 = percentile(flat_latencies, 0.50)
    p95 = percentile(flat_latencies, 0.95)
    p99 = percentile(flat_latencies, 0.99)
    avg = statistics.mean(flat_latencies) if flat_latencies else 0.0

    print("---- BENCHMARK RESULTS ----")
    print(f"Host: {host}:{port}")
    print(f"Mode: {mode}")
    print(f"Total requests (target): {total_requests}")
    print(f"Total requests (executed): {achieved_requests}")
    print(f"Concurrency: {concurrency}")
    print(f"Elapsed (sec): {elapsed:.4f}")
    print(f"Throughput (ops/sec): {throughput:.2f}")
    print(f"Average latency (us): {avg:.2f}")
    print(f"P50 latency (us): {p50:.2f}")
    print(f"P95 latency (us): {p95:.2f}")
    print(f"P99 latency (us): {p99:.2f}")
    print(f"Successful responses: {counters['success']}")
    print(f"Transport/protocol errors: {counters['errors']}")


def main() -> None:
    parser = argparse.ArgumentParser(description="Mini Redis TCP benchmark")
    parser.add_argument("--host", default="127.0.0.1", help="TCP server host")
    parser.add_argument("--port", type=int, default=6379, help="TCP server port")
    parser.add_argument("--requests", type=int, default=10000, help="Total request count")
    parser.add_argument("--concurrency", type=int, default=10, help="Parallel client threads")
    parser.add_argument("--mode", choices=["ping", "kv", "mixed"], default="ping", help="Workload mode")
    parser.add_argument("--key-space", type=int, default=1000, help="Number of unique keys for kv/mixed")
    parser.add_argument("--set-ratio", type=float, default=0.30, help="SET ratio for mixed mode")
    parser.add_argument("--get-ratio", type=float, default=0.70, help="GET ratio for mixed mode")

    args = parser.parse_args()

    if args.mode == "mixed":
        if args.set_ratio < 0 or args.get_ratio < 0 or (args.set_ratio + args.get_ratio) > 1.0:
            raise ValueError("For mixed mode, set-ratio and get-ratio must be >= 0 and sum to <= 1.0")

    run_benchmark(
        total_requests=args.requests,
        concurrency=args.concurrency,
        host=args.host,
        port=args.port,
        mode=args.mode,
        key_space=args.key_space,
        set_ratio=args.set_ratio,
        get_ratio=args.get_ratio,
    )


if __name__ == "__main__":
    main()
