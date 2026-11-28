#!/bin/bash
# hang.sh NAME "ENV..": run a party scenario, after the rebuild ask the game's debug socket for its recent guest functions (the hung loop)
N=$1; E=$2
cd ~/code/dissidia-tagslot; rm -rf ~/dissidia/tt/$N
env WALK=party_battle_walk.txt PSPRECOMP_TAGTEAM=1 PSPRECOMP_TAGTEAM_LOG=1 BT_NO_FREEZE=1 $E timeout 600 python3 .scratch/ts_run.py ~/dissidia/tt/$N ~/dissidia/tt/long70.txt 30 > ~/dissidia/tt/$N.log 2>&1 &
HP=$!
for i in $(seq 1 140); do grep -q "built as char" ~/dissidia/tt/$N/run_30.log 2>/dev/null && break; sleep 4; done
sleep 12
PID=$(pgrep -f "^/home/schindler/dissidia/work_tagslot/build/psprecomp_runtime" | head -1)
PORT=$(tr '\0' '\n' < /proc/$PID/environ | grep PSPRECOMP_DEBUG_PORT | cut -d= -f2)
echo pid=$PID port=$PORT
for k in 1 2 3; do python3 - $PORT <<'P'
import socket,sys
s=socket.create_connection(('127.0.0.1',int(sys.argv[1])),timeout=10); s.sendall(b'I\n'); f=s.makefile("rb"); f.readline(); l=f.readline().decode(); print(l[:1800])
P
sleep 1; done
pkill -P $HP 2>/dev/null; kill $PID 2>/dev/null
