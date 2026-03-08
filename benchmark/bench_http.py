#!/usr/bin/env python3
"""
HTTP 服务器性能压测脚本。
测试项：静态资源 GET、/tick API；输出 QPS、延迟（平均/中位数/P95/P99）。
用法: python3 bench_http.py [base_url] [并发数] [持续时间秒]
默认: http://127.0.0.1:8080  8  10
"""
import sys
import time
import statistics
import threading
import urllib.request
from collections import deque

def measure_one(url, timeout=5):
    t0 = time.perf_counter()
    try:
        with urllib.request.urlopen(url, timeout=timeout) as _:
            pass
        return (time.perf_counter() - t0) * 1000  # ms
    except Exception:
        return None

def worker(base_url, path, duration_sec, results_list, lock, stop_event):
    url = base_url.rstrip("/") + path
    while not stop_event.is_set():
        lat = measure_one(url)
        if lat is not None:
            with lock:
                results_list.append(lat)

def run_benchmark(base_url, path, concurrency, duration_sec):
    results = []
    lock = threading.Lock()
    stop_event = threading.Event()
    threads = []
    for _ in range(concurrency):
        t = threading.Thread(target=worker, args=(base_url, path, duration_sec, results, lock, stop_event))
        threads.append(t)
        t.start()
    time.sleep(duration_sec)
    stop_event.set()
    for t in threads:
        t.join()
    return results

def percentile(sorted_arr, p):
    if not sorted_arr:
        return 0
    k = (len(sorted_arr) - 1) * p / 100
    f = int(k)
    c = min(f + 1, len(sorted_arr) - 1)
    return sorted_arr[f] + (k - f) * (sorted_arr[c] - sorted_arr[f])

def report(name, results, total_sec):
    if not results:
        print("%s: 无有效样本" % name)
        return
    n = len(results)
    sorted_lat = sorted(results)
    qps = n / total_sec
    avg = statistics.mean(results)
    med = statistics.median(results)
    p95 = percentile(sorted_lat, 95)
    p99 = percentile(sorted_lat, 99)
    print("[%s] 样本数=%d, 总时长=%.1fs" % (name, n, total_sec))
    print("  QPS: %.0f" % qps)
    print("  延迟(ms): 平均=%.2f, 中位数=%.2f, P95=%.2f, P99=%.2f" % (avg, med, p95, p99))

def main():
    base_url = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"
    concurrency = int(sys.argv[2]) if len(sys.argv) > 2 else 8
    duration_sec = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    print("Base URL: %s, 并发: %d, 时长: %ds" % (base_url, concurrency, duration_sec))
    print("--- 静态资源 GET /index.html ---")
    r1 = run_benchmark(base_url, "/index.html", concurrency, duration_sec)
    report("GET /index.html", r1, duration_sec)
    print("--- /tick API (step=1&value=1.0) ---")
    r2 = run_benchmark(base_url, "/tick?step=1&value=1.0", concurrency, duration_sec)
    report("GET /tick", r2, duration_sec)
    print("--- 高并发 32 线程 /index.html ---")
    r3 = run_benchmark(base_url, "/index.html", 32, duration_sec)
    report("GET /index.html (32 并发)", r3, duration_sec)

if __name__ == "__main__":
    main()
