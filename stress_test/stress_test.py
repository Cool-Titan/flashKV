#!/usr/bin/env python3
"""FlashKV stress test: many concurrent clients hammering the server.

Each worker thread opens ONE connection and reuses it, which is what a real
client does. Reconnecting per operation would measure the TCP handshake more
than the server.
"""

import argparse
import json
import random
import socket
import statistics
import threading
import time


class Worker:
    """One connection, one thread, N operations."""

    OPERATIONS = ['SET', 'GET', 'DEL', 'LPUSH', 'LPOP', 'SADD', 'HSET', 'INCR']

    def __init__(self, host, port, thread_id, ops, results):
        self.host = host
        self.port = port
        self.thread_id = thread_id
        self.ops = ops
        self.results = results
        self.buffer = b''

    def build_request(self, op, key, seq):
        if op == 'SET':
            return {"cmd": "SET", "key": key, "value": f"val_{seq}"}
        if op == 'GET':
            return {"cmd": "GET", "key": key}
        if op == 'DEL':
            return {"cmd": "DEL", "args": [key]}
        if op == 'LPUSH':
            return {"cmd": "LPUSH", "key": f"{key}_list", "elements": ["elem"]}
        if op == 'LPOP':
            return {"cmd": "LPOP", "key": f"{key}_list"}
        if op == 'SADD':
            return {"cmd": "SADD", "key": f"{key}_set", "members": [f"m_{seq}"]}
        if op == 'HSET':
            return {"cmd": "HSET", "key": f"{key}_hash", "fields": {"f": str(seq)}}
        return {"cmd": "INCR", "key": f"{key}_counter"}

    def read_response(self, sock):
        while b'\n' not in self.buffer:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buffer += chunk
        line, self.buffer = self.buffer.split(b'\n', 1)
        return json.loads(line.decode())

    def run(self):
        rng = random.Random(self.thread_id)
        latencies = []
        success = errors = 0

        try:
            sock = socket.create_connection((self.host, self.port), timeout=10)
        except OSError as e:
            self.results.record(0, self.ops, [], f"connect failed: {e}")
            return

        with sock:
            for seq in range(self.ops):
                op = rng.choice(self.OPERATIONS)
                key = f"key_{self.thread_id}_{seq % 100}"
                request = json.dumps(self.build_request(op, key, seq)).encode()

                started = time.perf_counter()
                try:
                    sock.sendall(request)
                    response = self.read_response(sock)
                    latencies.append((time.perf_counter() - started) * 1000.0)
                    if response.get('status') in ('OK', 'PONG'):
                        success += 1
                    else:
                        errors += 1
                except (OSError, ConnectionError, json.JSONDecodeError) as e:
                    errors += 1
                    self.results.note_error(str(e))
                    break

        self.results.record(success, errors, latencies)


class Results:
    def __init__(self):
        self.lock = threading.Lock()
        self.success = 0
        self.errors = 0
        self.latencies = []
        self.first_error = None

    def record(self, success, errors, latencies, error=None):
        with self.lock:
            self.success += success
            self.errors += errors
            self.latencies.extend(latencies)
            if error and not self.first_error:
                self.first_error = error

    def note_error(self, message):
        with self.lock:
            if not self.first_error:
                self.first_error = message


def run_stress_test(host, port, num_threads, ops_per_thread):
    total_ops = num_threads * ops_per_thread
    print(f"Starting stress test: {num_threads} threads, {ops_per_thread} ops each")
    print(f"Total operations: {total_ops}")

    results = Results()
    threads = [
        threading.Thread(target=Worker(host, port, i, ops_per_thread, results).run)
        for i in range(num_threads)
    ]

    start = time.time()
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    elapsed = time.time() - start

    latencies = sorted(results.latencies)
    print("\n=== STRESS TEST RESULTS ===")
    print(f"Total operations:  {total_ops}")
    print(f"Successful:        {results.success}")
    print(f"Errors:            {results.errors}")
    print(f"Error rate:        {100 * results.errors / total_ops:.2f}%")
    print(f"Duration:          {elapsed:.2f}s")
    print(f"Throughput:        {total_ops / elapsed:.0f} ops/sec")
    if latencies:
        print(f"Latency mean:      {statistics.mean(latencies):.3f} ms")
        print(f"Latency p50:       {latencies[len(latencies) // 2]:.3f} ms")
        print(f"Latency p99:       {latencies[int(len(latencies) * 0.99)]:.3f} ms")
    if results.first_error:
        print(f"First error:       {results.first_error}")

    # Non-zero exit on a bad run makes this usable as a CI gate.
    return 0 if results.errors == 0 else 1


def main():
    parser = argparse.ArgumentParser(description='FlashKV stress test')
    parser.add_argument('--host', default='localhost')
    parser.add_argument('--port', type=int, default=6379)
    parser.add_argument('--threads', type=int, default=50)
    parser.add_argument('--ops', type=int, default=1000)
    args = parser.parse_args()

    return run_stress_test(args.host, args.port, args.threads, args.ops)


if __name__ == '__main__':
    raise SystemExit(main())
