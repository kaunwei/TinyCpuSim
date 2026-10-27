#!/usr/bin/env bash
# ==============================================================================
# Safe Queue Rescheduling Helper (Atomic tasks.txt lock)
# Used by Planner AI or Operator to safely rewrite/reorder tasks.txt
# while worker.sh is running in the background.
# ==============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ "$(basename "$SCRIPT_DIR")" = "scripts" ]; then
  NIGHTSHIFT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
else
  NIGHTSHIFT_DIR="$SCRIPT_DIR"
fi

TASKS_FILE="${TASKS_FILE:-$NIGHTSHIFT_DIR/tasks.txt}"
LOCK_DIR="${TASKS_FILE}.lock"
LOCK_MAX_RETRIES=50
LOCK_RETRY=0

# Acquire atomic lock
while ! mkdir "$LOCK_DIR" 2>/dev/null; do
  sleep 0.1
  LOCK_RETRY=$(( LOCK_RETRY + 1 ))
  if [ $LOCK_RETRY -ge $LOCK_MAX_RETRIES ]; then
    # Break stale lock if needed
    rmdir "$LOCK_DIR" 2>/dev/null || true
  fi
done

TEMP_FILE=$(mktemp)
if [ $# -ge 1 ] && [ -f "$1" ]; then
  cat "$1" > "$TEMP_FILE"
else
  cat - > "$TEMP_FILE"
fi

mv "$TEMP_FILE" "$TASKS_FILE"
rmdir "$LOCK_DIR" 2>/dev/null || true

echo "[QUEUE] $TASKS_FILE successfully and atomically rescheduled."
