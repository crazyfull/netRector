#!/usr/bin/env bash
# netRector vs nginx benchmark (run inside the dev container: nginx, wrk, openssl present).
#   bench/bench.sh [workers=4] [duration=10]
# Servers are pinned to the first `workers` CPUs, wrk to the rest, so both sides get the same CPU budget.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
W=${1:-4}; D=${2:-10}
BIN=${BIN:-$ROOT/build/netRector}
NCPU=$(nproc)
SRV_CPUS="0-$((W-1))"; WRK_CPUS="$W-$((NCPU-1))"; WRK_T=$((NCPU-W))
L=/tmp/bench; mkdir -p $L /tmp/bench-www
head -c 102400 /dev/urandom > /tmp/bench-www/file.bin
[ -f $L/cert.pem ] || openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=localhost" -keyout $L/key.pem -out $L/cert.pem 2>/dev/null
sed "s/WORKERS/$W/; s#CERT#$L/cert.pem#; s#KEY#$L/key.pem#" "$ROOT/bench/nginx.conf" > $L/nginx.conf
ulimit -n 1048576 2>/dev/null

run_wrk() { # name url [extra wrk args]
    local name=$1 url=$2; shift 2
    local out; out=$(taskset -c $WRK_CPUS wrk -t$WRK_T -d${D}s --timeout 10s "$@" "$url" 2>&1)
    local rps lat p99 err
    rps=$(echo "$out" | awk '/Requests\/sec/ {print $2}')
    lat=$(echo "$out" | awk '/^ *Latency/ {print $2}')
    p99=$(echo "$out" | awk '/ 99%/ {print "p99:" $2} /^ *Latency/ {print "max:" $4}' | tr '\n' ' ')
    err=$(echo "$out" | awk '/Socket errors/ {print "errors:" $4 $6 $8 $10}' )
    err2=$(echo "$out" | awk '/Non-2xx/ {print "non2xx:" $NF}')
    printf "%-62s %12s req/s   avg-lat %-9s %s %s %s\n" "$name" "$rps" "$lat" "$p99" "$err" "$err2"
}

rss_mb() { ps -o rss= -p "$@" 2>/dev/null | awk '{s+=$1} END {printf "%.0f", s/1024}'; }

echo "== netRector vs nginx | server workers=$W (cpus $SRV_CPUS) | wrk threads=$WRK_T (cpus $WRK_CPUS) | ${D}s each"
echo

# ---- nginx
taskset -c $SRV_CPUS nginx -c $L/nginx.conf; sleep 0.5
NGX_PIDS=$(pgrep -d, -f "nginx: worker")
echo "--- nginx"
run_wrk "nginx keep-alive, 64 conn"        http://127.0.0.1:8081/ -c64
run_wrk "nginx keep-alive, 1000 conn"      http://127.0.0.1:8081/ -c1000
run_wrk "nginx keep-alive, 10000 conn"     http://127.0.0.1:8081/ -c10000
run_wrk "nginx 100KB file, 256 conn"       http://127.0.0.1:8081/file -c256 --latency
run_wrk "nginx Connection: close, 256 conn" http://127.0.0.1:8081/ -c256 -H "Connection: close"
run_wrk "nginx TLS keep-alive, 1000 conn"  https://127.0.0.1:8444/ -c1000
run_wrk "nginx TLS Connection: close, 64 conn (handshakes)" https://127.0.0.1:8444/ -c64 -H "Connection: close"
echo "nginx RSS after tests: $(rss_mb ${NGX_PIDS//,/ }) MB"
nginx -c $L/nginx.conf -s quit; sleep 1

# ---- netRector
start_nr() { taskset -c $SRV_CPUS $BIN http --listen 0.0.0.0:8080 --threads $W "$@" > $L/nr.log 2>&1 & NR=$!; sleep 0.5; }
stop_nr() { kill -TERM $NR; wait $NR 2>/dev/null; }
echo "--- netRector"
start_nr
run_wrk "netRector keep-alive, 64 conn"        http://127.0.0.1:8080/ -c64
run_wrk "netRector keep-alive, 1000 conn"      http://127.0.0.1:8080/ -c1000
run_wrk "netRector keep-alive, 10000 conn"     http://127.0.0.1:8080/ -c10000
run_wrk "netRector Connection: close, 256 conn" http://127.0.0.1:8080/ -c256 -H "Connection: close"
echo "netRector RSS after tests: $(rss_mb $NR) MB"
stop_nr
start_nr --file /tmp/bench-www/file.bin
run_wrk "netRector 100KB file (sendfile), 256 conn" http://127.0.0.1:8080/ -c256 --latency
stop_nr
start_nr --cert $L/cert.pem --key $L/key.pem
run_wrk "netRector TLS keep-alive, 1000 conn"  https://127.0.0.1:8080/ -c1000
run_wrk "netRector TLS Connection: close, 64 conn (sync key)" https://127.0.0.1:8080/ -c64 -H "Connection: close"
stop_nr
start_nr --cert $L/cert.pem --key $L/key.pem --tls-threads $W
run_wrk "netRector TLS Connection: close, 64 conn (--tls-threads $W)" https://127.0.0.1:8080/ -c64 -H "Connection: close"
stop_nr
start_nr --single-accept
run_wrk "netRector Connection: close, 256 conn (--single-accept)" http://127.0.0.1:8080/ -c256 -H "Connection: close"
stop_nr

# responsiveness: keep-alive latency on 64 plain connections while a TLS handshake storm hits the same server
storm() { taskset -c $WRK_CPUS wrk -t2 -c64 -d$((D+2))s --timeout 10s -H "Connection: close" https://127.0.0.1:8443/ >/dev/null 2>&1 & STORM=$!; sleep 1; }
start_nr --cert $L/cert.pem --key $L/key.pem --tls-listen 0.0.0.0:8443
run_wrk "netRector keep-alive 64 conn, no storm (baseline p99)" http://127.0.0.1:8080/ -c64 -t2 --latency
storm; run_wrk "netRector keep-alive 64 conn during TLS storm (sync key)" http://127.0.0.1:8080/ -c64 -t2 --latency; wait $STORM; stop_nr
start_nr --cert $L/cert.pem --key $L/key.pem --tls-listen 0.0.0.0:8443 --tls-threads $W
storm; run_wrk "netRector keep-alive 64 conn during TLS storm (--tls-threads $W)" http://127.0.0.1:8080/ -c64 -t2 --latency; wait $STORM; stop_nr
