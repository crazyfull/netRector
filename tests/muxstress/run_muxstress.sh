#!/usr/bin/env bash
# Full multiplexing stress run (inside the dev container or on Linux with BoringSSL):
#   tests/muxstress/run_muxstress.sh [asan|release] [quick]
# Scenarios: direct (small/large/tiny records), fragmented via chaos relay, TLS, fuzz against the
# server, garbage server against the netRector tunnel client. Prints every error at the end.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$HERE/../.."
VARIANT=${1:-asan}; QUICK=${2:-}
BIN="$HERE/muxstress"; [ "$VARIANT" = "asan" ] && BIN="$HERE/muxstress-asan"
[ -x "$BIN" ] || "$HERE/build.sh"
NR="$ROOT/build/netRector"; [ -x "$NR" ] || { echo "build/netRector missing (scripts/build.sh)"; }
export ASAN_OPTIONS=detect_leaks=1
L=${MUXSTRESS_LOG:-/tmp/muxstress}; rm -rf "$L"; mkdir -p "$L"
FAILS=0; RESULTS=()
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=localhost" -keyout $L/key.pem -out $L/cert.pem 2>/dev/null
M=200; [ -n "$QUICK" ] && M=40

note() { RESULTS+=("$1"); echo "$1"; }
check_log() { # name file
    if grep -q "AddressSanitizer\|runtime error\|LeakSanitizer" "$2"; then note "FAIL  $1: sanitizer report in $2"; FAILS=$((FAILS+1)); fi
}
client() { # name args...
    local name=$1; shift
    timeout 300 "$BIN" client "$@" > "$L/$name.log" 2>&1
    local rc=$?
    local line; line=$(grep "^---- client:" "$L/$name.log" | tail -1 | sed 's/^---- client: //')
    if [ $rc -eq 0 ]; then note "PASS  $name: $line"; else note "FAIL  $name (exit $rc): $line"; FAILS=$((FAILS+1)); grep "ERROR:" "$L/$name.log" | head -5; fi
    check_log "$name" "$L/$name.log"
}

"$BIN" server --listen 127.0.0.1:9300 > $L/server.log 2>&1 & SRV=$!
"$BIN" server --listen 127.0.0.1:9310 --cert $L/cert.pem --key $L/key.pem --tls-threads 2 > $L/server-tls.log 2>&1 & SRVT=$!
python3 "$HERE/chaos_relay.py" --listen 127.0.0.1:9301 --target 127.0.0.1:9300 --max-chunk 500 > $L/relay.log 2>&1 & RELAY=$!
sleep 0.8

client small     --server 127.0.0.1:9300 --streams 100 --messages $M --max-size 4000   --slow 0.3 --rst 0.1 --threads 2 --duration 200
client large     --server 127.0.0.1:9300 --streams 30  --messages $((M/4)) --max-size 262144 --slow 0.5 --rst 0.1 --threads 2 --duration 200
client tiny      --server 127.0.0.1:9300 --streams 200 --messages $M --min-size 1 --max-size 13 --slow 0.2 --rst 0.05 --threads 2 --duration 200
client fragmented --server 127.0.0.1:9301 --streams 40 --messages $((M/3)) --max-size 20000 --slow 0.3 --rst 0.1 --threads 1 --duration 280
client tls-large --server 127.0.0.1:9310 --tls --streams 30 --messages $((M/4)) --max-size 262144 --slow 0.4 --rst 0.1 --threads 2 --duration 200
client tls-tiny  --server 127.0.0.1:9310 --tls --streams 100 --messages $M --min-size 1 --max-size 13 --slow 0.2 --rst 0.05 --threads 2 --duration 200
client many-streams --server 127.0.0.1:9300 --streams 3000 --messages 3 --max-size 2000 --slow 0.1 --rst 0.1 --threads 2 --duration 200

echo "== fuzz: malformed frames against the server"
if timeout 900 python3 "$HERE/fuzz.py" server --target 127.0.0.1:9300 --rounds 120 > $L/fuzz-server.log 2>&1; then note "PASS  fuzz-server: $(tail -1 $L/fuzz-server.log)"; else note "FAIL  fuzz-server: $(grep FAIL $L/fuzz-server.log | head -2)"; FAILS=$((FAILS+1)); fi
kill -TERM $SRV $SRVT $RELAY 2>/dev/null; wait $SRV; RC=$?; wait $SRVT; RCT=$?
[ $RC -eq 0 ] || { note "FAIL  server exit code $RC"; FAILS=$((FAILS+1)); }
[ $RCT -eq 0 ] || { note "FAIL  tls server exit code $RCT"; FAILS=$((FAILS+1)); }
note "INFO  server: $(grep '^---- server:' $L/server.log | sed 's/^---- server: //')"
check_log server $L/server.log; check_log server-tls $L/server-tls.log

if [ -x "$NR" ]; then
    echo "== fuzz: garbage server against the netRector tunnel client (reconnect loop)"
    python3 "$HERE/fuzz.py" client --listen 127.0.0.1:9302 --rounds 30 --timeout 20 > $L/fuzz-client.log 2>&1 & FZ=$!
    sleep 0.3
    "$NR" client --server 127.0.0.1:9302 --listen 127.0.0.1:1085 --keepalive 2 > $L/nr-client.log 2>&1 & NRC=$!
    wait $FZ
    if kill -0 $NRC 2>/dev/null; then
        # client must still be alive and must work again with a real server
        "$NR" server --listen 127.0.0.1:9302 > $L/nr-server.log 2>&1 & NRS=$!
        sleep 3
        if python3 - <<'PY'
import socket, struct, time
# echo server for the SOCKS target
ls = socket.socket(); ls.bind(("127.0.0.1", 0)); ls.listen(5); port = ls.getsockname()[1]
ok = False
for i in range(20):
    try:
        s = socket.create_connection(("127.0.0.1", 1085), timeout=3)
        s.sendall(b"\x05\x01\x00"); assert s.recv(2) == b"\x05\x00"
        s.sendall(b"\x05\x01\x00\x01" + socket.inet_aton("127.0.0.1") + struct.pack("!H", port))
        rep = s.recv(10)
        if rep[1] == 0:
            c, _ = ls.accept(); s.sendall(b"ping"); assert c.recv(4) == b"ping"; c.sendall(b"pong"); assert s.recv(4) == b"pong"; ok = True; break
        s.close()
    except Exception:
        pass
    time.sleep(0.5)
raise SystemExit(0 if ok else 1)
PY
        then note "PASS  fuzz-client: netRector client survived $(grep -c 'served tunnel' $L/fuzz-client.log) garbage tunnels and reconnected to a real server"; else note "FAIL  fuzz-client: client alive but not working after garbage"; FAILS=$((FAILS+1)); fi
        kill -TERM $NRS 2>/dev/null; wait $NRS 2>/dev/null
    else
        note "FAIL  fuzz-client: netRector client died"; FAILS=$((FAILS+1))
    fi
    kill -TERM $NRC 2>/dev/null; wait $NRC 2>/dev/null
    grep -c "tunnel lost" $L/nr-client.log | sed 's/^/      tunnels dropped by client: /'
fi

echo
echo "================ muxstress summary ($VARIANT) ================"
for r in "${RESULTS[@]}"; do echo "$r"; done
echo "logs: $L"
if [ $FAILS -eq 0 ]; then echo "ALL PASSED"; exit 0; else echo "$FAILS FAILED"; exit 1; fi
