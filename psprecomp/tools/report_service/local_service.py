#!/usr/bin/env python3
"""A LOCAL report service for testing the installers' (and the game's) Report an issue: the real report_service.py code with a
temp config and make_issue replaced (nothing is created on GitHub). NEVER point a test at the real https://party.schindlershadow.com.

  local_service.py STORE_DIR [PORT=18792]      then PSPRECOMP_REPORT_URL=http://127.0.0.1:18792 PSPRECOMP_REPORT_NO_OPEN=1 <program>
Each accepted report is stored in STORE_DIR and its report.json is printed; the "issue" is https://example.invalid/issues/<n>.
"""
import importlib.util, json, os, sys

here = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("report_service", os.path.join(here, "report_service.py"))
rs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rs)
key = open(os.path.join(here, "../../runtime/src/report_key.h")).read().split('"')[-2]
store = os.path.abspath(sys.argv[1])
port = int(sys.argv[2]) if len(sys.argv) > 2 else 18792
os.makedirs(store, exist_ok=True)
rs.CFG.update({"github_token": "x", "repo": "x/y", "keys": [key], "public_base": "http://127.0.0.1:%d" % port,
               "store_dir": store, "port": port, "labels": []})
count = [0]


def fake_issue(r, link):
    count[0] += 1
    n = 9000 + count[0]
    sys.stderr.write("REPORT %d: %s\n" % (n, json.dumps({k: v for k, v in r.items() if k != "description"})[:900]))
    sys.stderr.write("  description: %r\n" % r.get("description"))
    sys.stderr.flush()
    return "https://example.invalid/issues/%d" % n, n


rs.make_issue = fake_issue
rs.PER_IP_HOUR = rs.PER_IP_DAY = rs.ALL_DAY = 1000   # tests send several
rs.log("local test service on 127.0.0.1:%d, store %s" % (port, store))
rs.S(("127.0.0.1", port), rs.H).serve_forever()
