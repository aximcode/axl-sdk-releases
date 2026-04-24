#!/usr/bin/env python3
"""Minimal HTTP server for UEFI HTTP client integration tests.

Provides /hello (JSON), /redirect (302 → /hello), and 404 for anything else.
The UEFI guest reaches this via QEMU gateway at 10.0.2.2.

Usage: python3 host-server.py <port>
"""

import json
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/hello":
            body = json.dumps({"message": "hello from host"}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/redirect":
            port = self.server.server_address[1]
            self.send_response(302)
            self.send_header("Location", f"http://10.0.2.2:{port}/hello")
            self.send_header("Connection", "close")
            self.end_headers()
        else:
            self.send_response(404)
            self.send_header("Connection", "close")
            self.end_headers()

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18081
    HTTPServer(("0.0.0.0", port), Handler).serve_forever()
