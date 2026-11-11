#!/usr/bin/env python3
"""Local HTTP + HTTPS server for test_http (run_http.sh). Prints 'READY <http_port> <https_port>' when listening."""
import http.server, ssl, sys, threading, time

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def _send(self, code, body=b"", extra=(), close=True):
        self.send_response(code)
        for k, v in extra: self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
    def do_any(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        p = self.path
        if p == "/hello": self._send(200, b"hello")
        elif p == "/echo":
            self._send(200, body, [("X-Method", self.command), ("X-Auth", self.headers.get("Authorization", "")),
                                   ("X-Type", self.headers.get("Content-Type", ""))])
        elif p == "/redir": self._send(302, b"", [("Location", "/hello")])
        elif p == "/redir307": self._send(307, b"", [("Location", "/echo")])
        elif p == "/redir-x": self._send(302, b"", [("Location", "http://localhost:%d/echo" % PORTS[0])])
        elif p == "/404": self._send(404, b"nope")
        elif p == "/chunked":
            self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close")
            self.end_headers()
            for part in (b"chunk1-", b"chunk2-", b"x" * 5000):
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
            self.wfile.write(b"0\r\n\r\n")
        elif p == "/big": self._send(200, bytes((i * 31) & 255 for i in range(3 * 1024 * 1024)))
        elif p == "/noclen":
            self.send_response(200); self.send_header("Connection", "close"); self.end_headers()
            self.wfile.write(b"until-close")
        elif p == "/trunc":
            self.send_response(200); self.send_header("Content-Length", "100"); self.send_header("Connection", "close")
            self.end_headers(); self.wfile.write(b"short")
        elif p == "/slow": time.sleep(3); self._send(200, b"late")
        else: self._send(400, b"?")
    do_GET = do_POST = do_PUT = do_DELETE = do_PROPFIND = do_any

PORTS = [0, 0]
class S(http.server.ThreadingHTTPServer):
    daemon_threads = True

cert, key = sys.argv[1], sys.argv[2]
plain = S(("127.0.0.1", 0), H)
secure = S(("127.0.0.1", 0), H)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
secure.socket = ctx.wrap_socket(secure.socket, server_side=True)
PORTS[0], PORTS[1] = plain.server_address[1], secure.server_address[1]
for s in (plain, secure): threading.Thread(target=s.serve_forever, daemon=True).start()
print("READY %d %d" % tuple(PORTS), flush=True)
sys.stdin.read()    # lives until the test script closes our stdin
