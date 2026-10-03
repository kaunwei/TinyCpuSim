#!/usr/bin/env bash
# ==============================================================================
# 24-Hour Unattended Background Worker Daemon
# Dual-Terminal AI Development Framework
# ==============================================================================

set -uo pipefail

# ------------------------------------------------------------------------------
# Configuration (Auto-sources .worker.env if present, otherwise uses defaults)
# ------------------------------------------------------------------------------
if [ -f ".worker.env" ]; then
  # shellcheck source=/dev/null
  source ".worker.env"
fi

TASKS_FILE="${TASKS_FILE:-tasks.txt}"
TASKS_DONE_FILE="${TASKS_DONE_FILE:-tasks.done}"
PROGRESS_FILE="${PROGRESS_FILE:-progress.log}"
STATUS_FILE="${STATUS_FILE:-worker.status}"
LOG_FILE="${LOG_FILE:-.worker.log}"
PID_FILE="${PID_FILE:-.worker.pid}"
PAUSE_FILE="${PAUSE_FILE:-PAUSE}"

AI_CLI="${AI_CLI:-agy}"
AI_MODEL="${AI_MODEL:-gemini-3.7-flash-low}"
AI_EFFORT="${AI_EFFORT:-low}"
TASK_TIMEOUT="${TASK_TIMEOUT:-1800}"
POLL_INTERVAL="${POLL_INTERVAL:-5}"
USE_SANDBOX="${USE_SANDBOX:-0}"
ADDITIONAL_DIRS="${ADDITIONAL_DIRS:-}"

# Force non-interactive, unpaged batch environment
export CI=1
export PAGER=cat
export NO_COLOR=1
export DEBIAN_FRONTEND=noninteractive
export TERM=dumb

# ------------------------------------------------------------------------------
# Process Lock & Signal Handlers
# ------------------------------------------------------------------------------
cleanup() {
  echo ""
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] Shutting down worker daemon..."
  rm -f "$PID_FILE"
  update_status "STOPPED" "" "0" "0" "0" "0" "0%" "[------------------------------] 0%"
  exit 0
}

trap cleanup SIGINT SIGTERM EXIT

if [ -f "$PID_FILE" ]; then
  OLD_PID=$(cat "$PID_FILE" 2>/dev/null || echo "")
  if [ -n "$OLD_PID" ] && kill -0 "$OLD_PID" 2>/dev/null; then
    echo "ERROR: Worker daemon already running with PID $OLD_PID."
    exit 1
  fi
fi
echo "$$" > "$PID_FILE"

# ------------------------------------------------------------------------------
# Helper Functions
# ------------------------------------------------------------------------------
render_progress_bar() {
  local cur="$1"
  local total="$2"
  local width="${3:-30}"
  if [ "$total" -le 0 ]; then
    echo "[------------------------------] 0% (0/0)"
    return
  fi
  local pct=$(( cur * 100 / total ))
  local filled=$(( cur * width / total ))
  local empty=$(( width - filled ))
  local bar=""
  for ((i=0; i<filled; i++)); do bar+="#"; done
  for ((i=0; i<empty; i++)); do bar+="-"; done
  echo "[$bar] ${pct}% (${cur}/${total})"
}

update_status() {
  local state="$1"
  local current_task="${2:-}"
  local elapsed="${3:-0}"
  local current_idx="${4:-0}"
  local total_tasks="${5:-0}"
  local remaining="${6:-0}"
  local percent="${7:-0%}"
  local bar="${8:-}"
  local now
  now=$(date -u +"%Y-%m-%dT%H:%M:%SZ")

  if [ -z "$bar" ]; then
    bar=$(render_progress_bar "$current_idx" "$total_tasks" 30)
  fi

  cat <<EOF > "$STATUS_FILE"
{
  "state": "$state",
  "current_task": $(if [ -n "$current_task" ]; then echo "\"$current_task\""; else echo "null"; fi),
  "elapsed_seconds": $elapsed,
  "progress": {
    "current_index": $current_idx,
    "total_tasks": $total_tasks,
    "remaining": $remaining,
    "percentage": "$percent",
    "bar": "$bar"
  },
  "pid": $$,
  "model": "$AI_MODEL",
  "cli": "$AI_CLI",
  "timeout_seconds": $TASK_TIMEOUT,
  "last_heartbeat": "$now"
}
EOF
}

prepend_progress_log() {
  local entry="$1"
  local temp_file
  temp_file=$(mktemp)
  echo "$entry" > "$temp_file"
  if [ -f "$PROGRESS_FILE" ]; then
    cat "$PROGRESS_FILE" >> "$temp_file"
  fi
  mv "$temp_file" "$PROGRESS_FILE"
}

isolate_and_rollback_dirty_tree() {
  local task_id="$1"
  local backup_dir=".scratch/failed_tasks/${task_id}_$(date +%Y%m%d_%H%M%S)"
  mkdir -p "$backup_dir"
  
  echo "[WORKER] Isolating failed changes to $backup_dir and resetting clean git tree..."
  git diff > "$backup_dir/changes.diff" 2>/dev/null || true
  git status --porcelain > "$backup_dir/status.txt" 2>/dev/null || true
  
  # Safe reset to head
  git restore . 2>/dev/null || true
  git clean -df -e "$TASKS_FILE" -e "$TASKS_DONE_FILE" -e "$PROGRESS_FILE" -e "$STATUS_FILE" -e "$LOG_FILE" -e "$PID_FILE" -e ".scratch" 2>/dev/null || true
}

# ------------------------------------------------------------------------------
# Startup Initialization
# ------------------------------------------------------------------------------
touch "$TASKS_FILE" "$TASKS_DONE_FILE" "$PROGRESS_FILE"
echo "=============================================================================="
echo " 24-Hour Unattended AI Worker Daemon Started"
echo " PID: $$ | CLI: $AI_CLI | Model: $AI_MODEL | Timeout: ${TASK_TIMEOUT}s"
echo " Tasks File: $TASKS_FILE | Progress: $PROGRESS_FILE"
echo "=============================================================================="

update_status "IDLE" "" "0" "0" "0" "0" "0%" "[------------------------------] 0%"

WAS_BUSY=0
IDLE_HEARTBEAT_COUNT=0

# ------------------------------------------------------------------------------
# Main Polling Loop
# ------------------------------------------------------------------------------
while true; do
  # Check for physical pause switch
  if [ -f "$PAUSE_FILE" ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [PAUSED] Found $PAUSE_FILE. Sleeping..."
    update_status "PAUSED" "Paused via $PAUSE_FILE file" "0" "0" "0" "0" "0%" "[PAUSED]"
    sleep "$POLL_INTERVAL"
    continue
  fi

  # Peek at the first non-empty, non-comment line in tasks.txt
  RAW_TASK=$(grep -v '^[[:space:]]*#' "$TASKS_FILE" 2>/dev/null | grep -v '^[[:space:]]*$' | head -n 1 || true)

  DONE_COUNT=$(grep -v '^[[:space:]]*#' "$TASKS_DONE_FILE" 2>/dev/null | grep -v '^[[:space:]]*$' | wc -l)
  REMAINING_COUNT=$(grep -v '^[[:space:]]*#' "$TASKS_FILE" 2>/dev/null | grep -v '^[[:space:]]*$' | wc -l)
  TOTAL_COUNT=$(( DONE_COUNT + REMAINING_COUNT ))

  if [ -z "$RAW_TASK" ]; then
    BAR=$(render_progress_bar "$DONE_COUNT" "$TOTAL_COUNT" 30)
    update_status "IDLE" "" "0" "$DONE_COUNT" "$TOTAL_COUNT" "0" "100%" "$BAR"

    if [ $WAS_BUSY -eq 1 ]; then
      echo ""
      echo "=============================================================================="
      echo "[$(date '+%Y-%m-%d %H:%M:%S')] 🎉 [ALL TASKS COMPLETED] $BAR"
      echo "[IDLE] Queue is empty (Total completed: $DONE_COUNT tasks)."
      echo "[IDLE] Waiting for new tasks in $TASKS_FILE... (Polling every ${POLL_INTERVAL}s)"
      echo "=============================================================================="
      WAS_BUSY=0
      IDLE_HEARTBEAT_COUNT=0
    else
      IDLE_HEARTBEAT_COUNT=$(( IDLE_HEARTBEAT_COUNT + 1 ))
      # Print a heartbeat line every 60 seconds (12 intervals of 5s)
      if [ $(( IDLE_HEARTBEAT_COUNT % 12 )) -eq 0 ]; then
        echo "[$(date '+%Y-%m-%d %H:%M:%S')] [IDLE] Waiting for tasks in $TASKS_FILE... (Done: $DONE_COUNT | Heartbeat OK)"
      fi
    fi

    sleep "$POLL_INTERVAL"
    continue
  fi

  WAS_BUSY=1
  IDLE_HEARTBEAT_COUNT=0

  CURRENT_IDX=$(( DONE_COUNT + 1 ))
  PERCENT_START=$(( (DONE_COUNT * 100) / TOTAL_COUNT ))
  BAR_START=$(render_progress_bar "$DONE_COUNT" "$TOTAL_COUNT" 30)

  # Pop the task line atomically from tasks.txt
  TEMP_TASKS=$(mktemp)
  awk -v task="$RAW_TASK" '
    !found && $0 == task { found=1; next }
    { print }
  ' "$TASKS_FILE" > "$TEMP_TASKS"
  mv "$TEMP_TASKS" "$TASKS_FILE"

  TASK_TITLE="$RAW_TASK"
  TASK_ID=$(echo "$RAW_TASK" | awk -F'|' '{print $1}' | tr -d ' ' || echo "TASK-UNKNOWN")
  START_TIME=$(date +%s)
  
  echo ""
  echo "=============================================================================="
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] >>> [Task $CURRENT_IDX of $TOTAL_COUNT (In Progress) | Completed: $DONE_COUNT/$TOTAL_COUNT ($PERCENT_START%)]"
  echo ">>> Progress: $BAR_START"
  echo ">>> Task: $TASK_TITLE"
  echo "=============================================================================="

  update_status "RUNNING" "$TASK_TITLE" "0" "$CURRENT_IDX" "$TOTAL_COUNT" "$((REMAINING_COUNT - 1))" "${PERCENT_START}%" "$BAR_START"

  # Construct Prompt for the CLI Process
  PROMPT_PAYLOAD=$(cat <<PROMPT_EOF
You are the unattended Background Worker AI executing an atomic task.
Read and strictly follow the mandatory SOP in .skills/universal-build-verify.md and guidelines in AGENTS.md.

TASK TO IMPLEMENT:
$TASK_TITLE

MANDATORY EXECUTION RULES:
1. REAL-TIME STEP LOGGING: Before running any command or editing any file, print an explicit line (e.g. "[EXEC] Modifying <file>...", "[EXEC] Running command: <command>", "[EXEC] Running test: <command>") so the operator can see what you are doing in real time.
2. TARGET ONLY: Modify only the necessary files specified in the task.
3. NO SUDO: Under NO circumstances run sudo, apt, or global system commands.
4. BUILD & VERIFY: Run the appropriate build and test commands (C/C++ cmake/ninja, Python pytest, GUI timeout 2s smoke tests).
5. RETRY LIMIT: Max 3 repair attempts. If unable to pass after 3 fixes, stop immediately and record [BLOCKED].
6. ATOMIC COMMIT: On 100% verification pass, run git commit -m "feat/fix: $TASK_TITLE".
7. TOP PREPEND PROGRESS: Write a structured <=10 lines report at the VERY TOP of $PROGRESS_FILE.
PROMPT_EOF
)

  # Execute CLI with Timeout Guard
  CMD_OUTPUT_TMP=$(mktemp)
  EXIT_CODE=0

  if [ "$AI_CLI" = "agy" ]; then
    EXTRA_CLI_ARGS=()
    if [ "$USE_SANDBOX" = "1" ] || [ "$USE_SANDBOX" = "true" ]; then
      EXTRA_CLI_ARGS+=("--sandbox")
    fi
    if [ -n "$ADDITIONAL_DIRS" ]; then
      IFS=',' read -ra DIRS <<< "$ADDITIONAL_DIRS"
      for d in "${DIRS[@]}"; do
        d_trimmed=$(echo "$d" | xargs)
        if [ -n "$d_trimmed" ] && [ -d "$d_trimmed" ]; then
          EXTRA_CLI_ARGS+=("--add-dir" "$d_trimmed")
        fi
      done
    fi

    echo "[WORKER] Launching $AI_CLI (Model: $AI_MODEL)..."
    timeout "$TASK_TIMEOUT" agy -p "$PROMPT_PAYLOAD" \
      --dangerously-skip-permissions \
      --model "$AI_MODEL" \
      --effort "$AI_EFFORT" \
      "${EXTRA_CLI_ARGS[@]}" 2>&1 | tee "$CMD_OUTPUT_TMP"
    EXIT_CODE="${PIPESTATUS[0]}"
  else
    echo "[WORKER] Launching $AI_CLI..."
    timeout "$TASK_TIMEOUT" "$AI_CLI" "$PROMPT_PAYLOAD" 2>&1 | tee "$CMD_OUTPUT_TMP"
    EXIT_CODE="${PIPESTATUS[0]}"
  fi

  END_TIME=$(date +%s)
  DURATION=$(( END_TIME - START_TIME ))

  # Append run output to .worker.log for auditing
  echo "=== [$(date '+%Y-%m-%d %H:%M:%S')] Task: $TASK_TITLE (Duration: ${DURATION}s, Exit: $EXIT_CODE) ===" >> "$LOG_FILE"
  cat "$CMD_OUTPUT_TMP" >> "$LOG_FILE"
  echo "" >> "$LOG_FILE"

  if [ $EXIT_CODE -eq 124 ]; then
    # Timeout occurred
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [TIMEOUT] Task exceeded ${TASK_TIMEOUT}s limit!"
    isolate_and_rollback_dirty_tree "$TASK_ID"
    
    BLOCKED_SUMMARY=$(cat <<BLOCK_EOF
================================================================================
[BLOCKED] $TASK_ID: Timeout Exceeded (${TASK_TIMEOUT}s)
Time: $(date '+%Y-%m-%d %H:%M:%S') | Status: TIMEOUT
Failure Reason: Task execution timed out after ${TASK_TIMEOUT} seconds.
Action Needed for Human: Task may be too complex. Split into smaller atomic tasks.
================================================================================
BLOCK_EOF
)
    prepend_progress_log "$BLOCKED_SUMMARY"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [FAILED] $TASK_TITLE [TIMEOUT]" >> "$TASKS_DONE_FILE"
    update_status "BLOCKED" "$TASK_TITLE (TIMEOUT)" "$DURATION" "$CURRENT_IDX" "$TOTAL_COUNT" "$((REMAINING_COUNT - 1))" "${PERCENT_START}%" "$BAR_START"

  elif grep -q '\[NEED_GUIDANCE\]\|\[NEED_DECOMPOSITION\]' "$CMD_OUTPUT_TMP" 2>/dev/null; then
    # Worker honestly requested guidance or decomposition
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ✋ [AWAITING GUIDANCE] Worker honestly requested manager guidance."
    isolate_and_rollback_dirty_tree "$TASK_ID"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [GUIDANCE_REQUESTED] $TASK_TITLE" >> "$TASKS_DONE_FILE"
    update_status "AWAITING_GUIDANCE" "$TASK_TITLE" "$DURATION" "$CURRENT_IDX" "$TOTAL_COUNT" "$((REMAINING_COUNT - 1))" "${PERCENT_START}%" "$BAR_START"

  elif [ $EXIT_CODE -ne 0 ]; then
    # Execution failed
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [ERROR] CLI exited with code $EXIT_CODE"
    isolate_and_rollback_dirty_tree "$TASK_ID"
    
    BLOCKED_SUMMARY=$(cat <<BLOCK_EOF
================================================================================
[BLOCKED] $TASK_ID: Execution Failed (Exit Code: $EXIT_CODE)
Time: $(date '+%Y-%m-%d %H:%M:%S') | Status: FAILED
Failure Reason: Worker CLI returned non-zero exit status $EXIT_CODE.
Action Needed for Human: Inspect .worker.log for error details.
================================================================================
BLOCK_EOF
)
    prepend_progress_log "$BLOCKED_SUMMARY"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [FAILED] $TASK_TITLE [EXIT_$EXIT_CODE]" >> "$TASKS_DONE_FILE"
    update_status "BLOCKED" "$TASK_TITLE (ERROR $EXIT_CODE)" "$DURATION" "$CURRENT_IDX" "$TOTAL_COUNT" "$((REMAINING_COUNT - 1))" "${PERCENT_START}%" "$BAR_START"

  else
    # Success
    NEW_DONE_COUNT=$(( DONE_COUNT + 1 ))
    FINAL_BAR=$(render_progress_bar "$NEW_DONE_COUNT" "$TOTAL_COUNT" 30)
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [SUCCESS] Task completed in ${DURATION}s | $FINAL_BAR"
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [DONE] $TASK_TITLE (Duration: ${DURATION}s)" >> "$TASKS_DONE_FILE"
    update_status "IDLE" "" "0" "$NEW_DONE_COUNT" "$TOTAL_COUNT" "$((REMAINING_COUNT - 1))" "$(( (NEW_DONE_COUNT * 100) / TOTAL_COUNT ))%" "$FINAL_BAR"
  fi

  rm -f "$CMD_OUTPUT_TMP"
  sleep "$POLL_INTERVAL"
done
