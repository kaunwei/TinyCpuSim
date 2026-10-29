#!/usr/bin/env bash
# Compatibility launcher for the Python NightShift worker runtime.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -f "$SCRIPT_DIR/worker_runtime.py" ]; then
  NIGHTSHIFT_DIR="$SCRIPT_DIR"
  RUNTIME_PATH="$SCRIPT_DIR/worker_runtime.py"
  if [ "$(basename "$SCRIPT_DIR")" = "nightshift" ]; then
    PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
  else
    PROJECT_ROOT="$SCRIPT_DIR"
  fi
elif [ -f "$SCRIPT_DIR/scripts/worker_runtime.py" ]; then
  # Keep the legacy launcher seam usable for existing runtime test fixtures.
  NIGHTSHIFT_DIR="$SCRIPT_DIR"
  RUNTIME_PATH="$SCRIPT_DIR/scripts/worker_runtime.py"
  PROJECT_ROOT="$SCRIPT_DIR"
else
  echo "[FATAL ERROR] Missing nightshift/worker_runtime.py" >&2
  exit 1
fi

# Worker runtime settings live in this launcher. Agent-specific settings live
# in scripts/adapters/agents.json and are passed through the common adapter protocol.
PYTHON_BIN="${PYTHON_BIN:-python3}"
WORKER_ID="${WORKER_ID:-default}"
OS_TYPE="${OS_TYPE:-auto}"
TASKS_FILE="${TASKS_FILE:-$NIGHTSHIFT_DIR/tasks.txt}"
TASKS_DONE_FILE="${TASKS_DONE_FILE:-$NIGHTSHIFT_DIR/tasks.done}"
PROGRESS_FILE="${PROGRESS_FILE:-$NIGHTSHIFT_DIR/progress.log}"
PAUSE_FILE="${PAUSE_FILE:-$NIGHTSHIFT_DIR/PAUSE}"
WORKER_SCORE_FILE="${WORKER_SCORE_FILE:-$NIGHTSHIFT_DIR/.worker-score.json}"
WORKER_AGENT_SCORES_DIR="${WORKER_AGENT_SCORES_DIR:-$NIGHTSHIFT_DIR/.worker-agent-scores}"
WORKER_AGENT_EVENTS_FILE="${WORKER_AGENT_EVENTS_FILE:-$NIGHTSHIFT_DIR/.worker-agent-events.jsonl}"
AI_AGENT_STATE_FILE="${AI_AGENT_STATE_FILE:-$NIGHTSHIFT_DIR/.worker-agent-state.$WORKER_ID.json}"
WORKER_GHOST_STATE_FILE="${WORKER_GHOST_STATE_FILE:-$NIGHTSHIFT_DIR/.worker-ghost-state.$WORKER_ID.json}"
WORKER_GHOST_ARTIFACT_DIR="${WORKER_GHOST_ARTIFACT_DIR:-$NIGHTSHIFT_DIR/.worker-ghosts}"
WORKER_GHOST_STEP_BUDGET="${WORKER_GHOST_STEP_BUDGET:-25}"
WORKER_GHOST_TIMEOUT="${WORKER_GHOST_TIMEOUT:-180}"
AGENT_DISPATCH_GATE_FILE="${AGENT_DISPATCH_GATE_FILE:-$NIGHTSHIFT_DIR/.worker-dispatch-gate.json}"
if [ "$WORKER_ID" = "default" ]; then
  DEFAULT_STATUS_FILE="$NIGHTSHIFT_DIR/worker.status"
  DEFAULT_LOG_FILE="$NIGHTSHIFT_DIR/.worker.log"
  DEFAULT_PID_FILE="$NIGHTSHIFT_DIR/.worker.pid"
else
  DEFAULT_STATUS_FILE="$NIGHTSHIFT_DIR/worker.$WORKER_ID.status"
  DEFAULT_LOG_FILE="$NIGHTSHIFT_DIR/.worker.$WORKER_ID.log"
  DEFAULT_PID_FILE="$NIGHTSHIFT_DIR/.worker.$WORKER_ID.pid"
fi
STATUS_FILE="${STATUS_FILE:-$DEFAULT_STATUS_FILE}"
LOG_FILE="${LOG_FILE:-$DEFAULT_LOG_FILE}"
PID_FILE="${PID_FILE:-$DEFAULT_PID_FILE}"
TASK_TIMEOUT="${TASK_TIMEOUT:-1800}"
STEP_BASE="${STEP_BASE:-12}"
STEP_PER_FILE="${STEP_PER_FILE:-5}"
RESEARCH_STEP_BUDGET="${RESEARCH_STEP_BUDGET:-100}"
POLL_INTERVAL="${POLL_INTERVAL:-5}"
RUN_ONCE="${RUN_ONCE:-0}"
WORKER_PORT="${WORKER_PORT:-3000}"
if [ -f "$NIGHTSHIFT_DIR/adapters/agents.json" ]; then
  DEFAULT_AGENT_CONFIG="$NIGHTSHIFT_DIR/adapters/agents.json"
else
  DEFAULT_AGENT_CONFIG="$NIGHTSHIFT_DIR/scripts/adapters/agents.json"
fi
AGENT_CONFIG_FILE="${AGENT_CONFIG_FILE:-$DEFAULT_AGENT_CONFIG}"

if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
  echo "[FATAL ERROR] Python runtime not found: $PYTHON_BIN" >&2
  exit 1
fi
if [ ! -f "$RUNTIME_PATH" ]; then
  echo "[FATAL ERROR] Missing nightshift/worker_runtime.py" >&2
  exit 1
fi

# Export only documented worker settings to the runtime.
export AGENT_CONFIG_FILE
export WORKER_ID OS_TYPE TASKS_FILE TASKS_DONE_FILE PROGRESS_FILE PAUSE_FILE
export WORKER_SCORE_FILE WORKER_AGENT_SCORES_DIR WORKER_AGENT_EVENTS_FILE
export AI_AGENT_STATE_FILE WORKER_GHOST_STATE_FILE WORKER_GHOST_ARTIFACT_DIR WORKER_GHOST_STEP_BUDGET WORKER_GHOST_TIMEOUT
export AGENT_DISPATCH_GATE_FILE STATUS_FILE LOG_FILE PID_FILE TASK_TIMEOUT STEP_BASE STEP_PER_FILE RESEARCH_STEP_BUDGET
export POLL_INTERVAL RUN_ONCE WORKER_PORT

mkdir -p "$NIGHTSHIFT_DIR"
exec "$PYTHON_BIN" "$RUNTIME_PATH" daemon --root "$PROJECT_ROOT"
