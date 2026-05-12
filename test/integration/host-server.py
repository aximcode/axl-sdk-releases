#!/usr/bin/env python3
"""Minimal HTTP server for UEFI HTTP client integration tests.

Provides /hello (JSON), /redirect (302 → /hello), /chunked (Transfer-
Encoding: chunked, multi-chunk body), and 404 for anything else.
The UEFI guest reaches this via QEMU gateway at 10.0.2.2.

Usage: python3 host-server.py <port>
"""

import json
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler


# Cross-request state for the streaming-PUT integration tests. The
# UEFI guest issues a PUT to /upload (multi-chunk producer driven by
# axl_http_request_streaming) and then GETs /last-upload to verify
# the host received the bytes exactly. Single global slot is fine —
# tests are sequential, not concurrent.
_LAST_UPLOAD = {"len": 0, "head_hex": "", "tail_hex": ""}


class Handler(BaseHTTPRequestHandler):
    def do_PUT(self):
        if self.path == "/upload":
            # Honor either Content-Length or Transfer-Encoding:
            # chunked so the same endpoint validates both client
            # framing modes.
            te = self.headers.get("Transfer-Encoding", "").lower()
            if "chunked" in te:
                body = bytearray()
                rfile = self.rfile
                while True:
                    line = rfile.readline().strip()
                    if not line:
                        continue
                    n = int(line.split(b";", 1)[0], 16)
                    if n == 0:
                        # Read and discard trailers up to blank line.
                        while rfile.readline().strip():
                            pass
                        break
                    body.extend(rfile.read(n))
                    rfile.read(2)  # trailing CRLF
            else:
                cl = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(cl) if cl else b""
            _LAST_UPLOAD["len"] = len(body)
            _LAST_UPLOAD["head_hex"] = body[:8].hex()
            _LAST_UPLOAD["tail_hex"] = body[-8:].hex() if len(body) >= 8 else body.hex()
            self.send_response(201)
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
        else:
            self.send_response(404)
            self.send_header("Connection", "close")
            self.end_headers()

    def do_GET(self):
        if self.path == "/last-upload":
            body = json.dumps(_LAST_UPLOAD).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return
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
        elif self.path == "/chunked":
            # Manually emit Transfer-Encoding: chunked across multiple
            # chunks. BaseHTTPRequestHandler doesn't do chunked output
            # natively, so we write the framing bytes ourselves. The
            # client must concatenate all chunk bodies — assertions key
            # on the exact decoded bytes "hello-chunk-second!!" (20).
            self.wfile.write(b"HTTP/1.1 200 OK\r\n")
            self.wfile.write(b"Content-Type: text/plain\r\n")
            self.wfile.write(b"Transfer-Encoding: chunked\r\n")
            self.wfile.write(b"Connection: close\r\n")
            self.wfile.write(b"\r\n")
            self.wfile.write(b"b\r\nhello-chunk\r\n")
            self.wfile.write(b"9\r\n-second!!\r\n")
            self.wfile.write(b"0\r\n\r\n")
        elif self.path == "/chunked-ext":
            # Chunk extensions (`b;name=foo`) and trailers (`X-Trace`)
            # are valid HTTP/1.1 framing that some real servers emit.
            # Decoded body must still be "hello-chunk-second!!".
            self.wfile.write(b"HTTP/1.1 200 OK\r\n")
            self.wfile.write(b"Content-Type: text/plain\r\n")
            self.wfile.write(b"Transfer-Encoding: chunked\r\n")
            self.wfile.write(b"Connection: close\r\n")
            self.wfile.write(b"\r\n")
            self.wfile.write(b"b;name=foo\r\nhello-chunk\r\n")
            self.wfile.write(b"9; ext=bar=baz\r\n-second!!\r\n")
            self.wfile.write(b"0\r\nX-Trace: abc\r\n\r\n")
        elif self.path == "/chunked-with-cl":
            # Pathological: both Content-Length AND Transfer-Encoding.
            # RFC 7230 §3.3.3 says chunked wins; client must ignore
            # the bogus 999. Decoded body = "hello-chunk-second!!".
            self.wfile.write(b"HTTP/1.1 200 OK\r\n")
            self.wfile.write(b"Content-Type: text/plain\r\n")
            self.wfile.write(b"Content-Length: 999\r\n")
            self.wfile.write(b"Transfer-Encoding: chunked\r\n")
            self.wfile.write(b"Connection: close\r\n")
            self.wfile.write(b"\r\n")
            self.wfile.write(b"b\r\nhello-chunk\r\n")
            self.wfile.write(b"9\r\n-second!!\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_response(404)
            self.send_header("Connection", "close")
            self.end_headers()

    def log_message(self, fmt, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18081
    HTTPServer(("0.0.0.0", port), Handler).serve_forever()
