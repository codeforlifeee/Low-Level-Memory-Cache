#!/usr/bin/env python3
import argparse
import csv
import os
import re
import statistics
import subprocess
import sys
import time
from pathlib import Path

try:
    import matplotlib.pyplot as plt
except ImportError:
    print("ERROR: matplotlib is required. Install with: pip install matplotlib")
    sys.exit(1)


TUNED_BLOCK_RE = re.compile(
    r"\[(?P<profile>[a-z_]+)\]\s+(?P<variant>[a-z0-9_]+)\s*\n"
    r"\s*throughput ops/sec\s*:\s*(?P<throughput>[0-9.]+)\s*\n"
    r"\s*p99 latency \(us\)\s*:\s*(?P<p99>[0-9.]+)",
    re.IGNORECASE,
)

BENCH_LINE_RE = {
    "throughput": re.compile(r"Throughput \(ops/sec\):\s*([0-9.]+)", re.IGNORECASE),
    "avg": re.compile(r"Average latency \(us\):\s*([0-9.]+)", re.IGNORECASE),
    "p50": re.compile(r"P50 latency \(us\):\s*([0-9.]+)", re.IGNORECASE),
    "p95": re.compile(r"P95 latency \(us\):\s*([0-9.]+)", re.IGNORECASE),
    "p99": re.compile(r"P99 latency \(us\):\s*([0-9.]+)", re.IGNORECASE),
    "errors": re.compile(r"Transport/protocol errors:\s*([0-9]+)", re.IGNORECASE),
}


def ensure_dir(path: Path) -> None:
    path.mkdir(parents=True, exist_ok=True)


def read_text_auto(path: Path) -> str:
    raw = path.read_bytes()
    for enc in ("utf-8-sig", "utf-16", "utf-16-le", "latin-1"):
        try:
            return raw.decode(enc)
        except UnicodeDecodeError:
            continue
    return raw.decode("utf-8", errors="replace")


def parse_tuned_results(path: Path):
    text = read_text_auto(path)
    rows = []
    for m in TUNED_BLOCK_RE.finditer(text):
        rows.append(
            {
                "profile": m.group("profile"),
                "variant": m.group("variant"),
                "throughput_ops_sec": float(m.group("throughput")),
                "p99_us": float(m.group("p99")),
            }
        )
    return rows


def write_csv(path: Path, fieldnames, rows):
    with path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def build_optimization_summary(rows):
    by_profile = {}
    for row in rows:
        by_profile.setdefault(row["profile"], []).append(row)

    summary = []
    for profile, items in by_profile.items():
        base = next((x for x in items if x["variant"] == "single_lock"), None)
        best = None
        sharded = [x for x in items if x["variant"].startswith("sharded_")]
        if sharded:
            best = max(sharded, key=lambda x: x["throughput_ops_sec"])

        if base and best:
            summary.append(
                {
                    "profile": profile,
                    "baseline_variant": base["variant"],
                    "baseline_throughput_ops_sec": base["throughput_ops_sec"],
                    "best_variant": best["variant"],
                    "best_throughput_ops_sec": best["throughput_ops_sec"],
                    "baseline_p99_us": base["p99_us"],
                    "best_p99_us": best["p99_us"],
                    "throughput_gain_pct": ((best["throughput_ops_sec"] - base["throughput_ops_sec"]) / max(base["throughput_ops_sec"], 1e-9)) * 100.0,
                    "p99_reduction_pct": ((base["p99_us"] - best["p99_us"]) / max(base["p99_us"], 1e-9)) * 100.0,
                }
            )
    return summary


def plot_optimization(summary, out_png: Path):
    profiles = [x["profile"] for x in summary]
    base_tp = [x["baseline_throughput_ops_sec"] for x in summary]
    best_tp = [x["best_throughput_ops_sec"] for x in summary]
    base_p99 = [x["baseline_p99_us"] for x in summary]
    best_p99 = [x["best_p99_us"] for x in summary]

    fig, axes = plt.subplots(1, 2, figsize=(12, 4.5))

    x = range(len(profiles))
    w = 0.38

    axes[0].bar([i - w / 2 for i in x], base_tp, width=w, label="single_lock", color="#93c5fd")
    axes[0].bar([i + w / 2 for i in x], best_tp, width=w, label="best_sharded", color="#0ea5e9")
    axes[0].set_title("Throughput Comparison")
    axes[0].set_xticks(list(x), profiles)
    axes[0].set_ylabel("ops/sec")
    axes[0].legend()

    axes[1].bar([i - w / 2 for i in x], base_p99, width=w, label="single_lock", color="#fdba74")
    axes[1].bar([i + w / 2 for i in x], best_p99, width=w, label="best_sharded", color="#f97316")
    axes[1].set_title("P99 Latency Comparison")
    axes[1].set_xticks(list(x), profiles)
    axes[1].set_ylabel("microseconds")
    axes[1].legend()

    fig.tight_layout()
    fig.savefig(out_png, dpi=160)
    plt.close(fig)


def parse_benchmark_output(stdout: str):
    values = {}
    for key, pattern in BENCH_LINE_RE.items():
        m = pattern.search(stdout)
        if m:
            if key == "errors":
                values[key] = int(m.group(1))
            else:
                values[key] = float(m.group(1))
    return values


def run_sweep(project_root: Path, server_exe: Path, host: str, port: int, requests: int, concurrencies, mode: str, get_ratio: float, set_ratio: float, key_space: int):
    server_cmd = [str(server_exe), str(port), "50000"]
    server_proc = subprocess.Popen(server_cmd, cwd=str(project_root), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.0)

    rows = []
    try:
        for c in concurrencies:
            cmd = [
                sys.executable,
                str(project_root / "benchmark.py"),
                "--host",
                host,
                "--port",
                str(port),
                "--requests",
                str(requests),
                "--concurrency",
                str(c),
                "--mode",
                mode,
                "--get-ratio",
                str(get_ratio),
                "--set-ratio",
                str(set_ratio),
                "--key-space",
                str(key_space),
            ]
            proc = subprocess.run(cmd, cwd=str(project_root), capture_output=True, text=True, check=False)
            parsed = parse_benchmark_output(proc.stdout)
            parsed["concurrency"] = c
            parsed["requests"] = requests
            parsed["mode"] = mode
            parsed["return_code"] = proc.returncode
            rows.append(parsed)
            print(f"sweep concurrency={c}: throughput={parsed.get('throughput', 0):.2f} ops/sec, p99={parsed.get('p99', 0):.2f} us")
    finally:
        server_proc.terminate()
        try:
            server_proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            server_proc.kill()

    return rows


def plot_sweep(rows, throughput_png: Path, latency_png: Path):
    rows = sorted(rows, key=lambda x: x["concurrency"])
    x = [r["concurrency"] for r in rows]
    throughput = [r.get("throughput", 0.0) for r in rows]
    p50 = [r.get("p50", 0.0) for r in rows]
    p95 = [r.get("p95", 0.0) for r in rows]
    p99 = [r.get("p99", 0.0) for r in rows]

    fig = plt.figure(figsize=(7.5, 4.5))
    plt.plot(x, throughput, marker="o", color="#0ea5e9")
    plt.title("Throughput vs Concurrency")
    plt.xlabel("Concurrency (clients)")
    plt.ylabel("Throughput (ops/sec)")
    plt.grid(alpha=0.25)
    plt.tight_layout()
    plt.savefig(throughput_png, dpi=160)
    plt.close(fig)

    fig = plt.figure(figsize=(7.5, 4.5))
    plt.plot(x, p50, marker="o", label="P50", color="#22c55e")
    plt.plot(x, p95, marker="o", label="P95", color="#f59e0b")
    plt.plot(x, p99, marker="o", label="P99", color="#ef4444")
    plt.title("Latency vs Concurrency")
    plt.xlabel("Concurrency (clients)")
    plt.ylabel("Latency (us)")
    plt.grid(alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(latency_png, dpi=160)
    plt.close(fig)


def main():
    parser = argparse.ArgumentParser(description="Generate benchmark CSV/graphs for resume-ready proof.")
    parser.add_argument("--project-root", default=".", help="Project root directory")
    parser.add_argument("--tuned-output", default="benchmark_output.txt", help="Path to tuned benchmark output file")
    parser.add_argument("--out-dir", default="docs/graphs", help="Output directory for CSV and PNG artifacts")

    parser.add_argument("--run-sweep", action="store_true", help="Run TCP concurrency sweep using benchmark.py")
    parser.add_argument("--server-exe", default="build/Release/cache_tcp_server.exe", help="Path to cache_tcp_server executable")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=6379)
    parser.add_argument("--requests", type=int, default=20000)
    parser.add_argument("--mode", default="mixed", choices=["ping", "kv", "mixed"])
    parser.add_argument("--get-ratio", type=float, default=0.70)
    parser.add_argument("--set-ratio", type=float, default=0.25)
    parser.add_argument("--key-space", type=int, default=20000)
    parser.add_argument("--concurrency-list", default="10,50,100,200,500", help="Comma-separated list")

    args = parser.parse_args()

    project_root = Path(args.project_root).resolve()
    out_dir = (project_root / args.out_dir).resolve()
    ensure_dir(out_dir)

    tuned_path = (project_root / args.tuned_output).resolve()
    tuned_rows = parse_tuned_results(tuned_path)
    if tuned_rows:
        write_csv(out_dir / "tuned_benchmark_raw.csv", ["profile", "variant", "throughput_ops_sec", "p99_us"], tuned_rows)
        summary = build_optimization_summary(tuned_rows)
        write_csv(
            out_dir / "optimization_comparison.csv",
            [
                "profile",
                "baseline_variant",
                "baseline_throughput_ops_sec",
                "best_variant",
                "best_throughput_ops_sec",
                "baseline_p99_us",
                "best_p99_us",
                "throughput_gain_pct",
                "p99_reduction_pct",
            ],
            summary,
        )
        plot_optimization(summary, out_dir / "optimization_comparison.png")
        print(f"generated: {out_dir / 'optimization_comparison.png'}")

    if args.run_sweep:
        concurrencies = [int(x.strip()) for x in args.concurrency_list.split(",") if x.strip()]
        server_exe = (project_root / args.server_exe).resolve()
        rows = run_sweep(
            project_root,
            server_exe,
            args.host,
            args.port,
            args.requests,
            concurrencies,
            args.mode,
            args.get_ratio,
            args.set_ratio,
            args.key_space,
        )
        write_csv(
            out_dir / "concurrency_sweep.csv",
            ["concurrency", "requests", "mode", "throughput", "avg", "p50", "p95", "p99", "errors", "return_code"],
            rows,
        )
        plot_sweep(rows, out_dir / "throughput_vs_concurrency.png", out_dir / "latency_vs_concurrency.png")
        print(f"generated: {out_dir / 'throughput_vs_concurrency.png'}")
        print(f"generated: {out_dir / 'latency_vs_concurrency.png'}")

    print("done")


if __name__ == "__main__":
    main()
