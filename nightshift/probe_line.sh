#!/usr/bin/env bash
# ==============================================================================
# Code Probe Helper (Standardized Single-Point Auditor)
# Used by Planner or Sentinel to verify line-number claims in SUMMARY.md
# Usage: scripts/probe_line.sh <file>#L<line> [context_lines]
#    OR: scripts/probe_line.sh <file> <line> [context_lines]
# ==============================================================================

set -uo pipefail

if [ "${NIGHTSHIFT_WORKER:-0}" != "1" ]; then
  echo "[GUARD] scripts/probe_line.sh is restricted to the Background Worker (Terminal B)."
  echo "[GUARD] In Terminal A, read active .reports/<slug>/SUMMARY.md or nightshift/.worker.notes.md; if missing, enqueue a [RESEARCH] task to nightshift/tasks.txt."
  exit 1
fi

TARGET="${1:-}"
LINE="${2:-}"
CTX="${3:-10}"

if [[ "$TARGET" =~ ^(.*)#L([0-9]+)$ ]]; then
  TARGET="${BASH_REMATCH[1]}"
  LINE="${BASH_REMATCH[2]}"
  CTX="${2:-$CTX}"
fi

if [ -z "$TARGET" ] || [ -z "$LINE" ] || [ ! -f "$TARGET" ]; then
  echo "Usage: $0 <file>#L<line_number> [context_lines]  OR  $0 <file> <line_number> [context_lines]"
  exit 1
fi

START=$(( LINE - CTX ))
[ $START -lt 1 ] && START=1
END=$(( LINE + CTX ))

awk -v s="$START" -v e="$END" -v tgt="$LINE" '
  NR >= s && NR <= e {
    marker = (NR == tgt) ? ">> " : "   ";
    printf "%s%4d | %s\n", marker, NR, $0;
  }
' "$TARGET"
