#!/usr/bin/env bash
set -e

case "$(uname -s)" in
	Darwin) export DYLD_LIBRARY_PATH=./lib:${DYLD_LIBRARY_PATH} ;;
	*)      export LD_LIBRARY_PATH=./lib:${LD_LIBRARY_PATH} ;;
esac

# Serve the in-tree browser assets instead of $(PREFIX)/share/axil/htdocs so the
# asset routes can be exercised from a source checkout.
AXIL_HTDOCS=./htdocs
export AXIL_HTDOCS

port=$((20000 + RANDOM % 8000))
tmpout=$(mktemp)
trap 'rm -f "$tmpout"; kill "$mux_pid" 2>/dev/null; wait "$mux_pid" 2>/dev/null || true' EXIT

axil -d -A -p "$port" -m ./lib/axil-tty >/dev/null 2>&1 &
mux_pid=$!

# WS key and expected accept
key=$(head -c 16 /dev/urandom | base64 | tr -d '\n')
accept=$(printf '%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11' "$key" \
	| openssl dgst -sha1 -binary | base64)

# Emit the upgrade request on stdout. This must be a function, never a variable:
# $(...) strips trailing newlines, which would eat the final CRLF and leave the
# request unterminated, so the server would never answer.
ws_request() {
	printf 'GET /tty HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: %s\r\n\r\n' \
		"$port" "$key"
}

# Poll until server responds with 101 (proves port open AND module loaded)
tries=50
resp=""
while [ $tries -gt 0 ]; do
	resp=$(ws_request | nc -w1 127.0.0.1 "$port" 2>/dev/null | head -1 | tr -d '\r')
	[ "$resp" = "HTTP/1.1 101 Switching Protocols" ] && break
	tries=$((tries - 1))
	sleep 0.1
done
[ $tries -eq 0 ] && { echo "FAIL: axil did not become ready" >&2; exit 1; }

# Now open the real connection
exec 3<>/dev/tcp/127.0.0.1/$port
ws_request >&3

# Read HTTP response headers
resp=""
while IFS= read -r -t3 line <&3; do
	line="${line%$'\r'}"
	resp="$resp
$line"
	[ -z "$line" ] && break
done

echo "$resp" | grep -qF "101" \
	|| { echo "FAIL: no 101 in response" >&2; exit 1; }

got_accept=$(echo "$resp" | grep -i "sec-websocket-accept" \
	| sed 's/.*: *//' | tr -d '\r\n ')
[ "$got_accept" = "$accept" ] \
	|| { echo "FAIL: accept mismatch: got '$got_accept' want '$accept'" >&2; exit 1; }

# Collect WS frames for 1.5s
cat <&3 >"$tmpout" &
cat_pid=$!
sleep 0.05
kill $cat_pid 2>/dev/null || true
wait $cat_pid 2>/dev/null || true

hex=$(xxd -p "$tmpout" | tr -d '\n')
echo "$hex" | grep -qiF "fffd1f" || { echo "FAIL: IAC DO NAWS missing"   >&2; exit 1; }
echo "$hex" | grep -qiF "fffb01" || { echo "FAIL: IAC WILL ECHO missing" >&2; exit 1; }
echo "$hex" | grep -qiF "fffc03" || { echo "FAIL: IAC WONT SGA missing"  >&2; exit 1; }

# Spawn the shell the way the browser client does. This module ignores shell
# commands sent over the socket: axil never routes WebSocket frames through
# cmd_parse, so the registered "sh" command is unreachable here. The module
# spawns the login shell on the FIRST NAWS, once it knows the window size
# (src/libaxil-tty.c, on_axil_connect's auto_shell). So the trigger is NAWS,
# not a command.
#
# NAWS = IAC SB TELOPT_NAWS(31) cols_hi cols_lo rows_hi rows_lo IAC SE
#      = ff fa 1f 00 50 00 18 ff f0        (80x24, 9 bytes)
# Frame = 0x82 | (0x80|len) | 4 mask bytes | payload
#   The 0x80 is the MASK bit: axil's ws_read rejects unmasked client frames
#   ("client frame is not masked"). A zero mask key leaves the payload as-is.
printf '\x82\x89\x00\x00\x00\x00\xff\xfa\x1f\x00\x50\x00\x18\xff\xf0' >&3

# Let the login shell fork and initialise before writing to it.
sleep 0.5

printf '\x82\x8f\x00\x00\x00\x00echo AXIL_TEST\n' >&3

# Collect PTY output, polling until the echo shows up rather than guessing.
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
	|| { echo "FAIL: PTY output AXIL_TEST not seen" >&2; exit 1; }

echo "ws-mux ok"

# Browser assets: the module must serve every file index.html references, under
# the names the build actually emits. Skipped when the JS bundle isn't built or
# curl is unavailable.
if [ ! -f ./htdocs/index.html ]; then
	echo "assets skipped (no ./htdocs — run 'bun run build' first)"
	exit 0
fi
if ! command -v curl >/dev/null 2>&1; then
	echo "assets skipped (no curl)"
	exit 0
fi

for asset in axil-tty.js axil-tty.css demo.js; do
	body=$(curl -sS --max-time 5 "http://127.0.0.1:$port/$asset" 2>/dev/null || true)
	[ -n "$body" ] \
		|| { echo "FAIL: GET /$asset returned nothing" >&2; exit 1; }
	echo "asset /$asset ok"
done

# The page bootstrap must not 404
curl -sS --max-time 5 "http://127.0.0.1:$port/demo.js" 2>/dev/null \
	| grep -q "ttyAxil" \
	|| { echo "FAIL: /demo.js is not the page bootstrap" >&2; exit 1; }

# index.html must point at the built filenames, not the old axil.* names
page=$(curl -sS --max-time 5 "http://127.0.0.1:$port/tty" 2>/dev/null || true)
for want in ./axil-tty.js ./demo.js ./axil-tty.css; do
	echo "$page" | grep -qF "$want" \
		|| { echo "FAIL: index.html does not reference $want" >&2; exit 1; }
done
echo "$page" | grep -qF "./axil.js" \
	&& { echo "FAIL: index.html still references the old ./axil.js" >&2; exit 1; }
echo "$page" | grep -qF "./axil.css" \
	&& { echo "FAIL: index.html still references the old ./axil.css" >&2; exit 1; }

echo "assets ok"
