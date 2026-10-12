#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR=/home/010/c/ct/ctb220006/cs6378

if [[ ! -d "$PROJECT_DIR" ]]; then
	echo "Project directory does not exist: $PROJECT_DIR" >&2
	exit 1
fi

shopt -s nullglob
FILES=("$PROJECT_DIR"/*.out "$PROJECT_DIR"/*.log "$PROJECT_DIR"/*.o "$PROJECT_DIR"/.map-node-*.pid)
if (( ${#FILES[@]} == 0 )); then
	echo "No output, log, object, or node PID files found in $PROJECT_DIR."
	exit 0
fi

printf 'Removing generated files from %s:\n' "$PROJECT_DIR"
printf '  %s\n' "${FILES[@]}"
rm -f -- "${FILES[@]}"