#!/usr/bin/env python3
"""Issue report service for the game's "Report an issue" button.

The game POSTs one zip (logs, system info, optional memory dump, and report.json with the player's description) to
  POST <base>/v1/report      headers: X-Report-Time: <unix seconds>, X-Report-Sig: hex HMAC-SHA256(key, "<time>\n<sha256 hex of body>")
The service checks the signature, the size and the rate limits, stores the zip under a random name, creates a GitHub
issue (title + description + system info + a download link to the zip) with a token that never leaves this machine,
and answers {"ok": true, "issue_url": ..., "number": ...}. The game then opens issue_url in the browser.
  GET  <base>/f/<id>.zip     the stored zip (the link in the issue)
  GET  <base>/v1/ping        {"ok": true} (lets the game tell "service down" from "no network")

The pre-shared key ships inside the game, so it is not secret from a determined person; it keeps scanners and
drive-by spam out. The rate limits, size caps and disk cap are the real protection.

Config: JSON file (default ~/.config/dissidia-report/config.json, or $DISSIDIA_REPORT_CONFIG):
  {"github_token": "...", "repo": "owner/name", "keys": ["psk1", ...], "public_base": "https://host/dissidia-report",
   "store_dir": "~/dissidia-reports", "port": 8790, "labels": ["user-report"]}
Several keys may be listed so an old release keeps working after the key is rotated.
Runs on 127.0.0.1 behind Apache (ProxyPass), stdlib only.
"""
import hashlib, hmac, http.server, io, json, os, re, secrets, socketserver, sys, threading, time, urllib.request, zipfile

MAX_BODY = 25 * 1024 * 1024          # one report
MAX_STORE = 5 * 1024 * 1024 * 1024   # all stored reports; the oldest are deleted past this
KEEP_DAYS = 120
PER_IP_HOUR, PER_IP_DAY, ALL_DAY = 3, 10, 60
CLOCK_SKEW = 15 * 60                 # a signature is valid this long around its time stamp
MAX_DESC, MAX_TITLE = 8000, 120

CFG = {}
LOCK = threading.Lock()
HITS = {}          # ip -> [times]
ALL_HITS = []
SEEN_SIGS = {}     # sig -> time (replay guard)


def log(msg):
    sys.stderr.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
    sys.stderr.flush()


def load_cfg():
    p = os.environ.get("DISSIDIA_REPORT_CONFIG", os.path.expanduser("~/.config/dissidia-report/config.json"))
    with open(p) as f:
        c = json.load(f)
    c.setdefault("port", 8790)
    c.setdefault("labels", ["user-report"])
    c["store_dir"] = os.path.expanduser(c.get("store_dir", "~/dissidia-reports"))
    os.makedirs(c["store_dir"], exist_ok=True)
    for k in ("github_token", "repo", "keys", "public_base"):
        if not c.get(k):
            raise SystemExit("config: missing " + k)
    return c


def rate_ok(ip):
    now = time.time()
    with LOCK:
        h = [t for t in HITS.get(ip, []) if now - t < 86400]
        HITS[ip] = h
        ALL_HITS[:] = [t for t in ALL_HITS if now - t < 86400]
        if sum(1 for t in h if now - t < 3600) >= PER_IP_HOUR or len(h) >= PER_IP_DAY or len(ALL_HITS) >= ALL_DAY:
            return False
        h.append(now)
        ALL_HITS.append(now)
        return True


def sig_ok(ts, sig, body):
    try:
        t = int(ts)
    except (TypeError, ValueError):
        return False
    now = time.time()
    if abs(now - t) > CLOCK_SKEW or not re.fullmatch(r"[0-9a-f]{64}", sig or ""):
        return False
    msg = ("%d\n%s" % (t, hashlib.sha256(body).hexdigest())).encode()
    good = any(hmac.compare_digest(hmac.new(k.encode(), msg, hashlib.sha256).hexdigest(), sig) for k in CFG["keys"])
    if not good:
        return False
    with LOCK:
        for s, st in list(SEEN_SIGS.items()):
            if now - st > 2 * CLOCK_SKEW:
                del SEEN_SIGS[s]
        if sig in SEEN_SIGS:
            return False
        SEEN_SIGS[sig] = now
    return True


def prune_store():
    d = CFG["store_dir"]
    files = sorted((os.path.getmtime(os.path.join(d, n)), os.path.getsize(os.path.join(d, n)), n)
                   for n in os.listdir(d) if n.endswith(".zip"))
    total = sum(s for _, s, _ in files)
    now = time.time()
    for mt, sz, n in files:
        if now - mt > KEEP_DAYS * 86400 or total > MAX_STORE:
            os.remove(os.path.join(d, n))
            total -= sz


def clean(s, n):
    s = "".join(ch for ch in str(s) if ch == "\n" or ch == "\t" or ord(ch) >= 32)[:n]
    return s.replace("@", "@​")   # no mentions / pings from player text


def read_report(body):
    """The zip must be a real zip with report.json; returns its dict."""
    z = zipfile.ZipFile(io.BytesIO(body))
    if sum(i.file_size for i in z.infolist()) > 400 * 1024 * 1024:   # zip bomb guard
        raise ValueError("too big unpacked")
    r = json.loads(z.read("report.json").decode("utf-8", "replace"))
    if not isinstance(r, dict):
        raise ValueError("report.json is not an object")
    r["_files"] = [(i.filename, i.file_size) for i in z.infolist()][:60]
    return r


def make_issue(r, link):
    desc = clean(r.get("description", ""), MAX_DESC).strip() or "(no description)"
    title = clean(r.get("title", ""), MAX_TITLE).strip() or clean(desc.splitlines()[0], 80)
    info = r.get("system", {}) if isinstance(r.get("system"), dict) else {}
    lines = ["**Player's description**", "", "\n".join("> " + l for l in desc.splitlines()), "", "**System**", ""]
    for k, v in list(info.items())[:40]:
        lines.append("- %s: `%s`" % (clean(k, 40), clean(v, 200).replace("`", "'")))
    lines += ["", "**Logs and data**: [%s](%s) (zip, kept %d days)" % (link.rsplit("/", 1)[-1], link, KEEP_DAYS), "",
              "<details><summary>Files in the zip</summary>", ""]
    lines += ["- `%s` (%d KB)" % (clean(n, 120).replace("`", "'"), s // 1024) for n, s in r["_files"]]
    lines += ["", "</details>", "", "_Sent from the %s's Report an issue button._" % ("installer" if info.get("product") == "installer" else "game")]
    data = json.dumps({"title": "[Report] " + title, "body": "\n".join(lines), "labels": CFG["labels"]}).encode()
    req = urllib.request.Request("https://api.github.com/repos/%s/issues" % CFG["repo"], data=data, method="POST", headers={
        "Authorization": "Bearer " + CFG["github_token"], "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28", "Content-Type": "application/json", "User-Agent": "dissidia-report-service"})
    with urllib.request.urlopen(req, timeout=30) as resp:
        j = json.load(resp)
    return j["html_url"], j["number"]


class H(http.server.BaseHTTPRequestHandler):
    server_version = "dissidia-report/1"

    def ip(self):
        xff = self.headers.get("X-Forwarded-For", "")
        return xff.split(",")[-1].strip() if xff else self.client_address[0]

    def reply(self, code, obj=None, body=None, ctype="application/json"):
        b = body if body is not None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(b)))
        self.end_headers()
        self.wfile.write(b)

    def log_message(self, fmt, *a):
        log("%s %s" % (self.ip(), fmt % a))

    def do_GET(self):
        p = self.path.split("?", 1)[0]
        if p.endswith("/v1/ping"):
            return self.reply(200, {"ok": True})
        m = re.search(r"/f/([0-9a-f]{32}\.zip)$", p)
        if m and os.path.isfile(os.path.join(CFG["store_dir"], m.group(1))):
            with open(os.path.join(CFG["store_dir"], m.group(1)), "rb") as f:
                return self.reply(200, body=f.read(), ctype="application/zip")
        self.reply(404, {"ok": False, "error": "not found"})

    def do_POST(self):
        if not self.path.split("?", 1)[0].endswith("/v1/report"):
            return self.reply(404, {"ok": False, "error": "not found"})
        try:
            n = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            n = 0
        if n <= 0 or n > MAX_BODY:
            return self.reply(413, {"ok": False, "error": "the report is too big (max %d MB)" % (MAX_BODY >> 20)})
        body = self.rfile.read(n)
        if not sig_ok(self.headers.get("X-Report-Time"), self.headers.get("X-Report-Sig"), body):
            return self.reply(403, {"ok": False, "error": "bad signature or clock (check the device's date and time)"})
        if not rate_ok(self.ip()):
            return self.reply(429, {"ok": False, "error": "too many reports from here, please try again later"})
        try:
            r = read_report(body)
        except Exception as e:
            return self.reply(400, {"ok": False, "error": "not a valid report zip (%s)" % e.__class__.__name__})
        rid = secrets.token_hex(16)
        with open(os.path.join(CFG["store_dir"], rid + ".zip"), "wb") as f:
            f.write(body)
        try:
            prune_store()
        except OSError as e:
            log("prune: %s" % e)
        link = CFG["public_base"].rstrip("/") + "/f/" + rid + ".zip"
        try:
            url, num = make_issue(r, link)
        except Exception as e:
            log("github: %s" % e)
            return self.reply(502, {"ok": False, "error": "could not create the GitHub issue", "zip_url": link})
        log("report %s -> issue #%s (%d bytes)" % (rid, num, n))
        self.reply(200, {"ok": True, "issue_url": url, "number": num, "zip_url": link})


class S(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True


if __name__ == "__main__":
    CFG.update(load_cfg())
    log("listening on 127.0.0.1:%d, repo %s" % (CFG["port"], CFG["repo"]))
    S(("127.0.0.1", CFG["port"]), H).serve_forever()
