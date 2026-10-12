#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR=/home/010/c/ct/ctb220006/cs6378
NETID=ctb220006
HOST_SUFFIX=utdallas.edu

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 /home/010/c/ct/ctb220006/cs6378/<config-file>" >&2
    exit 2
fi
CONFIG=$(realpath -- "$1")

mapfile -t NODE_ROWS < <(awk '
    {
        sub(/\r$/, "")
        sub(/#.*/, "")
        sub(/^[[:space:]]+/, "")
        sub(/[[:space:]]+$/, "")
        if ($0 ~ /^[0-9]+([[:space:]]|$)/) print $0
    }
' "$CONFIG")
if (( ${#NODE_ROWS[@]} < 1 )); then
    echo "Config has no valid global parameter line: $CONFIG" >&2
    exit 1
fi
read -r NODE_COUNT _ <<< "${NODE_ROWS[0]}"
if [[ ! "$NODE_COUNT" =~ ^[0-9]+$ ]] || (( NODE_COUNT < 1 )) || (( ${#NODE_ROWS[@]} < NODE_COUNT + 1 )); then
    echo "Config does not contain a global line and $NODE_COUNT node rows." >&2
    exit 1
fi

shell_quote() {
    printf "'%s'" "${1//\'/\'\\\'\'}"
}
PROJECT_Q=$(shell_quote "$PROJECT_DIR")

for ((row = 1; row <= NODE_COUNT; row++)); do
    read -r NODE_ID HOST _ <<< "${NODE_ROWS[$row]}"
    [[ "$HOST" == *.* || -z "$HOST_SUFFIX" ]] || HOST="$HOST.$HOST_SUFFIX"
    PIDFILE_Q=$(shell_quote "$PROJECT_DIR/.map-node-$NODE_ID.pid")
    PROCESS_MATCH_Q=$(shell_quote "$PROJECT_DIR/map-node $CONFIG $NODE_ID")
    REMOTE_COMMAND="cd $PROJECT_Q && if [ -f $PIDFILE_Q ]; then pid=\$(cat $PIDFILE_Q); if kill -0 \"\$pid\" 2>/dev/null && ps -p \"\$pid\" -o args= | grep -F -- $PROCESS_MATCH_Q >/dev/null; then kill \"\$pid\" && echo 'stopped node $NODE_ID'; else echo 'node $NODE_ID is not running this project command'; fi; rm -f $PIDFILE_Q; else echo 'no pid file for node $NODE_ID'; fi"
    printf 'Stopping node %s on %s\n' "$NODE_ID" "$HOST"
    ssh -n "$NETID@$HOST" "$REMOTE_COMMAND"
done

echo "Cleanup complete."
