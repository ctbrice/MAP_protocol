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

if [[ ! -x "$PROJECT_DIR/map-node" ]]; then
    echo "Executable not found: $PROJECT_DIR/map-node (run build.sh first)" >&2
    exit 1
fi
if [[ ! -r "$CONFIG" ]]; then
    echo "Cannot read config: $CONFIG" >&2
    exit 1
fi

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
CONFIG_Q=$(shell_quote "$CONFIG")
printf 'Launching %d nodes using %s\n' "$NODE_COUNT" "$CONFIG"

for ((row = 1; row <= NODE_COUNT; row++)); do
    read -r NODE_ID HOST _ <<< "${NODE_ROWS[$row]}"
    if [[ ! "$NODE_ID" =~ ^[0-9]+$ || ! "$HOST" ]]; then
        echo "Invalid node row: ${NODE_ROWS[$row]}" >&2
        exit 1
    fi
    if [[ "$HOST" != *.* && -n "$HOST_SUFFIX" ]]; then
        HOST="$HOST.$HOST_SUFFIX"
    fi

    PIDFILE_Q=$(shell_quote "$PROJECT_DIR/.map-node-$NODE_ID.pid")
    LOG_Q=$(shell_quote "$PROJECT_DIR/$(basename "${CONFIG%.txt}")-$NODE_ID.log")
    NODE_ID_Q=$(shell_quote "$NODE_ID")
    REMOTE_COMMAND="cd $PROJECT_Q && if [ -f $PIDFILE_Q ] && kill -0 \"\$(cat $PIDFILE_Q)\" 2>/dev/null; then echo 'node $NODE_ID already appears to be running'; else nohup $PROJECT_Q/map-node $CONFIG_Q $NODE_ID_Q > $LOG_Q 2>&1 < /dev/null & echo \$! > $PIDFILE_Q; echo 'started node $NODE_ID'; fi"

    printf '  node %s -> %s\n' "$NODE_ID" "$HOST"
    ssh -n "$NETID@$HOST" "$REMOTE_COMMAND"
done

echo "Launcher complete. Node logs are in $PROJECT_DIR."
