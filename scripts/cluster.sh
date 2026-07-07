#!/usr/bin/env bash
# Boot a local 3-node restos-ledger cluster.  Usage: scripts/cluster.sh [build-dir]
set -euo pipefail
BUILD="${1:-build}"
NODE="$BUILD/restos-node"; [ -x "$NODE.exe" ] && NODE="$NODE.exe"
DATA="$(mktemp -d)"
trap 'kill $(jobs -p) 2>/dev/null; rm -rf "$DATA"' EXIT

"$NODE" --id 0 --port 5000 --peers 1:127.0.0.1:5001,2:127.0.0.1:5002 --data "$DATA" &
"$NODE" --id 1 --port 5001 --peers 0:127.0.0.1:5000,2:127.0.0.1:5002 --data "$DATA" &
"$NODE" --id 2 --port 5002 --peers 0:127.0.0.1:5000,1:127.0.0.1:5001 --data "$DATA" &

echo "3-node cluster up on ports 5000-5002 (data: $DATA)."
echo "Try:  $BUILD/restos-cli --nodes 127.0.0.1:5000,127.0.0.1:5001,127.0.0.1:5002 put a hello"
echo "Ctrl-C to stop."
wait
