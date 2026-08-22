#!/usr/bin/env bash
# Run the proxy in the foreground. It runs on this machine, not on the OpenClaw
# host: the board can reach this Mac directly on the LAN, which removes the
# port forwarding the old arrangement needed, and keeps our code out of
# someone else's service directory.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/beebo-proxy"
[ -f .env ] || { echo "no .env - copy .env.example and fill it in"; exit 1; }
[ -d node_modules ] || npm install
exec node server.js
