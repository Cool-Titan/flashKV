#!/usr/bin/env python3
"""FlashKV command line client.

Usage:
    python3 flashkv_client.py SET foo bar
    python3 flashkv_client.py GET foo
    python3 flashkv_client.py LPUSH mylist a b c
    python3 flashkv_client.py --interactive
"""

import argparse
import json
import socket
import sys


class FlashKVClient:
    """Thin JSON-over-TCP client. One connection, many commands."""

    def __init__(self, host='localhost', port=6379, timeout=10.0):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.socket = None
        self._buffer = b''

    def connect(self):
        self.socket = socket.create_connection((self.host, self.port), self.timeout)
        return self

    def close(self):
        if self.socket:
            self.socket.close()
            self.socket = None

    def __enter__(self):
        return self.connect()

    def __exit__(self, *_exc):
        self.close()

    def send_command(self, cmd, *args):
        """Send one command and return the decoded response dict."""
        request = {"cmd": cmd}
        if args:
            request["args"] = [str(a) for a in args]

        self.socket.sendall(json.dumps(request).encode())
        return self._read_response()

    def _read_response(self):
        # Responses are newline terminated, so read until one shows up. A
        # single recv() is not enough once replies get large.
        while b'\n' not in self._buffer:
            chunk = self.socket.recv(4096)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self._buffer += chunk

        line, self._buffer = self._buffer.split(b'\n', 1)
        return json.loads(line.decode())


def print_response(response):
    if response.get('status') == 'ERROR':
        print(f"ERROR: {response.get('error_msg', 'Unknown error')}", file=sys.stderr)
        return 1
    print(json.dumps(response.get('data'), indent=2))
    return 0


def interactive_loop(client):
    print("FlashKV interactive client. Ctrl-D or 'quit' to exit.")
    while True:
        try:
            line = input("flashkv> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return 0
        if not line:
            continue
        if line.lower() in ('quit', 'exit'):
            return 0

        parts = line.split()
        try:
            print_response(client.send_command(parts[0], *parts[1:]))
        except (ConnectionError, OSError, json.JSONDecodeError) as e:
            print(f"Connection error: {e}", file=sys.stderr)
            return 1


def main():
    parser = argparse.ArgumentParser(description='FlashKV CLI Client')
    parser.add_argument('--host', default='localhost')
    parser.add_argument('--port', type=int, default=6379)
    parser.add_argument('--interactive', '-i', action='store_true',
                        help='start a REPL instead of running a single command')
    parser.add_argument('command', nargs='*', help='Command and arguments')

    args = parser.parse_args()

    if not args.command and not args.interactive:
        parser.error('give a command, or use --interactive')

    try:
        with FlashKVClient(args.host, args.port) as client:
            if args.interactive:
                return interactive_loop(client)
            response = client.send_command(args.command[0], *args.command[1:])
            return print_response(response)
    except (OSError, ConnectionError) as e:
        print(f"Connection error: {e}", file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
