#!/usr/bin/env python3
"""Minimal HTTP server for UEFI HTTP client integration tests.

Provides /hello (JSON), /redirect (302 → /hello), /chunked (Transfer-
Encoding: chunked, multi-chunk body), and 404 for anything else.
The UEFI guest reaches this via QEMU gateway at 10.0.2.2.

Usage: python3 host-server.py <port>
"""

import json
import socket
import ssl
import struct
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler


# Cross-request state for the streaming-PUT integration tests. The
# UEFI guest issues a PUT to /upload (multi-chunk producer driven by
# axl_http_request_streaming) and then GETs /last-upload to verify
# the host received the bytes exactly. Single global slot is fine —
# tests are sequential, not concurrent.
_LAST_UPLOAD = {"len": 0, "head_hex": "", "tail_hex": ""}

# Optional override for /redirect's Location, set from --redirect-base. Lets a
# plain-http instance redirect into an https instance (cross-scheme redirect).
_REDIRECT_BASE: str | None = None

# Connections accepted so far (see Handler.setup).
_CONN_SEQ = [0]

# The client's cap on interim 1xx heads before a final response
# (HTTP_CLIENT_INTERIM_MAX); /resp/1xx-flood-ka sends one more.
_INTERIM_CAP = 16

# Arrivals of a counted /resp/ route since its last read (/arrivals/<name>).
_ARRIVALS: dict[str, int] = {}


class Handler(BaseHTTPRequestHandler):
    def _path_echo(self) -> None:
        # /path-echo/<anything>: the length of the request target exactly as
        # it arrived -- what the client sent, cut or whole. A PUT's body is
        # read and dropped.
        te = self.headers.get("Transfer-Encoding", "").lower()
        cl = int(self.headers.get("Content-Length", "0"))
        if "chunked" in te:
            while True:
                n = int(self.rfile.readline().strip().split(b";", 1)[0] or b"0", 16)
                if n == 0:
                    while self.rfile.readline().strip():
                        pass
                    break
                self.rfile.read(n + 2)
        elif cl:
            self.rfile.read(cl)
        body = json.dumps({"len": len(self.path)}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_PUT(self):
        if self.path.startswith("/path-echo/"):
            self._path_echo()
            return
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

    def do_POST(self):
        # /echo returns the posted body verbatim — drives the async POST
        # integration test (the UEFI guest asserts the echoed body arrives
        # via the completion callback).
        if self.path == "/echo":
            cl = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(cl) if cl else b""
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.send_header("Connection", "close")
            self.end_headers()

    def setup(self) -> None:
        # Each connection gets a serial number: a response that reports it
        # shows whether the client sent its next request on the SAME
        # connection (reused) or a new one.
        super().setup()
        _CONN_SEQ[0] += 1
        self.conn_id = _CONN_SEQ[0]
        self.req_seq = 0

    def _resp_raw(self) -> bool:
        # /resp/<name>: a response written byte for byte, for the shapes a
        # user agent must read as RFC 9112 says -- a fold, a repeated or a
        # differing Content-Length, Content-Length with chunked on a
        # connection left open, bare-LF line ends, and a Transfer-Encoding
        # whose last coding is not chunked (its body runs to the close).
        if not self.path.startswith("/resp/"):
            return False
        cl = int(self.headers.get("Content-Length", "0") or "0")
        if cl:
            self.rfile.read(cl)
        name = self.path[len("/resp/"):]
        self.req_seq += 1
        if name.startswith("host-echo"):
            # The Host header exactly as it arrived (RFC 9110 7.2).
            body = (self.headers.get("Host") or "(none)").encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return True
        if name.startswith("redirect-tls?port="):
            # A 302 to the TLS instance's /resp/host-echo on another port: the
            # Host a client sends after the redirect must name the NEW target.
            port = name.split("=", 1)[1]
            self.send_response(302)
            self.send_header("Location", f"https://10.0.2.2:{port}/resp/host-echo")
            self.send_header("Content-Length", "0")
            self.send_header("Connection", "close")
            self.end_headers()
            return True
        if self._resp_interim(name):
            return True
        conn = f"conn-{self.conn_id}".encode()
        chunk = f"{len(conn):x}\r\n".encode() + conn + b"\r\n0\r\n\r\n"
        raw = {
            "fold": b"HTTP/1.1 200 OK\r\nX-Folded: a\r\n b\r\n\t c\r\n"
                    b"Content-Length: 5\r\nConnection: close\r\n\r\nhello",
            "dup-cl": b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                      b"Content-Length: 5\r\nConnection: close\r\n\r\nhello",
            "list-cl": b"HTTP/1.1 200 OK\r\nContent-Length: 5, 5\r\n"
                       b"Connection: close\r\n\r\nhello",
            "diff-cl": b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n"
                       b"Content-Length: 6\r\nConnection: close\r\n\r\nhello!",
            "bare-lf": b"HTTP/1.1 200 OK\nContent-Length: 5\nX-Lf: y\n"
                       b"Connection: close\n\nhello",
            "te-close": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n"
                        b"Content-Length: 3\r\n\r\nhello-until-close",
            "cl-te-ka": b"HTTP/1.1 200 OK\r\nContent-Length: 999\r\n"
                        b"Transfer-Encoding: chunked\r\n\r\n" + chunk,
            "te-ka": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunk,
            "te-close-rst": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\npartial",
            # RFC 9112 6.3 rule 8: no Content-Length, no Transfer-Encoding --
            # the body runs to the close; and rule 1's exceptions, which have
            # no body whatever they say, on a connection left OPEN, so a
            # client that waits for a body hangs.
            "close-200": b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nbody-runs-to-the-close",
            "close-200-rst": b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial",
            "204-ka": b"HTTP/1.1 204 No Content\r\n\r\n",
            "304-ka": b"HTTP/1.1 304 Not Modified\r\nETag: \"x\"\r\n\r\n",
            "head-ka": b"HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n",
            "head-cl-ka": b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
            # Part of a body, then nothing for 4 s (past a 2 s client idle
            # timeout), then the close: the client must time out mid-body.
            "close-200-stall": b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial",
            "cl-stall": b"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\npartial",
            # Content-Length 20, 7 bytes of it, then the close (over TLS, no
            # close_notify): a cut that no close_notify mode may accept.
            "cl-cut-nonotify": b"HTTP/1.1 200 OK\r\nContent-Length: 20\r\n\r\npartial",
            "te-close-notify": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n"
                               b"notify-body-intact",
            "te-close-nonotify": b"HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\n"
                                 b"cut-body-no-notify",
        }.get(name)
        if raw is None:
            self.send_response(404)
            self.send_header("Connection", "close")
            self.end_headers()
            return True
        self.wfile.write(raw)
        self.wfile.flush()
        if name.endswith("-stall"):
            time.sleep(4)
            self.close_connection = True
            return True
        if name.endswith("-rst"):
            # Part of a body framed by the close, then a RESET, not a FIN:
            # the body is cut, and a client must not take it for whole.
            # The pause lets the part reach the client before the RST
            # (SO_LINGER 0 discards what is still unsent).
            time.sleep(0.5)
            self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                       struct.pack("ii", 1, 0))
            self.connection.close()
            self.close_connection = True
            return True
        if name.endswith("-notify") and isinstance(self.connection, ssl.SSLSocket):
            # Over TLS, the orderly end of a stream is close_notify (the
            # server's default close sends none -- that is -nonotify).
            try:
                self.connection.unwrap()
            except (OSError, ssl.SSLError):
                pass
            self.close_connection = True
            return True
        # The -ka shapes leave the connection open for a next request; the
        # rest close it (te-close's body is framed BY the close).
        self.close_connection = not name.endswith("-ka")
        return True

    def _resp_interim(self, name: str) -> bool:
        # Interim 1xx heads before the final response (RFC 9110 15.2), on a
        # connection left open: the final body names the connection and the
        # request's place on it, so a client that takes a 1xx for the final
        # response -- and reads this 200 as the NEXT request's -- shows.
        body = f"c{self.conn_id}r{self.req_seq}".encode()
        final = (b"HTTP/1.1 200 OK\r\nContent-Length: " + str(len(body)).encode()
                 + b"\r\n\r\n" + body)
        hundred = b"HTTP/1.1 100 Continue\r\n\r\n"
        early = b"HTTP/1.1 103 Early Hints\r\nLink: </s.css>; rel=preload\r\n\r\n"
        if name == "100-200-ka":
            self.wfile.write(hundred)
            self.wfile.flush()
            time.sleep(0.3)   # the final head arrives in a later segment
            self.wfile.write(final)
        elif name == "103-100-200-ka":
            self.wfile.write(early + hundred + final)   # all in one segment
        elif name == "1xx-flood-ka":
            # The final head comes late: by then the client has given up on
            # the request, so a client that kept the connection reads this
            # 200 as its NEXT request's.
            self.wfile.write(hundred * (_INTERIM_CAP + 1))
            self.wfile.flush()
            time.sleep(0.5)
            self.wfile.write(final)
        elif name == "100-rst-ka":
            # Arrivals are counted: the connection's FIRST request is answered
            # 200 (keep-alive), its SECOND 100 Continue then a reset. A client
            # that took the reset for a stale kept-alive connection would
            # replay the request -- a second arrival of the same request.
            _ARRIVALS[name] = _ARRIVALS.get(name, 0) + 1
            if self.req_seq == 1:
                self.wfile.write(final)
            else:
                self.wfile.write(hundred)
                self.wfile.flush()
                time.sleep(0.3)
                self.connection.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER,
                                           struct.pack("ii", 1, 0))
                self.connection.close()
                self.close_connection = True
                return True
        elif name == "101-ka":
            self.wfile.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n"
                             b"Connection: Upgrade\r\n\r\n")
        else:
            return False
        self.wfile.flush()
        self.close_connection = False
        return True

    def do_HEAD(self):
        if self._resp_raw():
            return
        self.send_response(404)
        self.send_header("Connection", "close")
        self.end_headers()

    def do_GET(self):
        if self.path.startswith("/arrivals/"):
            # How many times /resp/<name> arrived since the last read (reset).
            name = self.path[len("/arrivals/"):]
            body = str(_ARRIVALS.pop(name, 0)).encode()
            self.send_response(200)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return
        if self._resp_raw():
            return
        if self.path.startswith("/path-echo/"):
            self._path_echo()
            return
        if self.path == "/last-upload":
            body = json.dumps(_LAST_UPLOAD).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close")
            self.end_headers()
            self.wfile.write(body)
            return
        if self.path.startswith("/large"):
            # A large, deterministic, single Content-Length body — byte i is
            # (i & 0xFF). Drives the one-shot whole-body GET path (the
            # gBS->LoadImage-over-AxlFsProvider regression: a >1 MiB GET was
            # capped at 1 MiB in the async core). Size via ?size=N (default
            # 1.5 MiB, comfortably over the old 1 MiB cap).
            from urllib.parse import urlparse, parse_qs

            q = parse_qs(urlparse(self.path).query)
            n = int(q.get("size", ["1572864"])[0])
            body = bytes((i & 0xFF) for i in range(n))
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(n))
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
            # Default: redirect to /hello on this same (http) server. With
            # --redirect-base set, redirect to {base}/hello instead — used to
            # drive an http->https cross-scheme redirect in the https-client
            # test (a plain-http server pointing at the https server).
            if _REDIRECT_BASE is not None:
                location = f"{_REDIRECT_BASE}/hello"
            else:
                port = self.server.server_address[1]
                location = f"http://10.0.2.2:{port}/hello"
            self.send_response(302)
            self.send_header("Location", location)
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
    # Usage: host-server.py <port> [--tls <cert.pem> <key.pem>]
    #                              [--redirect-base <scheme://host:port>]
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18081
    if "--redirect-base" in sys.argv:
        _REDIRECT_BASE = sys.argv[sys.argv.index("--redirect-base") + 1]
    httpd = HTTPServer(("0.0.0.0", port), Handler)
    if "--tls" in sys.argv:
        i = sys.argv.index("--tls")
        cert, key = sys.argv[i + 1], sys.argv[i + 2]
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(certfile=cert, keyfile=key)
        httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)
    httpd.serve_forever()
