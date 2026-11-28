import hashlib, hmac, importlib.util, io, json, os, subprocess, sys, threading, time, urllib.request, zipfile
binary, out = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("report_service", os.path.join(here, "../../tools/report_service/report_service.py"))
rs = importlib.util.module_from_spec(spec); spec.loader.exec_module(rs)
key = open(os.path.join(here, "../src/report_key.h")).read().split('"')[-2]
store = os.path.join(out, "store"); os.makedirs(store, exist_ok=True)
rs.CFG.update({"github_token": "x", "repo": "x/y", "keys": [key], "public_base": "http://127.0.0.1:18791", "store_dir": store, "port": 18791, "labels": []})
made = []
def fake_issue(r, link):   # no GitHub
    made.append(r); return "https://example.invalid/issues/4242", 4242
rs.make_issue = fake_issue
srv = rs.S(("127.0.0.1", 18791), rs.H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
rc = subprocess.call([binary, "http://127.0.0.1:18791"])
assert rc == 0, "test_report failed"
zips = [f for f in os.listdir(store) if f.endswith(".zip")]
assert len(zips) == 1, zips
z = zipfile.ZipFile(os.path.join(store, zips[0]))
names = z.namelist(); print("zip entries:", names)
assert "report.json" in names and "last_run.log" in names and "ram.bin" in names and "settings.ini" in names and "crash-1.txt" in names
assert not any("cloud" in n for n in names)
rep = json.loads(z.read("report.json"))
assert rep["title"] == "Test title" and "badly" in rep["description"] and "alice" not in rep["description"], rep["description"]
assert rep["system"]["version"] == "test" and rep["system"]["resolution"] == "1280x720"
log = z.read("last_run.log").decode()
assert "alice" not in log and log.rstrip().endswith("end of log") and "earlier bytes left out" in log and len(log) < 8.1 * 1024 * 1024
assert "SECRET" not in z.read("settings.ini").decode()
ram = z.read("ram.bin"); assert len(ram) == 40 << 20
assert made and made[0]["title"] == "Test title"
# independent signing check in python: wrong key -> 403, right key -> accepted by the same rule
def post(k, body, t=None):
    t = str(int(time.time()) if t is None else t)
    sig = hmac.new(k.encode(), (t + "\n" + hashlib.sha256(body).hexdigest()).encode(), hashlib.sha256).hexdigest()
    rq = urllib.request.Request("http://127.0.0.1:18791/v1/report", data=body, method="POST", headers={"X-Report-Time": t, "X-Report-Sig": sig})
    try: return urllib.request.urlopen(rq).status
    except urllib.error.HTTPError as e: return e.code
assert post("wrong", b"zz") == 403
assert post(key, b"zz", int(time.time()) - 7200) == 403
print("report service test OK")
