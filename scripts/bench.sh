#!/usr/bin/env bash
# wrk + optional perf bench for tiny, compared against nginx and/or apache.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TOOLS="$ROOT/tools"
WRK="${WRK:-}"
DURATION="${DURATION:-5s}"
THREADS="${THREADS:-4}"
HOST=127.0.0.1
PERF=0
PEER=both
TINY_BIN="${TINY_BIN:-$ROOT/server}"
CASES_FILTER=()

usage() {
  cat <<EOF
Usage: $(basename "$0") [options]

Options:
  --bin PATH       tiny server binary (default: ./server)
  --duration T     wrk -d (default: 5s)
  --threads N      wrk -t (default: 4)
  --peer NAME      nginx|apache|both|none (default: both = nginx+apache)
  --perf           wrap tiny in 'perf record -g' -> ./perf.data
  --case NAME      run only named case(s); repeatable
  -h, --help

Cases: small, small_close, medium, large, not_found, high_c
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bin) TINY_BIN="$2"; shift 2 ;;
    --duration) DURATION="$2"; shift 2 ;;
    --threads) THREADS="$2"; shift 2 ;;
    --peer) PEER="$2"; shift 2 ;;
    --perf) PERF=1; shift ;;
    --case) CASES_FILTER+=("$2"); shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage; exit 1 ;;
  esac
done

pick_port() {
  python3 -c 'import socket;s=socket.socket();s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1);s.bind(("127.0.0.1",0));print(s.getsockname()[1]);s.close()'
}

wait_port() {
  local port="$1" timeout="${2:-8}"
  python3 - "$HOST" "$port" "$timeout" <<'PY'
import socket, sys, time
host, port, timeout = sys.argv[1], int(sys.argv[2]), float(sys.argv[3])
t0 = time.time()
while time.time() - t0 < timeout:
    try:
        s = socket.create_connection((host, port), 0.2)
        s.close()
        sys.exit(0)
    except OSError:
        time.sleep(0.05)
sys.exit(1)
PY
}

ensure_wrk() {
  if [[ -n "$WRK" && -x "$WRK" ]]; then
    return 0
  fi
  if command -v wrk >/dev/null 2>&1; then
    WRK="$(command -v wrk)"
    return 0
  fi
  if [[ -x "$TOOLS/wrk" ]]; then
    WRK="$TOOLS/wrk"
    return 0
  fi
  if [[ -x /tmp/wrk-4.2.0/wrk ]]; then
    mkdir -p "$TOOLS"
    cp -f /tmp/wrk-4.2.0/wrk "$TOOLS/wrk"
    WRK="$TOOLS/wrk"
    return 0
  fi

  echo "building wrk into $TOOLS/wrk (one-time)..." >&2
  mkdir -p "$TOOLS/src"
  local src="$TOOLS/src/wrk"
  rm -rf "$src" "$TOOLS/src/wrk.zip" "$TOOLS/src/wrk-4.2.0"
  curl -fsSL -L -o "$TOOLS/src/wrk.zip" \
    "https://codeload.github.com/wg/wrk/zip/refs/tags/4.2.0"
  unzip -q "$TOOLS/src/wrk.zip" -d "$TOOLS/src"
  mv "$TOOLS/src/wrk-4.2.0" "$src"
  make -C "$src" -j"$(nproc)"
  cp -f "$src/wrk" "$TOOLS/wrk"
  WRK="$TOOLS/wrk"
}

find_nginx_bin() {
  if command -v nginx >/dev/null 2>&1; then
    command -v nginx
    return 0
  fi
  if [[ -x "$TOOLS/nginx/sbin/nginx" ]]; then
    echo "$TOOLS/nginx/sbin/nginx"
    return 0
  fi
  if [[ -x /tmp/nginx-native/sbin/nginx ]]; then
    mkdir -p "$TOOLS"
    if [[ ! -x "$TOOLS/nginx/sbin/nginx" ]]; then
      echo "seeding tools/nginx from /tmp/nginx-native..." >&2
      rm -rf "$TOOLS/nginx"
      cp -a /tmp/nginx-native "$TOOLS/nginx"
    fi
    echo "$TOOLS/nginx/sbin/nginx"
    return 0
  fi
  return 1
}

find_apache_bin() {
  if command -v apache2 >/dev/null 2>&1; then
    command -v apache2
    return 0
  fi
  if command -v httpd >/dev/null 2>&1; then
    command -v httpd
    return 0
  fi
  return 1
}

DOCROOT=""
TINY_PID=""
APACHE_PID=""
DOCKER_NAME=""
PERF_PID=""
NGINX_PREFIX=""
RESULTS=()

cleanup() {
  set +e
  [[ -n "${TINY_PID:-}" ]] && kill "$TINY_PID" 2>/dev/null
  [[ -n "${PERF_PID:-}" ]] && kill "$PERF_PID" 2>/dev/null
  [[ -n "${APACHE_PID:-}" ]] && kill "$APACHE_PID" 2>/dev/null
  if [[ -n "${DOCKER_NAME:-}" ]]; then
    docker rm -f "$DOCKER_NAME" >/dev/null 2>&1
  fi
  if [[ -n "${NGINX_PREFIX:-}" && -x "${NGINX_PREFIX}/sbin/nginx" ]]; then
    "${NGINX_PREFIX}/sbin/nginx" -p "${NGINX_PREFIX}/" -s stop >/dev/null 2>&1
  elif [[ -n "${NGINX_PREFIX:-}" && -f "${NGINX_PREFIX}/logs/nginx.pid" ]]; then
    kill "$(cat "${NGINX_PREFIX}/logs/nginx.pid")" 2>/dev/null
  fi
  [[ -n "${DOCROOT:-}" && -d "$DOCROOT" ]] && rm -rf "$DOCROOT"
}
trap cleanup EXIT

want_case() {
  local name="$1"
  if [[ ${#CASES_FILTER[@]} -eq 0 ]]; then
    return 0
  fi
  local c
  for c in "${CASES_FILTER[@]}"; do
    [[ "$c" == "$name" ]] && return 0
  done
  return 1
}

parse_wrk() {
  local out="$1"
  W_RPS=$(echo "$out" | awk '/Requests\/sec:/ {print $2; exit}')
  W_XFER=$(echo "$out" | awk '/Transfer\/sec:/ {
    u=$2; if (NF>=3 && $3 !~ /^Socket/) u=u$3; print u; exit
  }')
  W_P50=$(echo "$out" | awk '/^[[:space:]]*50%/ {print $2; exit}')
  W_P99=$(echo "$out" | awk '/^[[:space:]]*99%/ {print $2; exit}')
  W_ERR=$(echo "$out" | awk '/Socket errors:/ {
    for (i=1;i<=NF;i++) if ($i=="timeout") { print $(i+1); exit }
  }')
  W_RPS=${W_RPS:-0}
  W_XFER=${W_XFER:-0}
  W_P50=${W_P50:--}
  W_P99=${W_P99:--}
  W_ERR=${W_ERR:-0}
}

run_wrk() {
  local label="$1" url="$2" conns="$3"
  shift 3
  local out
  out=$("$WRK" -t"$THREADS" -c"$conns" -d"$DURATION" --latency "$@" "$url" 2>&1) || true
  parse_wrk "$out"
  printf "  %-12s %-8s  rps=%10s  xfer=%12s  p50=%8s  p99=%8s  to=%s\n" \
    "$label" "$SERVER_LABEL" "$W_RPS" "$W_XFER" "$W_P50" "$W_P99" "$W_ERR"
  RESULTS+=("${label}|${SERVER_LABEL}|${W_RPS}|${W_XFER}|${W_P50}|${W_P99}|${W_ERR}")
}

run_cases_against() {
  local base="$1"
  SERVER_LABEL="$2"

  if want_case small; then
    run_wrk small "$base/" 64
  fi
  if want_case small_close; then
    run_wrk small_close "$base/" 64 -H "Connection: close"
  fi
  if want_case medium; then
    run_wrk medium "$base/med.bin" 32
  fi
  if want_case large; then
    run_wrk large "$base/large.bin" 16
  fi
  if want_case not_found; then
    run_wrk not_found "$base/no/such/file" 64
  fi
  if want_case high_c; then
    run_wrk high_c "$base/" 256
  fi
}

start_tiny() {
  local port="$1"
  local cmd=("$TINY_BIN" "$DOCROOT" "$port")
  if [[ "$PERF" -eq 1 ]]; then
    if ! command -v perf >/dev/null 2>&1; then
      echo "perf not found; install linux-tools" >&2
      exit 1
    fi
    perf record -g -o "$ROOT/perf.data" -- "${cmd[@]}" >"$DOCROOT/tiny.log" 2>&1 &
    PERF_PID=$!
    sleep 0.5
    TINY_PID=$(pgrep -P "$PERF_PID" 2>/dev/null | head -1 || true)
    [[ -z "$TINY_PID" ]] && TINY_PID=$PERF_PID
  else
    "${cmd[@]}" >"$DOCROOT/tiny.log" 2>&1 &
    TINY_PID=$!
  fi
  if ! wait_port "$port"; then
    echo "tiny failed to listen on $port" >&2
    cat "$DOCROOT/tiny.log" >&2 || true
    exit 1
  fi
}

start_nginx_local() {
  local port="$1"
  local bin="$2"
  local prefix="$DOCROOT/nginx-run"
  mkdir -p "$prefix"/{logs,conf,sbin}
  # Prefer invoking the real binary with an isolated -p prefix.
  ln -sfn "$bin" "$prefix/sbin/nginx"
  NGINX_PREFIX="$prefix"
  cat >"$prefix/conf/nginx.conf" <<EOF
worker_processes auto;
error_log $prefix/logs/error.log warn;
pid $prefix/logs/nginx.pid;
events { worker_connections 4096; }
http {
  access_log off;
  sendfile on;
  tcp_nopush on;
  keepalive_timeout 65;
  types { text/html html; application/octet-stream bin; }
  default_type application/octet-stream;
  server {
    listen $port;
    root $DOCROOT;
  }
}
EOF
  "$bin" -p "$prefix/" -c conf/nginx.conf
  if ! wait_port "$port"; then
    echo "nginx failed to listen on $port" >&2
    cat "$prefix/logs/error.log" >&2 || true
    return 1
  fi
  return 0
}

start_nginx_docker() {
  local port="$1"
  DOCKER_NAME="tiny-bench-nginx-$$"
  docker rm -f "$DOCKER_NAME" >/dev/null 2>&1 || true
  mkdir -p "$DOCROOT/nginx-docker"
  cat >"$DOCROOT/nginx-docker/nginx.conf" <<EOF
worker_processes auto;
error_log /dev/stderr warn;
pid /tmp/nginx.pid;
events { worker_connections 4096; }
http {
  access_log off;
  sendfile on;
  tcp_nopush on;
  keepalive_timeout 65;
  types { text/html html; application/octet-stream bin; }
  default_type application/octet-stream;
  server {
    listen 80;
    root /usr/share/nginx/html;
  }
}
EOF
  docker run -d --name "$DOCKER_NAME" \
    -p "$port:80" \
    -v "$DOCROOT:/usr/share/nginx/html:ro" \
    -v "$DOCROOT/nginx-docker/nginx.conf:/etc/nginx/nginx.conf:ro" \
    nginx:alpine >/dev/null
  if ! wait_port "$port" 15; then
    echo "docker nginx failed on $port" >&2
    docker logs "$DOCKER_NAME" >&2 || true
    return 1
  fi
  return 0
}

start_apache() {
  local port="$1"
  local bin="$2"
  local confdir="$DOCROOT/apache"
  mkdir -p "$confdir"/{logs,run}
  local mods=""
  local d
  for d in /usr/lib/apache2/modules /usr/lib64/httpd/modules /usr/lib/httpd/modules; do
    if [[ -d "$d" ]]; then
      mods="$d"
      break
    fi
  done
  if [[ -z "$mods" ]]; then
    echo "apache modules dir not found; skipping apache" >&2
    return 1
  fi

  local user group
  user="$(id -un)"
  group="$(id -gn)"

  {
    echo "ServerRoot \"$confdir\""
    echo "DefaultRuntimeDir \"$confdir/run\""
    echo "PidFile \"$confdir/httpd.pid\""
    echo "Mutex file:$confdir/run default"
    echo "Listen $port"
    echo "User $user"
    echo "Group $group"
    echo "ErrorLog \"$confdir/logs/error.log\""
    echo "LogLevel warn"
    echo "ServerName localhost"
    echo "DocumentRoot \"$DOCROOT\""
    echo "DirectoryIndex index.html"
    echo "EnableSendfile On"
    echo "KeepAlive On"
    echo "MaxKeepAliveRequests 10000"
    echo "KeepAliveTimeout 65"
    # Skip modules that are often compiled-in (unixd, etc.).
    for m in mpm_event authz_core mime dir; do
      if [[ -f "$mods/mod_${m}.so" ]]; then
        echo "LoadModule ${m}_module $mods/mod_${m}.so"
      fi
    done
    [[ -f /etc/mime.types ]] && echo "TypesConfig /etc/mime.types"
    echo "<Directory \"$DOCROOT\">"
    echo "  Require all granted"
    echo "  Options FollowSymLinks"
    echo "  AllowOverride None"
    echo "</Directory>"
  } >"$confdir/httpd.conf"

  "$bin" -f "$confdir/httpd.conf" -DFOREGROUND >"$confdir/logs/boot.log" 2>&1 &
  APACHE_PID=$!
  if ! wait_port "$port" 4; then
    echo "apache failed to start; skipping" >&2
    cat "$confdir/logs/boot.log" >&2 || true
    cat "$confdir/logs/error.log" >&2 || true
    kill "$APACHE_PID" 2>/dev/null || true
    APACHE_PID=""
    return 1
  fi
  return 0
}

print_comparison() {
  echo
  echo "Comparison (Requests/sec)"
  printf "%-12s  %12s  %12s  %12s  %10s  %11s\n" \
    "case" "tiny" "nginx" "apache" "tiny/nginx" "tiny/apache"
  printf "%-12s  %12s  %12s  %12s  %10s  %11s\n" \
    "------------" "------------" "------------" "------------" "----------" "-----------"

  local cases=()
  local line case_name c seen
  for line in "${RESULTS[@]+"${RESULTS[@]}"}"; do
    case_name="${line%%|*}"
    seen=0
    for c in "${cases[@]+"${cases[@]}"}"; do
      [[ "$c" == "$case_name" ]] && { seen=1; break; }
    done
    [[ $seen -eq 0 ]] && cases+=("$case_name")
  done

  for case_name in "${cases[@]+"${cases[@]}"}"; do
    local tiny="-" nginx="-" apache="-"
    for line in "${RESULTS[@]}"; do
      local c srv rps
      c="${line%%|*}"
      rest="${line#*|}"
      srv="${rest%%|*}"
      rest="${rest#*|}"
      rps="${rest%%|*}"
      [[ "$c" != "$case_name" ]] && continue
      case "$srv" in
        tiny) tiny="$rps" ;;
        nginx) nginx="$rps" ;;
        apache) apache="$rps" ;;
      esac
    done
    local ratio_n="-" ratio_a="-"
    if [[ "$tiny" != "-" && "$nginx" != "-" && "$nginx" != "0" ]]; then
      ratio_n=$(python3 -c "print(f'{float(\"$tiny\")/float(\"$nginx\"):.2f}x')")
    fi
    if [[ "$tiny" != "-" && "$apache" != "-" && "$apache" != "0" ]]; then
      ratio_a=$(python3 -c "print(f'{float(\"$tiny\")/float(\"$apache\"):.2f}x')")
    fi
    printf "%-12s  %12s  %12s  %12s  %10s  %11s\n" \
      "$case_name" "$tiny" "$nginx" "$apache" "$ratio_n" "$ratio_a"
  done
}

# --- main ---
ensure_wrk
echo "wrk: $WRK"

DOCROOT="$(mktemp -d /tmp/tiny-bench-XXXXXX)"
echo "<html>bench</html>" >"$DOCROOT/index.html"
mkdir -p "$DOCROOT/subdir"
echo sub >"$DOCROOT/subdir/index.html"
dd if=/dev/urandom of="$DOCROOT/med.bin" bs=64K count=1 status=none
dd if=/dev/urandom of="$DOCROOT/large.bin" bs=1M count=1 status=none

if [[ ! -x "$TINY_BIN" ]]; then
  echo "tiny binary not found: $TINY_BIN (run make server)" >&2
  exit 1
fi

TINY_PORT="$(pick_port)"
if [[ "$PERF" -eq 1 ]]; then
  echo "tiny  -> http://$HOST:$TINY_PORT  bin=$TINY_BIN (perf)"
else
  echo "tiny  -> http://$HOST:$TINY_PORT  bin=$TINY_BIN"
fi
start_tiny "$TINY_PORT"
echo "--------------------------------------------------------------------"
run_cases_against "http://$HOST:$TINY_PORT" tiny

run_nginx=0
run_apache=0
case "$PEER" in
  nginx) run_nginx=1 ;;
  apache) run_apache=1 ;;
  both) run_nginx=1; run_apache=1 ;;
  none) ;;
  *) echo "bad --peer $PEER" >&2; exit 1 ;;
esac

if [[ $run_nginx -eq 1 ]]; then
  echo "--------------------------------------------------------------------"
  if NGINX_BIN=$(find_nginx_bin); then
    NGINX_PORT="$(pick_port)"
    echo "nginx -> http://$HOST:$NGINX_PORT  ($NGINX_BIN)"
    if start_nginx_local "$NGINX_PORT" "$NGINX_BIN"; then
      run_cases_against "http://$HOST:$NGINX_PORT" nginx
    elif command -v docker >/dev/null 2>&1; then
      echo "local nginx failed; trying docker..." >&2
      NGINX_PORT="$(pick_port)"
      if start_nginx_docker "$NGINX_PORT"; then
        echo "nginx -> http://$HOST:$NGINX_PORT  (docker)"
        run_cases_against "http://$HOST:$NGINX_PORT" nginx
      fi
    fi
  elif command -v docker >/dev/null 2>&1; then
    NGINX_PORT="$(pick_port)"
    echo "nginx -> http://$HOST:$NGINX_PORT  (docker)"
    if start_nginx_docker "$NGINX_PORT"; then
      run_cases_against "http://$HOST:$NGINX_PORT" nginx
    fi
  else
    echo "nginx not available; skipping peer" >&2
  fi
fi

if [[ $run_apache -eq 1 ]]; then
  echo "--------------------------------------------------------------------"
  if APACHE_BIN=$(find_apache_bin); then
    APACHE_PORT="$(pick_port)"
    echo "apache -> http://$HOST:$APACHE_PORT  ($APACHE_BIN)"
    if start_apache "$APACHE_PORT" "$APACHE_BIN"; then
      run_cases_against "http://$HOST:$APACHE_PORT" apache
    fi
  else
    echo "apache not available; skipping" >&2
  fi
fi

print_comparison
echo "--------------------------------------------------------------------"
if [[ "$PERF" -eq 1 ]]; then
  echo "perf data: $ROOT/perf.data"
  echo "  perf report -i perf.data"
fi
