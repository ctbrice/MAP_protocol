#!/usr/bin/env bash
set -euo pipefail

PROJECT_DIR=/home/010/c/ct/ctb220006/cs6378

if [[ ! -f "$PROJECT_DIR/Makefile" ]]; then
	echo "No Makefile found in PROJECT_DIR=$PROJECT_DIR" >&2
	echo "Expected project Makefile in $PROJECT_DIR" >&2
	exit 1
fi

make -C "$PROJECT_DIR"
echo "Build complete: $PROJECT_DIR/map-node"