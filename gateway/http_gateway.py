#!/usr/bin/env python3
"""HTTP gateway in front of FlashKV, so a browser can talk to it.

FlashKV speaks one JSON object per request over raw TCP, which no browser can
do. This translates URLs into those requests:

    GET /api/SET/foo/bar   ->  {"cmd": "SET", "args": ["foo", "bar"]}
    GET /api/GET/foo       ->  {"cmd": "GET", "args": ["foo"]}

and serves a one-page console at /.

Run it next to a FlashKV server:

    python3 gateway/http_gateway.py --backend-port 6380
    # then open http://localhost:8080

Binds to 127.0.0.1 by default on purpose: this exposes the whole key-value
store with no authentication, so it must be opted into with --host.
"""

import argparse
import json
import os
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote, urlparse

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'client'))
from flashkv_client import FlashKVClient  # noqa: E402

INDEX_HTML = """<!doctype html>
<title>FlashKV console</title>
<style>
  body { font: 15px/1.5 system-ui, sans-serif; max-width: 46rem; margin: 3rem auto;
         padding: 0 1rem; background: #101418; color: #e6e6e6; }
  h1 { font-size: 1.3rem; }
  input { width: 100%; padding: .6rem .7rem; font: inherit; font-family: monospace;
          background: #1b2027; color: #e6e6e6; border: 1px solid #333c47; border-radius: 6px; }
  pre { background: #1b2027; border: 1px solid #333c47; border-radius: 6px;
        padding: .8rem; overflow-x: auto; white-space: pre-wrap; }
  .hint { color: #8b98a5; font-size: .85rem; }
  .err { color: #ff8a80; }
</style>
<h1>FlashKV console</h1>
<p class="hint">Type a command and press Enter. Example:
  <code>SET foo bar</code>, <code>GET foo</code>, <code>LPUSH l a b c</code>,
  <code>LRANGE l 0 -1</code>, <code>KEYS</code></p>
<input id="cmd" autofocus autocomplete="off" placeholder="GET foo">
<pre id="out">ready</pre>
<script>
const input = document.getElementById('cmd');
const out = document.getElementById('out');

input.addEventListener('keydown', async (e) => {
  if (e.key !== 'Enter') return;
  const parts = input.value.trim().split(/\\s+/).filter(Boolean);
  if (!parts.length) return;

  const url = '/api/' + parts.map(encodeURIComponent).join('/');
  try {
    const res = await fetch(url);
    const body = await res.json();
    out.className = body.status === 'ERROR' ? 'err' : '';
    out.textContent = JSON.stringify(body, null, 2);
  } catch (err) {
    out.className = 'err';
    out.textContent = 'gateway error: ' + err;
  }
  input.select();
});
</script>
"""


def parse_path(path):
    """'/api/SET/foo/bar' -> ('SET', ['foo', 'bar']). Returns None if not a command."""
    parts = [unquote(p) for p in urlparse(path).path.split('/') if p]
    if len(parts) < 2 or parts[0] != 'api':
        return None
    return parts[1], parts[2:]


class GatewayHandler(BaseHTTPRequestHandler):
    backend_host = 'localhost'
    backend_port = 6379

    server_version = 'FlashKVGateway/1.0'
    protocol_version = 'HTTP/1.1'

    def do_GET(self):
        route = urlparse(self.path).path

        if route == '/':
            return self._send(200, INDEX_HTML.encode(), 'text/html; charset=utf-8')

        if route == '/favicon.ico':
            return self._send(204, b'', 'image/x-icon')

        parsed = parse_path(self.path)
        if not parsed:
            return self._send_json(404, {"status": "ERROR",
                                         "error_msg": "unknown route, use /api/<CMD>/<arg>...",
                                         "data": None})

        cmd, args = parsed
        try:
            # ponytail: one backend connection per request. Pool them if this
            # ever sits in a hot path; for a debug console it is free.
            with FlashKVClient(self.backend_host, self.backend_port) as client:
                response = client.send_command(cmd, *args)
        except (OSError, ConnectionError) as e:
            return self._send_json(502, {"status": "ERROR",
                                         "error_msg": f"cannot reach FlashKV at "
                                                      f"{self.backend_host}:{self.backend_port}: {e}",
                                         "data": None})

        status = 400 if response.get('status') == 'ERROR' else 200
        return self._send_json(status, response)

    def _send_json(self, code, payload):
        self._send(code, json.dumps(payload).encode(), 'application/json')

    def _send(self, code, body, content_type):
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def log_message(self, fmt, *args):
        sys.stderr.write("[gateway] %s - %s\n" % (self.address_string(), fmt % args))


def selftest():
    assert parse_path('/api/SET/foo/bar') == ('SET', ['foo', 'bar'])
    assert parse_path('/api/GET/foo') == ('GET', ['foo'])
    assert parse_path('/api/KEYS') == ('KEYS', [])
    assert parse_path('/api/GET/a%20b') == ('GET', ['a b'])
    assert parse_path('/api/LRANGE/l/0/-1') == ('LRANGE', ['l', '0', '-1'])
    assert parse_path('/api/GET/foo?x=1') == ('GET', ['foo'])
    assert parse_path('/') is None
    assert parse_path('/api') is None
    assert parse_path('/nope/GET/foo') is None
    print("selftest OK")


def main():
    parser = argparse.ArgumentParser(description='HTTP gateway for FlashKV')
    parser.add_argument('--host', default='127.0.0.1',
                        help='interface to bind (default 127.0.0.1; the store has no auth)')
    parser.add_argument('--port', type=int, default=8080)
    parser.add_argument('--backend-host', default='localhost')
    parser.add_argument('--backend-port', type=int, default=6379)
    parser.add_argument('--selftest', action='store_true', help='check URL parsing and exit')
    args = parser.parse_args()

    if args.selftest:
        selftest()
        return 0

    GatewayHandler.backend_host = args.backend_host
    GatewayHandler.backend_port = args.backend_port

    httpd = ThreadingHTTPServer((args.host, args.port), GatewayHandler)
    print(f"FlashKV HTTP gateway on http://{args.host}:{args.port} "
          f"-> {args.backend_host}:{args.backend_port}")
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nshutting down")
    finally:
        httpd.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
