#!/usr/bin/env python3
"""Fake WebDAV (/dav/) and Google OAuth + Drive v3 (drive.file scope, files in a folder the app creates) server for test_cloud_providers (run_cloud.sh).
Prints 'READY <port>'. Admin: GET /admin/expire (invalidates access tokens), /admin/reset, /admin/stats."""
import http.server, json, sys, threading, urllib.parse, base64, re

lock = threading.Lock()
S = {}
def reset():
    S.clear()
    S.update(dav_dir=False, dav={}, drive={}, next_id=1, tokens=set(), token_n=0, polls=0, refreshes=0, device_starts=0, dav_requests=0, folders_created=0)
reset()
DAV_USER, DAV_PASS = "me", "app-password"
PAGE = 2

class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def send(self, code, body=b"", ctype="application/json", extra=()):
        if isinstance(body, (dict, list)): body = json.dumps(body).encode()
        if isinstance(body, str): body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        for k, v in extra: self.send_header(k, v)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def handle_any(self):
        n = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(n) if n else b""
        u = urllib.parse.urlsplit(self.path)
        q = urllib.parse.parse_qs(u.query)
        with lock:
            if u.path.startswith("/dav"): return self.dav(u, body)
            if u.path == "/admin/expire": S["tokens"].clear(); return self.send(200, {})
            if u.path == "/admin/reset": reset(); return self.send(200, {})
            if u.path == "/admin/stats": return self.send(200, {k: S[k] for k in ("refreshes", "polls", "device_starts", "dav_requests", "folders_created")})
            if u.path == "/device/code": return self.device_code(body)
            if u.path == "/token": return self.token(body)
            if u.path.startswith("/drive/") or u.path.startswith("/upload/drive/"): return self.drive(u, q, body)
        self.send(404, {"error": "not found"})

    # ---------------- WebDAV
    def dav(self, u, body):
        S["dav_requests"] += 1
        auth = self.headers.get("Authorization", "")
        want = "Basic " + base64.b64encode(f"{DAV_USER}:{DAV_PASS}".encode()).decode()
        if auth != want: return self.send(401, b"", "text/plain", [("WWW-Authenticate", 'Basic realm="x"')])
        path = urllib.parse.unquote(u.path)[len("/dav"):]
        m = self.command
        if path in ("", "/"):
            if m == "PROPFIND":
                if not S["dav_dir"]: return self.send(404, b"")
                out = '<?xml version="1.0"?><D:multistatus xmlns:D="DAV:">'
                out += '<D:response><D:href>/dav/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop></D:propstat></D:response>'
                for k, v in sorted(S["dav"].items()):
                    href = "/dav/" + urllib.parse.quote(k)
                    out += (f'<D:response><D:href>{href}</D:href><D:propstat><D:prop><D:getcontentlength>{len(v)}</D:getcontentlength>'
                            f'<D:resourcetype/></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>')
                out += "</D:multistatus>"
                return self.send(207, out, "application/xml")
            if m == "MKCOL":
                if S["dav_dir"]: return self.send(405, b"")
                S["dav_dir"] = True; return self.send(201, b"")
            return self.send(405, b"")
        name = path.lstrip("/")
        if not S["dav_dir"] and m in ("PUT",): return self.send(409, b"")
        if m == "PUT": new = name not in S["dav"]; S["dav"][name] = body; return self.send(201 if new else 204, b"")
        if m == "GET":
            if name not in S["dav"]: return self.send(404, b"")
            return self.send(200, S["dav"][name], "application/octet-stream")
        if m == "DELETE":
            if name not in S["dav"]: return self.send(404, b"")
            del S["dav"][name]; return self.send(204, b"")
        self.send(405, b"")

    # ---------------- OAuth
    def device_code(self, body):
        f = urllib.parse.parse_qs(body.decode())
        if f.get("scope", [""])[0] != "https://www.googleapis.com/auth/drive.file":
            return self.send(400, {"error": "invalid_scope", "error_description": "Invalid device flow scope: " + f.get("scope", [""])[0]})   # as Google does for drive.appdata
        if f.get("client_id", [""])[0] == "bad": return self.send(401, {"error": "invalid_client", "error_description": "no such client"})
        S["device_starts"] += 1; S["polls"] = 0
        self.send(200, {"device_code": "DEV-" + f["client_id"][0], "user_code": "ABCD-EFGH", "verification_url": "https://www.google.com/device",
                        "expires_in": 1800, "interval": 1})

    def token(self, body):
        f = {k: v[0] for k, v in urllib.parse.parse_qs(body.decode()).items()}
        if f.get("client_secret") != "SECRET": return self.send(401, {"error": "invalid_client"})
        g = f.get("grant_type")
        if g == "urn:ietf:params:oauth:grant-type:device_code":
            if f.get("client_id") == "deny": return self.send(403, {"error": "access_denied"})
            if f.get("client_id") == "expire": return self.send(400, {"error": "expired_token"})
            S["polls"] += 1
            if f.get("client_id") == "fast": S["polls"] = 4
            if S["polls"] <= 2: return self.send(428, {"error": "authorization_pending"})
            if S["polls"] == 3: return self.send(400, {"error": "slow_down"})
            return self.send(200, {"access_token": "unused", "refresh_token": "RT1", "expires_in": 3599, "token_type": "Bearer"})
        if g == "refresh_token":
            if f.get("refresh_token") != "RT1": return self.send(400, {"error": "invalid_grant"})
            S["refreshes"] += 1; S["token_n"] += 1
            t = "AT%d" % S["token_n"]; S["tokens"].add(t)
            return self.send(200, {"access_token": t, "expires_in": 3599, "token_type": "Bearer"})
        self.send(400, {"error": "unsupported_grant_type"})

    # ---------------- Drive
    def drive(self, u, q, body):
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("Bearer ") or auth[7:] not in S["tokens"]:
            return self.send(401, {"error": {"code": 401, "message": "Invalid Credentials"}})
        m, p = self.command, u.path
        files = S["drive"]
        if p == "/drive/v3/files" and m == "GET":
            if q.get("spaces"): return self.send(400, {"error": "spaces not used with drive.file"})
            items = sorted(files.items(), key=lambda kv: int(kv[0]))
            qq = q.get("q", [""])[0]
            if "trashed = false" not in qq: return self.send(400, {"error": "bad q"})
            nm = re.search(r"name = '(.*?)'", qq)
            mt = re.search(r"mimeType = '(.*?)'", qq)
            par = re.search(r"'(\w+)' in parents", qq)
            if not par: return self.send(400, {"error": "bad q (parent)"})
            items = [kv for kv in items if par.group(1) in kv[1]["parents"]
                     and (not nm or kv[1]["name"] == nm.group(1)) and (not mt or kv[1]["mime"] == mt.group(1))]
            if not mt: items = [kv for kv in items if kv[1]["mime"] != "application/vnd.google-apps.folder"]
            start = int(q.get("pageToken", ["0"])[0])
            page = items[start:start + PAGE]
            out = {"files": [{"id": i, "name": f["name"], "size": str(len(f["data"]))} for i, f in page]}
            if start + PAGE < len(items): out["nextPageToken"] = str(start + PAGE)
            return self.send(200, out)
        if p == "/drive/v3/files" and m == "POST":      # create a folder
            meta = json.loads(body)
            if meta.get("mimeType") != "application/vnd.google-apps.folder": return self.send(400, {"error": "folders only"})
            fid = str(S["next_id"]); S["next_id"] += 1
            files[fid] = {"name": meta["name"], "data": b"", "parents": meta.get("parents", ["root"]), "mime": meta["mimeType"]}
            S["folders_created"] += 1
            return self.send(200, {"id": fid})
        mm = re.match(r"^/drive/v3/files/(\d+)$", p)
        if mm:
            fid = mm.group(1)
            if fid not in files: return self.send(404, {"error": "not found"})
            if m == "GET" and q.get("alt", [""])[0] == "media": return self.send(200, files[fid]["data"], "application/octet-stream")
            if m == "DELETE": del files[fid]; return self.send(204, b"")
        if p == "/upload/drive/v3/files" and m == "POST" and q.get("uploadType", [""])[0] == "multipart":
            ct = self.headers.get("Content-Type", "")
            b = ct.split("boundary=")[1].encode()
            parts = [x for x in body.split(b"--" + b) if x.strip(b"-\r\n")]
            meta = json.loads(parts[0].split(b"\r\n\r\n", 1)[1].rstrip(b"\r\n"))
            data = parts[1].split(b"\r\n\r\n", 1)[1]
            assert data.endswith(b"\r\n"); data = data[:-2]
            pid = (meta.get("parents") or [""])[0]
            if pid not in files or files[pid]["mime"] != "application/vnd.google-apps.folder": return self.send(400, {"error": "parent is not a folder"})
            fid = str(S["next_id"]); S["next_id"] += 1
            files[fid] = {"name": meta["name"], "data": data, "parents": [pid], "mime": "application/octet-stream"}
            return self.send(200, {"id": fid})
        mm = re.match(r"^/upload/drive/v3/files/(\d+)$", p)
        if mm and m == "PATCH" and q.get("uploadType", [""])[0] == "media":
            fid = mm.group(1)
            if fid not in files: return self.send(404, {"error": "not found"})
            files[fid]["data"] = body
            return self.send(200, {"id": fid})
        self.send(404, {"error": "no route " + m + " " + p})

    do_GET = do_POST = do_PUT = do_PATCH = do_DELETE = do_PROPFIND = do_MKCOL = handle_any

class S_(http.server.ThreadingHTTPServer):
    daemon_threads = True

srv = S_(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
print("READY %d" % srv.server_address[1], flush=True)
sys.stdin.read()
