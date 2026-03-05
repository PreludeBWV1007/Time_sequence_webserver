#!/usr/bin/env python3
"""
时序信号发送：先发起始时间点 step=0，等待后并发发 step=1..100。
消费者仅当堆顶为“下一步”时才处理，避免前面的步被丢。
用法: python3 send_ticks_concurrent.py [base_url]
默认: http://127.0.0.1:8080
"""
import sys
import time
import threading
import urllib.request

def send_one(base_url, step, value):
    url = "%s/tick?step=%d&value=%s" % (base_url.rstrip('/'), step, value)
    try:
        with urllib.request.urlopen(url, timeout=10) as _:
            pass
    except Exception as e:
        print("error step=%d: %s" % (step, e))

def worker(base_url, points):
    for step, value in points:
        send_one(base_url, step, value)

def main():
    base_url = sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8080"
    # 1) 先发起始时间点 step=0，保证入队后再发后续，避免前面数据被丢
    send_one(base_url, 0, 0.0)
    time.sleep(0.5)
    # 2) 并发发 step=1..100，(step, value) = (1,10), (2,20), ..., (100,1000)
    rest_signal = [(i, i * 10.0) for i in range(1, 101)]
    n_threads = 5
    chunk_size = 20
    threads = []
    for t in range(n_threads):
        start = t * chunk_size
        chunk = rest_signal[start : start + chunk_size]
        th = threading.Thread(target=worker, args=(base_url, chunk))
        threads.append(th)
        th.start()
    for th in threads:
        th.join()
    print("sent step 0 then 1..100. open %s/state to see ordered result (*2)." % base_url)

if __name__ == "__main__":
    main()
