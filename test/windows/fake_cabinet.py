#!/usr/bin/env python3
"""Minimal in-memory Cabinet server for exercising the Windows CLI (reverse-tunnel it to the PC: ssh -R 8765:127.0.0.1:8765).
Implements /api/pair, /api/me and the /api/cloud/* file store (docs/cloud-api.md)."""
import hashlib, json, sys, time, urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CODE = "ABCDEFGHJKMN"
TOKEN = "cab_" + "A" * 43
files = {}
log = open(sys.argv[2] if len(sys.argv) > 2 else "fake_cabinet.log", "a", buffering=1)

class H(BaseHTTPRequestHandler):
    def _send(self, status, body=b"", ctype="application/json", headers=None):
        if isinstance(body, (dict, list)): body = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        for k, v in (headers or {}).items(): self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD": self.wfile.write(body)

    def _authed(self):
        if self.headers.get("Authorization") == "Bearer " + TOKEN: return True
        self._send(401, {"error": "unauthorized"}); return False

    def _route(self):
        u = urllib.parse.urlsplit(self.path)
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        log.write(f"{self.command} {u.path} ua={self.headers.get('User-Agent')!r} len={n}\n")
        if u.path == "/api/pair" and self.command == "POST":
            req = json.loads(body or b"{}")
            log.write(f"  deviceName={req.get('deviceName')!r}\n")
            if req.get("code") != CODE: return self._send(401, {"error": "invalid_code"})
            return self._send(200, {"token": TOKEN, "user": {"id": "u_wine", "name": "Wine Test"}})
        if not self._authed(): return
        if u.path == "/api/me": return self._send(200, {"user": {"id": "u_wine", "name": "Wine Test"}})
        if u.path == "/api/cloud/list":
            q = urllib.parse.parse_qs(u.query); p = q.get("prefix", [""])[0]
            out = [dict(path=k, size=len(v), sha256=hashlib.sha256(v).hexdigest(), modifiedTime=int(time.time()))
                   for k, v in sorted(files.items()) if k.startswith(p)]
            return self._send(200, {"files": out, "next": None})
        if u.path.startswith("/api/cloud/files/"):
            key = urllib.parse.unquote(u.path[len("/api/cloud/files/"):])
            if self.command == "PUT":
                files[key] = body
                return self._send(200, dict(path=key, size=len(body), sha256=hashlib.sha256(body).hexdigest(), modifiedTime=int(time.time())))
            if self.command in ("GET", "HEAD"):
                if key not in files: return self._send(404, {"error": "not_found"})
                return self._send(200, files[key], "application/octet-stream", {"ETag": '"%s"' % hashlib.sha256(files[key]).hexdigest()})
            if self.command == "DELETE":
                files.pop(key, None); return self._send(200, {"ok": True})
        self._send(404, {"error": "not_found"})

    do_GET = do_PUT = do_POST = do_DELETE = do_HEAD = _route
    def log_message(self, *a): pass

ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8765), H).serve_forever()
