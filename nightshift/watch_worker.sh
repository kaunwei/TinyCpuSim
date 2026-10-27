#!/usr/bin/env bash
# Wait for a specific worker task to finish and pass its mechanical audit.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ "$(basename "$SCRIPT_DIR")" = "scripts" ]; then
  NIGHTSHIFT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
  PROJECT_ROOT="$NIGHTSHIFT_DIR"
  RUNTIME_PATH="$SCRIPT_DIR/worker_runtime.py"
else
  NIGHTSHIFT_DIR="$SCRIPT_DIR"
  PROJECT_ROOT="$(cd "$NIGHTSHIFT_DIR/.." && pwd)"
  RUNTIME_PATH="$NIGHTSHIFT_DIR/worker_runtime.py"
fi
cd "$PROJECT_ROOT" || exit 1

TASKS_FILE="${TASKS_FILE:-$NIGHTSHIFT_DIR/tasks.txt}"
TASKS_DONE_FILE="${TASKS_DONE_FILE:-$NIGHTSHIFT_DIR/tasks.done}"
STATUS_FILE="${STATUS_FILE:-$NIGHTSHIFT_DIR/worker.status}"
PROGRESS_FILE="${PROGRESS_FILE:-$NIGHTSHIFT_DIR/progress.log}"
if [ "${WORKER_ID:-default}" != "default" ]; then
  STATUS_FILE="$NIGHTSHIFT_DIR/worker.${WORKER_ID}.status"
fi
MAX_WAIT_SECONDS="${1:-0}"
POLL_INTERVAL="${2:-5}"
EXPECTED_TASK_ID="${3:-${WATCH_TASK_ID:-}}"
AGENT_EVENTS_FILE="${WORKER_AGENT_EVENTS_FILE:-$NIGHTSHIFT_DIR/.worker-agent-events.jsonl}"
AGENT_EVENTS_CURSOR_FILE="${WORKER_AGENT_EVENTS_CURSOR_FILE:-$NIGHTSHIFT_DIR/.worker-agent-events.cursor.json}"
AGENT_EVENTS_ACK_FILE="${WORKER_AGENT_EVENTS_ACK_FILE:-$NIGHTSHIFT_DIR/.worker-agent-events.acks.jsonl}"
AGENT_DISPATCH_GATE_FILE="${AGENT_DISPATCH_GATE_FILE:-$NIGHTSHIFT_DIR/.worker-dispatch-gate.json}"

SECONDS=0
START_TIME=$SECONDS
LAST_DONE_BYTES=0
BASELINE_AUDITS=$(mktemp "${TMPDIR:-/tmp}/nightshift-audit-baseline.XXXXXX") || exit 1
trap 'rm -f "$BASELINE_AUDITS"' EXIT
if [ -f "$PROGRESS_FILE" ]; then
  grep -E '^\[MECHANICAL_AUDIT\] Status: PASS \| Task: TASK-' "$PROGRESS_FILE" > "$BASELINE_AUDITS" || true
fi
if [ -f "$TASKS_DONE_FILE" ]; then
  LAST_DONE_BYTES=$(wc -c < "$TASKS_DONE_FILE" | tr -d '[:space:]')
fi
TRACKED_TASKS=""
AUDITED_DONE_TASKS=""
PENDING_DONE_TASKS=""
PENDING_FAILED_TASKS=""
REPLAY_TASKS=""
DONE_AUDIT_KEYS=""

track_task() {
  local task_id="$1"
  [ -n "$task_id" ] || return 0
  case " $TRACKED_TASKS " in
    *" $task_id "*) ;;
    *) TRACKED_TASKS="${TRACKED_TASKS}${TRACKED_TASKS:+ }$task_id" ;;
  esac
}

contains_task() {
  local task_id="$1" list="$2"
  case " $list " in *" $task_id "*) return 0 ;; *) return 1 ;; esac
}

task_ids_from_file() {
  local file="$1"
  [ -f "$file" ] || return 0
  sed -nE 's/^[[:space:]]*(TASK-[[:alnum:]_.-]+).*/\1/p' "$file"
}

# The queue stores kind tags separately; runtime audit identities may attach them.
canonical_task() { printf '%s\n' "$1" | grep -Eo 'TASK-[[:alnum:]_.-]+' | head -n 1; }
EXPECTED_TASK_ID=$(canonical_task "$EXPECTED_TASK_ID")

matching_audits() {
  [ -f "$PROGRESS_FILE" ] || return 0
  awk -F ' [|] ' -v expected="$1" '
    /^\[MECHANICAL_AUDIT\] Status: PASS/ {
      for (i=2; i<=NF; i++) if ($i ~ /^Task: /) {
        task=$i; sub(/^Task: /, "", task); sub(/[[:space:]\[].*$/, "", task)
        if (task == expected) print
      }
    }' "$PROGRESS_FILE"
}

audit_identity() {
  printf '%s\n' "$1" | sed -E 's/ \| Planner_Review:.*$//'
}

audit_ns_of() {
  printf '%s\n' "$1" | grep -Eo 'Audit_NS: [0-9]+' | awk '{print $2}'
}

remember_done_key() {
  local key
  key=$(audit_ns_of "$2")
  DONE_AUDIT_KEYS="$DONE_AUDIT_KEYS
$1:$key"
}

# Pending review is the durable acknowledgement boundary. A late watcher can
# replay unreviewed results, but never reuse history for a queued retry.
REPLAY_CANDIDATES="$EXPECTED_TASK_ID"
if [ -z "$EXPECTED_TASK_ID" ] && [ -f "$PROGRESS_FILE" ]; then
  REPLAY_CANDIDATES=$(grep -F 'Planner_Review: pending' "$PROGRESS_FILE" | grep -Eo 'Task: TASK-[[:alnum:]_.-]+' | awk '{print $2}' | sort -u)
fi
for task_id in $REPLAY_CANDIDATES; do
  if task_ids_from_file "$TASKS_FILE" | grep -Fxq "$task_id"; then continue; fi
  LAST_RESULT=$(grep -E '\[(DONE|FAILED|SCOPE_BLOCKED|GUIDANCE_REQUESTED)\]' "$TASKS_DONE_FILE" 2>/dev/null |
    awk -v expected="$task_id" '{ for(i=1;i<=NF;i++) if($i ~ /^TASK-/) {
      task=$i; sub(/\[.*$/, "", task); if(task==expected) last=$0; break
    }} END {print last}')
  if [[ "$LAST_RESULT" == *"[DONE]"* ]] && matching_audits "$task_id" | grep -Fq 'Planner_Review: pending'; then
    track_task "$task_id"
    PENDING_DONE_TASKS="$PENDING_DONE_TASKS $task_id"
    REPLAY_TASKS="$REPLAY_TASKS $task_id"
    remember_done_key "$task_id" "$LAST_RESULT"
  fi
done

INITIAL_STATE=$(grep -o '"state":[[:space:]]*"[^"]*"' "$STATUS_FILE" 2>/dev/null | cut -d'"' -f4 || true)
INITIAL_TASK=$(canonical_task "$(grep -o '"current_task":[[:space:]]*"[^"]*"' "$STATUS_FILE" 2>/dev/null || true)")
track_task "$EXPECTED_TASK_ID"
echo "[SENTINEL] Watching background worker (Timeout: ${MAX_WAIT_SECONDS}s, Interval: ${POLL_INTERVAL}s${EXPECTED_TASK_ID:+, Task: ${EXPECTED_TASK_ID}})..."

while true; do
  if [ -f "$RUNTIME_PATH" ]; then
    python3 "$RUNTIME_PATH" agent-events-watch \
      --events "$AGENT_EVENTS_FILE" --cursor "$AGENT_EVENTS_CURSOR_FILE" \
      --acknowledgements "$AGENT_EVENTS_ACK_FILE" --gate "$AGENT_DISPATCH_GATE_FILE" \
      --handoff "$PROGRESS_FILE" >/dev/null || echo "[SENTINEL_WARNING] Agent event watcher failed; dispatch remains gated." >&2
  fi
  NOW=$SECONDS
  ELAPSED=$(( NOW - START_TIME ))

  if [ -z "$EXPECTED_TASK_ID" ]; then
    while IFS= read -r task_id; do track_task "$task_id"; done < <(task_ids_from_file "$TASKS_FILE")
  fi

  STATE="UNKNOWN"
  CURRENT_TASK=""
  WORKER_PID=""
  if [ -f "$STATUS_FILE" ]; then
    STATE=$(grep -o '"state":[[:space:]]*"[^"]*"' "$STATUS_FILE" 2>/dev/null | cut -d'"' -f4 || true)
    CURRENT_TASK=$(grep -o '"current_task":[[:space:]]*"[^"]*"' "$STATUS_FILE" 2>/dev/null | grep -Eo 'TASK-[[:alnum:]_.-]+' | head -n 1 || true)
    WORKER_PID=$(grep -o '"pid":[[:space:]]*[0-9]*' "$STATUS_FILE" 2>/dev/null | awk '{print $2}' || true)
    [ -z "$STATE" ] && STATE="UNKNOWN"
  fi
  if [ -z "$EXPECTED_TASK_ID" ]; then track_task "$CURRENT_TASK"; fi

  NEW_DONE=""
  if [ -f "$TASKS_DONE_FILE" ]; then
    CURRENT_DONE_BYTES=$(wc -c < "$TASKS_DONE_FILE" | tr -d '[:space:]')
    if [ "$CURRENT_DONE_BYTES" -lt "$LAST_DONE_BYTES" ]; then
      # The ledger was rotated. Treat its current contents as new evidence.
      LAST_DONE_BYTES=0
    fi
    if [ "$CURRENT_DONE_BYTES" -gt "$LAST_DONE_BYTES" ]; then
      NEW_DONE=$(tail -c +$((LAST_DONE_BYTES + 1)) "$TASKS_DONE_FILE" 2>/dev/null || true)
      LAST_DONE_BYTES="$CURRENT_DONE_BYTES"
    fi
  fi

  # Consume terminal records in ledger order, retaining the latest attempt.
  while IFS= read -r result_line; do
    terminal=$(printf '%s\n' "$result_line" | grep -Eo '\[(DONE|FAILED|SCOPE_BLOCKED|GUIDANCE_REQUESTED)\][[:space:]]+TASK-[[:alnum:]_.-]+' | head -n 1 || true)
    [ -n "$terminal" ] || continue
    task_id=$(canonical_task "$terminal")
    if [ -n "$EXPECTED_TASK_ID" ] && [ "$task_id" != "$EXPECTED_TASK_ID" ]; then continue; fi
    track_task "$task_id"
    if [[ "$terminal" == "[DONE]"* ]]; then
      contains_task "$task_id" "$PENDING_DONE_TASKS" || PENDING_DONE_TASKS="$PENDING_DONE_TASKS $task_id"
      remember_done_key "$task_id" "$result_line"
      PENDING_FAILED_TASKS=" $(printf '%s\n' "$PENDING_FAILED_TASKS" | xargs -n1 2>/dev/null | grep -Fxv "$task_id" | xargs) "
    else
      contains_task "$task_id" "$PENDING_FAILED_TASKS" || PENDING_FAILED_TASKS="$PENDING_FAILED_TASKS $task_id"
      PENDING_DONE_TASKS=" $(printf '%s\n' "$PENDING_DONE_TASKS" | xargs -n1 2>/dev/null | grep -Fxv "$task_id" | xargs) "
      AUDITED_DONE_TASKS=" $(printf '%s\n' "$AUDITED_DONE_TASKS" | xargs -n1 2>/dev/null | grep -Fxv "$task_id" | xargs) "
    fi
  done <<< "$NEW_DONE"

  REMAINING_TASKS=$(grep -v '^[[:space:]]*#' "$TASKS_FILE" 2>/dev/null | grep -v '^[[:space:]]*$' | wc -l | tr -d '[:space:]')
  ACTIVE_TASK=0
  # Treat any RUNNING observation in this iteration as active. A transition to
  # IDLE after the ledger read must not make us expire before reading its DONE.
  if { [ "$STATE" = "RUNNING" ] || [ "$STATE" = "VERIFYING" ]; } &&
     { [ -z "$EXPECTED_TASK_ID" ] || [ "$CURRENT_TASK" = "$EXPECTED_TASK_ID" ]; }; then
    ACTIVE_TASK=1
  fi
  # A task result is not delivered while any worker still executes that task.
  for active_status in "$STATUS_FILE" "$NIGHTSHIFT_DIR"/worker.*.status; do
    [ -f "$active_status" ] || continue
    active_state=$(grep -o '"state":[[:space:]]*"[^"]*"' "$active_status" | cut -d'"' -f4 || true)
    active_id=$(canonical_task "$(grep -o '"current_task":[[:space:]]*"[^"]*"' "$active_status" || true)")
    if { [ "$active_state" = "RUNNING" ] || [ "$active_state" = "VERIFYING" ]; } &&
       { [ -z "$EXPECTED_TASK_ID" ] || [ "$active_id" = "$EXPECTED_TASK_ID" ]; }; then
      ACTIVE_TASK=1
    fi
  done

  for task_id in $PENDING_DONE_TASKS; do
    FRESH_AUDIT=0
    required_ns=$(printf '%s\n' "$DONE_AUDIT_KEYS" | awk -F: -v expected="$task_id" '$1==expected {key=$2} END {print key}')
    while IFS= read -r audit_line; do
      [ -n "$audit_line" ] || continue
      if [ -n "$required_ns" ] && [ "$(audit_ns_of "$audit_line")" != "$required_ns" ]; then continue; fi
      identity=$(audit_identity "$audit_line")
      if contains_task "$task_id" "$REPLAY_TASKS" ||
         ! sed -E 's/ \| Planner_Review:.*$//' "$BASELINE_AUDITS" | grep -Fqx -- "$identity"; then
        FRESH_AUDIT=1
        break
      fi
    done < <(matching_audits "$task_id")
    if [ "$FRESH_AUDIT" -eq 1 ]; then
      contains_task "$task_id" "$AUDITED_DONE_TASKS" || AUDITED_DONE_TASKS="$AUDITED_DONE_TASKS $task_id"
    fi
  done

  if [ "$ACTIVE_TASK" -eq 0 ]; then
    if [ -n "$EXPECTED_TASK_ID" ]; then
      if contains_task "$EXPECTED_TASK_ID" "$PENDING_FAILED_TASKS"; then
        echo "[SENTINEL_FAILED] Task $EXPECTED_TASK_ID received a failure result after ${ELAPSED}s."
        exit 1
      fi
      if contains_task "$EXPECTED_TASK_ID" "$AUDITED_DONE_TASKS"; then
        echo "[SENTINEL_COMPLETED] Task $EXPECTED_TASK_ID completed with a task-scoped mechanical audit PASS in ${ELAPSED}s."
        exit 0
      fi
    elif [ "$REMAINING_TASKS" -eq 0 ]; then
      if [ -n "${PENDING_FAILED_TASKS// /}" ]; then
        echo "[SENTINEL_FAILED] Tasks $PENDING_FAILED_TASKS received failure results after ${ELAPSED}s."
        exit 1
      fi
      if [ -n "$AUDITED_DONE_TASKS" ] && { [ "$STATE" = "IDLE" ] || [ "$STATE" = "STOPPED" ]; }; then
        echo "[SENTINEL_COMPLETED] Task $AUDITED_DONE_TASKS completed with a task-scoped mechanical audit PASS in ${ELAPSED}s."
        exit 0
      fi
    fi
  fi

  # Running/verification and an unrelated task's status cannot wake Planner.
  STATUS_RELEVANT=1
  if [ -z "$EXPECTED_TASK_ID" ] && [ "$STATE|$CURRENT_TASK" = "$INITIAL_STATE|$INITIAL_TASK" ]; then
    STATUS_RELEVANT=0
  fi
  if [ -n "$EXPECTED_TASK_ID" ] && [ "$CURRENT_TASK" != "$EXPECTED_TASK_ID" ]; then STATUS_RELEVANT=0; fi
  if task_ids_from_file "$TASKS_FILE" | grep -Fxq "$EXPECTED_TASK_ID"; then STATUS_RELEVANT=0; fi
  if [ "$ACTIVE_TASK" -eq 0 ] && [ "$STATUS_RELEVANT" -eq 1 ]; then
  if [ "$STATE" = "SCOPE_BLOCKED" ]; then
    echo "[SENTINEL_SCOPE_BLOCKED] Worker encountered unannounced downstream scope after ${ELAPSED}s. (No-fault escalation)."
    exit 3
  fi
  if [ "$STATE" = "BLOCKED" ] || [ "$STATE" = "AWAITING_GUIDANCE" ]; then
    echo "[SENTINEL_BLOCKED] Worker entered state: $STATE after ${ELAPSED}s."
    exit 1
  fi
  if [ "$STATE" = "AGENT_NO_RESPONSE" ]; then
    echo "[SENTINEL_AGENT_NO_RESPONSE] Worker entered state: $STATE after ${ELAPSED}s."
    exit 1
  fi
  if [ "$STATE" = "PAUSED_FOR_PLANNER" ]; then
    echo "[SENTINEL_PAUSED_FOR_PLANNER] Worker entered state: $STATE after ${ELAPSED}s."
    exit 1
  fi

  fi

  if [ -n "$WORKER_PID" ] && [ "$STATE" = "RUNNING" ] &&
     { [ -z "$EXPECTED_TASK_ID" ] || [ "$CURRENT_TASK" = "$EXPECTED_TASK_ID" ]; }; then
    if ! kill -0 "$WORKER_PID" 2>/dev/null; then
      echo "[SENTINEL_WORKER_DIED] Worker process $WORKER_PID terminated unexpectedly."
      exit 1
    fi
  fi

  if [ "$MAX_WAIT_SECONDS" -gt 0 ] && [ "$ELAPSED" -ge "$MAX_WAIT_SECONDS" ] && [ "$ACTIVE_TASK" -eq 0 ]; then
    echo "[SENTINEL_TIMEOUT] Exceeded ${MAX_WAIT_SECONDS}s waiting for task completion; no task failure inferred."
    exit 2
  fi

  sleep "$POLL_INTERVAL"
done
