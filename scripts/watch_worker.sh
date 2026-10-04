#!/usr/bin/env bash
# ==============================================================================
# Watch Worker Sentinel Helper (Zero-Token Blocking Waiter)
# Used by Watcher Subagent to monitor Terminal B background worker.sh
# ==============================================================================

set -uo pipefail

TASKS_FILE="${TASKS_FILE:-tasks.txt}"
STATUS_FILE="${STATUS_FILE:-worker.status}"
PROGRESS_FILE="${PROGRESS_FILE:-progress.log}"
MAX_WAIT_SECONDS="${1:-3600}"
POLL_INTERVAL="${2:-5}"

START_TIME=$(date +%s)

echo "[SENTINEL] Watching background worker (Timeout: ${MAX_WAIT_SECONDS}s, Interval: ${POLL_INTERVAL}s)..."

while true; do
  NOW=$(date +%s)
  ELAPSED=$(( NOW - START_TIME ))

  if [ "$ELAPSED" -ge "$MAX_WAIT_SECONDS" ]; then
    echo "[SENTINEL_TIMEOUT] Exceeded ${MAX_WAIT_SECONDS}s waiting for worker."
    exit 2
  fi

  # Check remaining tasks in tasks.txt safely
  REMAINING_TASKS=0
  if [ -f "$TASKS_FILE" ]; then
    RAW_COUNT=$(grep -v '^[[:space:]]*#' "$TASKS_FILE" 2>/dev/null | grep -v '^[[:space:]]*$' | wc -l) || true
    REMAINING_TASKS=$(echo "$RAW_COUNT" | tr -dc '0-9')
  fi
  if [ -z "$REMAINING_TASKS" ]; then
    REMAINING_TASKS=0
  fi
  
  STATE="UNKNOWN"
  if [ -f "$STATUS_FILE" ]; then
    STATE=$(grep -o '"state":[[:space:]]*"[^"]*"' "$STATUS_FILE" 2>/dev/null | cut -d'"' -f4 || true)
    [ -z "$STATE" ] && STATE="UNKNOWN"
  fi

  # Check for terminal/blocking states
  if [ "$STATE" = "BLOCKED" ] || [ "$STATE" = "AWAITING_GUIDANCE" ]; then
    echo "[SENTINEL_BLOCKED] Worker entered state: $STATE after ${ELAPSED}s."
    exit 1
  fi

  # Check for completion (queue is empty and worker is in IDLE or STOPPED state)
  if [ "$REMAINING_TASKS" -eq 0 ] && { [ "$STATE" = "IDLE" ] || [ "$STATE" = "STOPPED" ]; }; then
    echo "[SENTINEL_COMPLETED] All tasks completed successfully in ${ELAPSED}s."
    exit 0
  fi

  sleep "$POLL_INTERVAL"
done
