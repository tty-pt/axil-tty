#!/usr/bin/env bash
# Terminal access gate tests.
#
# The server under test is axil's own test-auth harness with this module loaded
# (-T): test-auth's file-backed cookie auth is the only way to give a connection
# a proven REMOTE_USER without -A, and -A is refused by design, so the module's
# historic backend (axil -A -m) can no longer produce a shell at all.
#
#   1. anonymous /tty ............ refused ("no account"), closed, no PTY
#   2. cookie session for id -un .. shell spawns and echoes (positive control)
#   3. -A server, anonymous /tty . refused ("automatic login"), closed
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
case "$(uname -s)" in
	Darwin) export DYLD_LIBRARY_PATH="$HERE/lib:${DYLD_LIBRARY_PATH:-}" ;;
	*)      export LD_LIBRARY_PATH="$HERE/lib:${LD_LIBRARY_PATH:-}" ;;
esac
for dep in axil libcorm libqsys libxylem; do
	case "$(uname -s)" in
		Darwin) export DYLD_LIBRARY_PATH="$HERE/../$dep/lib:${DYLD_LIBRARY_PATH:-}" ;;
		*)      export LD_LIBRARY_PATH="$HERE/../$dep/lib:${LD_LIBRARY_PATH:-}" ;;
	esac
done

TESTAUTH="$HERE/../axil/bin/test-auth"
TTYSO="$HERE/lib/axil-tty" # xy_load() appends .so itself; never pass the suffix
[ -x "$TESTAUTH" ] || { echo "FAIL: $TESTAUTH missing (build external/axil first)" >&2; exit 1; }
[ -f "$TTYSO.so" ] || { echo "FAIL: tty module missing (build first)" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"; kill "$srv_pid" 2>/dev/null; wait "$srv_pid" 2>/dev/null || true' EXIT
mkdir -p "$WORK/sessions"
SRVLOG="$WORK/server.log"

srv_pid=""
start_server() { # [extra test-auth args...]
	[ -n "$srv_pid" ] && { kill "$srv_pid" 2>/dev/null; wait "$srv_pid" 2>/dev/null || true; srv_pid=""; }
	port=$((20000 + RANDOM % 8000))
	: >"$SRVLOG"
	(cd "$WORK" && "$TESTAUTH" -p "$port" -C "$WORK" -T "$TTYSO" "$@" >"$SRVLOG" 2>&1 &)
	# capture the background pid of the subshell's server: re-find by port
	tries=50
	while [ $tries -gt 0 ]; do
		srv_pid=$(pgrep -f "test-auth -p $port" | head -1 || true)
		[ -n "$srv_pid" ] && break
		tries=$((tries - 1))
		sleep 0.1
	done
	[ -n "$srv_pid" ] || { echo "FAIL: server did not start on $port" >&2; exit 1; }
	# poll until the port answers
	tries=50
	while [ $tries -gt 0 ]; do
		curl -sS --max-time 1 "http://127.0.0.1:$port/" >/dev/null 2>&1 && break
		tries=$((tries - 1))
		sleep 0.1
	done
	if [ $tries -eq 0 ]; then
		echo "FAIL: server silent on $port" >&2
		sed -n '1,20p' "$SRVLOG" >&2
		exit 1
	fi
	return 0
}

# --- ws client bits (borrowed from the old suite) ---
ws_request() { # port key [cookie]
	if [ -n "${3:-}" ]; then
		printf 'GET /tty HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\nCookie: %s\r\n\r\n' \
			"$1" "$2" "$3"
	else
		printf 'GET /tty HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n' \
			"$1" "$2"
	fi
}
new_key() { head -c 16 /dev/urandom | base64 | tr -d '\n'; }
send_text() { # $1 = text on fd 3 (masked frame, zero key)
	msg=$1
	len=$(printf '%s' "$msg" | wc -c)
	{
		printf '\x82'
		printf "\\$(printf '%03o' $((0x80 + len)))"
		printf '\x00\x00\x00\x00%s' "$msg"
	} >&3
}
send_naws() { # 80x24 NAWS subnegotiation as one masked binary frame, fd 3
	printf '\x82\x89\x00\x00\x00\x00\xff\xfa\x1f\x00\x50\x00\x18\xff\xf0' >&3
}

echo "== 1. anonymous /tty is refused with no PTY =="
start_server
key=$(new_key)
tmpcap=$(mktemp)
exec 3<>/dev/tcp/127.0.0.1/$port
ws_request "$port" "$key" >&3
resp=""
while IFS= read -r -t3 line <&3; do
	line="${line%$'\r'}"
	[ -z "$line" ] && break
	resp="$resp $line"
done
echo "$resp" | grep -qF "101" || { echo "FAIL: no 101 in response" >&2; exit 1; }
send_naws
sleep 1
out=$(timeout 3 cat <&3 2>/dev/null || true)
exec 3<&-; exec 3>&- || true
echo "$out" | grep -qF "Terminal disabled: no account." \
	|| { echo "FAIL: refusal text missing (got $(echo "$out" | xxd -p | head -c 120))" >&2; exit 1; }
echo "$out" | grep -qF $'\xff\xfb\x01' \
	&& { echo "FAIL: negotiation bytes present, a PTY was born" >&2; exit 1; }
grep -q "no authenticated passwd identity" "$SRVLOG" \
	|| { echo "FAIL: no refusal log line" >&2; exit 1; }
echo "PASS anonymous refused, no PTY, logged"

echo "== 2. cookie session for a real account gets a shell =="
ME=$(id -un)
SHELL_OF_ME=$(getent passwd "$ME" | cut -d: -f7)
case "$SHELL_OF_ME" in
*/false|*/nologin|"") echo "SKIP positive control ($ME has no login shell)";;
*)
	TOK="ttytest$$"
	printf '%s' "$ME" >"$WORK/sessions/$TOK"
	key=$(new_key)
	tmpout=$(mktemp)
	exec 3<>/dev/tcp/127.0.0.1/$port
	ws_request "$port" "$key" "session=$TOK" >&3
	resp=""
	while IFS= read -r -t3 line <&3; do
		line="${line%$'\r'}"
		[ -z "$line" ] && break
		resp="$resp $line"
	done
	echo "$resp" | grep -qF "101" || { echo "FAIL: no 101 in response" >&2; exit 1; }
	send_naws
	sleep 1
	# the echo probe: a partial line must come straight back from the PTY
	: >"$tmpout"
	cat <&3 >"$tmpout" &
	cat_pid=$!
	printf '\x82\x8f\x00\x00\x00\x00AXIL_ECHO_PROBE' >&3
	tries=20
	while [ $tries -gt 0 ]; do
		grep -qa "AXIL_ECHO_PROBE" "$tmpout" && break
		sleep 0.05
		tries=$((tries - 1))
	done
	kill $cat_pid 2>/dev/null || true
	wait $cat_pid 2>/dev/null || true
	grep -qa "AXIL_ECHO_PROBE" "$tmpout" \
		|| { echo "FAIL: authenticated shell did not echo" >&2; exit 1; }
	# and it runs commands
	printf '\x82\x81\x00\x00\x00\x00\177' >&3
	sleep 0.3
	send_text 'echo AXIL_TEST'
	printf '\x82\x81\x00\x00\x00\x00\x0a' >&3
	: >"$tmpout"
	cat <&3 >"$tmpout" &
	cat_pid=$!
	axil_test_hex=$(printf 'AXIL_TEST' | xxd -p | tr -d '\n')
	tries=40
	while [ $tries -gt 0 ]; do
		hex2=$(xxd -p "$tmpout" | tr -d '\n')
		echo "$hex2" | grep -qiF "$axil_test_hex" && break
		sleep 0.1
		tries=$((tries - 1))
	done
	kill $cat_pid 2>/dev/null || true
	wait $cat_pid 2>/dev/null || true
	hex2=$(xxd -p "$tmpout" | tr -d '\n')
	echo "$hex2" | grep -qiF "$axil_test_hex" \
		|| { echo "FAIL: authenticated shell ran nothing" >&2; exit 1; }
	exec 3<&-; exec 3>&- || true
	rm -f "$tmpout" "$WORK/sessions/$TOK"
	echo "PASS cookie identity spawns a working shell"
	;;
esac

echo "== 3. -A server refuses with the automatic-login message =="
start_server -A
key=$(new_key)
exec 3<>/dev/tcp/127.0.0.1/$port
ws_request "$port" "$key" >&3
resp=""
while IFS= read -r -t3 line <&3; do
	line="${line%$'\r'}"
	[ -z "$line" ] && break
	resp="$resp $line"
done
echo "$resp" | grep -qF "101" || { echo "FAIL: no 101 in response" >&2; exit 1; }
send_naws
sleep 1
out=$(timeout 3 cat <&3 2>/dev/null || true)
exec 3<&-; exec 3>&- || true
echo "$out" | grep -qF "Terminal disabled: automatic login." \
	|| { echo "FAIL: -A refusal text missing" >&2; exit 1; }
grep -q "automatic (-A) identity" "$SRVLOG" \
	|| { echo "FAIL: no -A refusal log line" >&2; exit 1; }
echo "PASS -A refused with its own message, logged"

echo "== 4. assets =="
if [ ! -f ./htdocs/index.html ]; then
	echo "assets skipped (no ./htdocs)"
	exit 0
fi
if ! command -v curl >/dev/null 2>&1; then
	echo "assets skipped (no curl)"
	exit 0
fi
for asset in axil-tty.js axil-tty.css demo.js; do
	body=$(curl -sS --max-time 5 "http://127.0.0.1:$port/$asset" 2>/dev/null || true)
	[ -n "$body" ] || { echo "FAIL: GET /$asset returned nothing" >&2; exit 1; }
	echo "asset /$asset ok"
done
page=$(curl -sS --max-time 5 "http://127.0.0.1:$port/tty" 2>/dev/null || true)
for want in ./axil-tty.js ./demo.js ./axil-tty.css; do
	echo "$page" | grep -qF "$want" \
		|| { echo "FAIL: index.html does not reference $want" >&2; exit 1; }
done
echo "assets ok"
echo "ws-mux ok"
