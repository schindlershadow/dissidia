#!/bin/bash
# Adds the /dissidia-report/ proxy to the HTTPS VirtualHost that serves the public name, then checks and reloads Apache.
# Run with sudo. Idempotent (does nothing when the block is already there); on a failed config test the old file is restored.
# usage: sudo ./install_apache.sh [conf file] [server name]
set -euo pipefail
CONF=${1:-/etc/apache2/sites-enabled/schindlershadow.duckdns.org-le-ssl.conf}
NAME=${2:-party.schindlershadow.com}
MARK="# dissidia-report (report_service.py)"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
[ -f "$CONF" ] || { echo "no such file: $CONF"; exit 1; }
CONF=$(readlink -f "$CONF")
if grep -qF "$MARK" "$CONF"; then echo "already installed in $CONF"; exit 0; fi
grep -q "ServerAlias.*$NAME\|ServerName.*$NAME" "$CONF" || { echo "$NAME is not served by $CONF"; exit 1; }
BAK="$CONF.bak-dissidia-report-$(date +%Y%m%d%H%M%S)"
cp -p "$CONF" "$BAK"
python3 - "$CONF" "$NAME" "$MARK" <<'EOF'
import re, sys
p, name, mark = sys.argv[1:]
s = open(p).read()
block = (f"    {mark}\n"
         "    ProxyPass        /dissidia-report/ http://127.0.0.1:8790/dissidia-report/\n"
         "    ProxyPassReverse /dissidia-report/ http://127.0.0.1:8790/dissidia-report/\n"
         "    <Location /dissidia-report/>\n"
         "        LimitRequestBody 27262976\n"
         "    </Location>\n")
# the <VirtualHost> ... </VirtualHost> section that names the server
for m in re.finditer(r"<VirtualHost[^>]*>.*?</VirtualHost>", s, flags=re.S):
    if re.search(r"Server(Name|Alias)\s+.*\b" + re.escape(name) + r"\b", m.group(0)):
        end = m.end() - len("</VirtualHost>")
        s = s[:end] + block + s[end:]
        open(p, "w").write(s)
        sys.exit(0)
sys.exit("no VirtualHost with " + name)
EOF
if apache2ctl configtest; then
    systemctl reload apache2
    echo "installed; backup at $BAK"
else
    cp -p "$BAK" "$CONF"
    echo "config test FAILED: restored the old file"
    exit 1
fi
