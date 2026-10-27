#!/usr/bin/env bash
# ==============================================================================
# Signature Skeleton & Code Slice Extractor
# Extracts type/function declarations and 10-line context slices for SUMMARY.md
# Usage: scripts/extract_skeleton.sh <file>#L<line> OR scripts/extract_skeleton.sh <file> [line]
# ==============================================================================

set -uo pipefail

if [ "${NIGHTSHIFT_WORKER:-0}" != "1" ]; then
  echo "[GUARD] scripts/extract_skeleton.sh is restricted to the Background Worker (Terminal B)."
  echo "[GUARD] In Terminal A, read active .reports/<slug>/SUMMARY.md or nightshift/.worker.notes.md; if missing, enqueue a [RESEARCH] task to nightshift/tasks.txt."
  exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TARGET="${1:-}"
LINE="${2:-}"

if [[ "$TARGET" =~ ^(.*)#L([0-9]+)$ ]]; then
  TARGET="${BASH_REMATCH[1]}"
  LINE="${BASH_REMATCH[2]}"
fi

if [ -z "$TARGET" ] || [ ! -f "$TARGET" ]; then
  echo "Usage: $0 <file>#L<line_number>  OR  $0 <file> [line_number]"
  exit 1
fi

EXT="${TARGET##*.}"
case "$EXT" in
  swift) LANG="swift" ;;
  py)    LANG="python" ;;
  ts|js) LANG="typescript" ;;
  rs)    LANG="rust" ;;
  go)    LANG="go" ;;
  *)     LANG="" ;;
esac

echo "### Interface Skeleton: \`$TARGET\`"
echo "\`\`\`$LANG"
grep -E '^[[:space:]]*(public |private |internal |fileprivate |open |export |protected |async )*(final )*(protocol|struct|class|enum|actor|extension|func |var |let |def |type |interface |init\(|init\?|trait|impl)' "$TARGET" 2>/dev/null | sed -E 's/\{[[:space:]]*$//' | head -n 40 || true
echo "\`\`\`"

if [ -n "$LINE" ]; then
  echo ""
  echo "### Context Slice: \`$TARGET#L$LINE\`"
  echo "\`\`\`$LANG"
  bash "$SCRIPT_DIR/probe_line.sh" "$TARGET" "$LINE" 10 || true
  echo "\`\`\`"
fi
