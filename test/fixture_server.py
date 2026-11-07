#!/usr/bin/env python3
"""Local HTTP server for the http_client fixture tests (test/sql/httpclient_fixture_*.test).

Endpoints (every method):
  /echo                 200, JSON {method, path, query, headers, body}
  /status/<code>        empty response with that status
  /redirect?to=<url>[&code=<3xx>]   redirect to <url> (default 302)
  /big?bytes=<n>        n bytes of "x"
  /binary               a body that is not valid UTF-8
  /unicode              a UTF-8 body
  /multi                response with repeated and quoted headers
  /slow?seconds=<n>     waits n seconds, then 200

Usage:
  python3 test/fixture_server.py [port]   (default 8766)
  HTTP_CLIENT_FIXTURE_URL=http://127.0.0.1:8766 HTTP_CLIENT_HTTPFS=/path/to/httpfs.duckdb_extension \
    ./build/release/test/unittest 'test/sql/httpclient_fixture*'
"""
import http.server
import json
import sys
import time
import urllib.parse


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def _send(self, status, body=b"", headers=None):
        self.send_response(status)
        for k, v in (headers or []):
            self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def _handle(self):
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query, keep_blank_values=True)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        path = parsed.path
        if path == "/echo":
            payload = {
                "method": self.command,
                "path": path,
                "query": {k: v if len(v) > 1 else v[0] for k, v in query.items()},
                "raw_query": parsed.query,
                "headers": {k.lower(): v for k, v in self.headers.items()},
                "body": body.decode("utf-8", "replace"),
            }
            self._send(200, json.dumps(payload, sort_keys=True).encode(), [("Content-Type", "application/json")])
        elif path.startswith("/status/"):
            self._send(int(path.split("/")[2]))
        elif path == "/redirect":
            self._send(int(query.get("code", ["302"])[0]), b"", [("Location", query["to"][0])])
        elif path == "/big":
            self._send(200, b"x" * int(query.get("bytes", ["1024"])[0]))
        elif path == "/binary":
            self._send(200, b"\xff\xfe\x00\x01", [("Content-Type", "application/octet-stream")])
        elif path == "/unicode":
            self._send(200, "héllo wörld 🦆 \"q\"".encode("utf-8"), [("Content-Type", "text/plain; charset=utf-8")])
        elif path == "/multi":
            self._send(200, b"ok", [("X-Quoted", 'say "hi"\\'), ("X-Repeat", "a"), ("X-Repeat", "b")])
        elif path == "/slow":
            time.sleep(float(query.get("seconds", ["5"])[0]))
            self._send(200, b"slow")
        else:
            self._send(404, b"not found")

    do_GET = do_POST = do_PUT = do_DELETE = do_HEAD = do_PATCH = _handle

    def log_message(self, format, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8766
    http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
