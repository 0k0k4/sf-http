"""Local HTTP + HTTPS test server for http_selftest.exe (standard library only).

    python test_server.py [--port 8780] [--tls-port 8781]

The HTTPS server uses certs/localhost.pem, a self-signed certificate for "localhost" that is
created on first run (needs the `cryptography` package: pip install cryptography).

Routes (same on both servers):
  /echo                 JSON {method, path, headers, body}; X-Test is echoed as X-Test-Echo
  /status/<n>           empty response with status n
  /redirect?to=<url>    302 to <url>
  /loop                 302 to /loop (redirect loop)
  /chunked              chunked transfer encoding, "part0 part1 ... part9"
  /gzip                 gzip-encoded body ("hello gzip " x 200)
  /bytes/<n>            n bytes (pattern a..z)
  /slow?ms=<n>          waits n ms, then "slow"
  /drip?count=&ms=      Content-Length count, one byte every ms
  /truncate             announces 100 bytes, sends 10, closes
  /multi                two Set-Cookie and two X-Multi headers
"""
import argparse
import datetime
import gzip
import json
import os
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

HERE = os.path.dirname(os.path.abspath(__file__))


def make_cert(cert_path, key_path):
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    from cryptography.x509.oid import NameOID

    key = ec.generate_private_key(ec.SECP256R1())
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "localhost")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (
        x509.CertificateBuilder()
        .subject_name(name).issuer_name(name).public_key(key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(now - datetime.timedelta(days=1))
        .not_valid_after(now + datetime.timedelta(days=365))
        .add_extension(x509.SubjectAlternativeName([x509.DNSName("localhost")]), critical=False)
        .sign(key, hashes.SHA256())
    )
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    def send_body(self, status, body, content_type="text/plain", extra=None):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (extra or []):
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def read_body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def route(self):
        url = urlparse(self.path)
        q = parse_qs(url.query)
        path = url.path
        body = self.read_body()

        if path == "/echo":
            data = {
                "method": self.command,
                "path": self.path,
                "headers": {k.lower(): v for k, v in self.headers.items()},
                "body": body.decode("utf-8", "replace"),
            }
            extra = [("X-Echo-Method", self.command)]
            if self.headers.get("X-Test"):
                extra.append(("X-Test-Echo", self.headers["X-Test"]))
            return self.send_body(200, json.dumps(data), "application/json", extra)
        if path.startswith("/status/"):
            return self.send_body(int(path[8:]), "")
        if path == "/redirect":
            return self.send_body(302, "", extra=[("Location", q["to"][0])])
        if path == "/loop":
            return self.send_body(302, "", extra=[("Location", "/loop")])
        if path == "/chunked":
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(10):
                part = ("part%d " % i).encode()
                self.wfile.write(b"%x\r\n%s\r\n" % (len(part), part))
                self.wfile.flush()
                time.sleep(0.01)
            self.wfile.write(b"0\r\n\r\n")
            return
        if path == "/gzip":
            raw = gzip.compress(("hello gzip " * 200).encode())
            return self.send_body(200, raw, extra=[("Content-Encoding", "gzip")])
        if path.startswith("/bytes/"):
            n = int(path[7:])
            pattern = b"abcdefghijklmnopqrstuvwxyz"
            data = (pattern * (n // len(pattern) + 1))[:n]
            return self.send_body(200, data, "application/octet-stream")
        if path == "/slow":
            time.sleep(int(q.get("ms", ["1000"])[0]) / 1000)
            return self.send_body(200, "slow")
        if path == "/drip":
            count = int(q.get("count", ["10"])[0])
            ms = int(q.get("ms", ["500"])[0])
            self.send_response(200)
            self.send_header("Content-Length", str(count))
            self.end_headers()
            try:
                for _ in range(count):
                    self.wfile.write(b"x")
                    self.wfile.flush()
                    time.sleep(ms / 1000)
            except OSError:
                pass
            return
        if path == "/truncate":
            self.send_response(200)
            self.send_header("Content-Length", "100")
            self.end_headers()
            self.wfile.write(b"0123456789")
            self.wfile.flush()
            self.close_connection = True
            return
        if path == "/multi":
            return self.send_body(200, "multi", extra=[
                ("Set-Cookie", "a=1"), ("Set-Cookie", "b=2"), ("X-Multi", "one"), ("X-Multi", "two")])
        return self.send_body(404, "not found")

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = route


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8780)
    parser.add_argument("--tls-port", type=int, default=8781)
    args = parser.parse_args()

    plain = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    secure = ThreadingHTTPServer(("127.0.0.1", args.tls_port), Handler)
    cert_dir = os.path.join(HERE, "certs")
    os.makedirs(cert_dir, exist_ok=True)
    cert, key = os.path.join(cert_dir, "localhost.pem"), os.path.join(cert_dir, "localhost.key")
    if not os.path.exists(cert) or not os.path.exists(key):
        make_cert(cert, key)
    tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    tls.load_cert_chain(cert, key)
    secure.socket = tls.wrap_socket(secure.socket, server_side=True)
    plain.daemon_threads = secure.daemon_threads = True
    threading.Thread(target=secure.serve_forever, daemon=True).start()
    print("listening http://127.0.0.1:%d https://localhost:%d" % (args.port, args.tls_port), flush=True)
    try:
        plain.serve_forever()
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
