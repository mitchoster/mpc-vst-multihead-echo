#!/usr/bin/env bash
# Drive the MPC Live II through the remote addin (jacob-sabella/mpc-addin-remote, MIT) and the
# SSH tunnel, so device behaviour can be checked without touching the unit by hand.
#
# Prereq (one-time, on the device, root):
#   install the addin, then add it to HAKAI's LD_PRELOAD (this port's wrapper drop-in).
# And on this machine, keep the tunnel open:
#   ssh -N -L 6720:127.0.0.1:6720 root@<mpc ip>
#
#   vst/device_test.sh status                 # device_status (OS, CPU, memory, ...)
#   vst/device_test.sh info                    # screen + touch info
#   vst/device_test.sh shot out.png            # whole screen (1280x800)
#   vst/device_test.sh plugin out.png          # the plugin area only (1280x628 at y=110)
#   vst/device_test.sh tap X Y [hold_ms]       # touch and release
#   vst/device_test.sh drag X1 Y1 X2 Y2        # slide (knobs/faders)
#   vst/device_test.sh scroll X Y down|up [px] # fling a list (plugin browser, sample lists)
#   vst/device_test.sh call <tool> '<json>'    # any MCP tool (see `tools`)
#   vst/device_test.sh tools                   # list MCP tools
#
# The addin listens on the device only; MCP is plain JSON-RPC over HTTP (POST /mcp), no session.
set -euo pipefail
DEVICE="${DEVICE:?set DEVICE to the MPC IP address}"
URL="${ADDIN_URL:-http://127.0.0.1:6720}"

need_tunnel() {
  curl -s -m 5 "$URL/info" >/dev/null 2>&1 || {
    echo "no addin at $URL — open the tunnel:  ssh -N -L 6720:127.0.0.1:6720 root@$DEVICE" >&2; exit 2; }
}
mcp() {  # <tool> <json-args>
  curl -s -m 60 -X POST "$URL/mcp" \
    -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
    -d "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"$1\",\"arguments\":$2}}"
}
# pull the first text or image content item out of a tools/call result
mcp_parse() {  # <out.png|->  (reads JSON on stdin)
  python3 -c '
import sys, json, base64
out = sys.argv[1]
d = json.load(sys.stdin)
if "error" in d:
    print("MCP error:", json.dumps(d["error"])[:300]); sys.exit(1)
for c in d.get("result", {}).get("content", []):
    if c.get("type") == "image":
        data = base64.b64decode(c["data"])
        if out != "-":
            open(out, "wb").write(data); print("wrote", out, len(data), "bytes")
        else:
            print("<image>", len(data), "bytes")
    elif c.get("type") == "text":
        print(c.get("text", ""))
' "$1"
}

cmd="${1:-}"; shift || true
case "$cmd" in
  tools)  curl -s -m 10 -X POST "$URL/mcp" -H 'Content-Type: application/json' \
            -H 'Accept: application/json, text/event-stream' \
            -d '{"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}' | python3 -c 'import sys,json;print("\n".join(t["name"] for t in json.load(sys.stdin)["result"]["tools"]))' ;;
  status) need_tunnel; mcp device_status '{}' | mcp_parse - ;;
  info)   need_tunnel; curl -s -m 8 "$URL/info"; echo ;;
  shot)   need_tunnel; mcp screenshot '{"x":0,"y":0,"width":1280,"height":800,"zoom":1}' | mcp_parse "${1:-/tmp/device_screen.png}" ;;
  plugin) need_tunnel; mcp screenshot '{"x":0,"y":110,"width":1280,"height":628,"zoom":1}' | mcp_parse "${1:-/tmp/device_plugin.png}" ;;
  tap)    need_tunnel; mcp tap "{\"x\":$1,\"y\":$2${3:+,\"hold_ms\":$3}}" | mcp_parse - ;;
  # The addin's drag takes flat from_x/from_y/to_x/to_y — the nested {"from":{…}} form it
  # used before was rejected silently, so every drag looked like a tap (2026-11-01).
  drag)   need_tunnel; mcp drag "{\"from_x\":$1,\"from_y\":$2,\"to_x\":$3,\"to_y\":$4}" | mcp_parse - ;;
  scroll) need_tunnel; mcp scroll "{\"x\":$1,\"y\":$2,\"direction\":\"$3\",\"amount\":${4:-400}}" | mcp_parse - ;;
  call)   need_tunnel
          args="${2:-}"; [ -n "$args" ] || args='{}'   # ${2:-{}} alone appends a stray '}'
          mcp "$1" "$args" | mcp_parse - ;;
  *) sed -n '2,22p' "$0" ;;
esac
