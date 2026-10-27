#!/usr/bin/env bash
# Install NightShift or update an existing deployment in place.
set -euo pipefail

SCRIPT_DIR="$(cd -P "$(dirname "${BASH_SOURCE[0]}")" && pwd -P)"
if [ -d "$SCRIPT_DIR/nightshift" ] && [ -f "$SCRIPT_DIR/nightshift/worker.sh" ]; then
  SOURCE_ROOT="$SCRIPT_DIR"
  BUNDLE_SOURCE="$SCRIPT_DIR/nightshift"
  SELF_DEPLOYED=0
elif [ -f "$SCRIPT_DIR/worker.sh" ] && [ -f "$SCRIPT_DIR/worker_runtime.py" ]; then
  BUNDLE_SOURCE="$SCRIPT_DIR"
  SOURCE_ROOT="$(cd "$SCRIPT_DIR/.." && pwd -P)"
  SELF_DEPLOYED=1
else
  echo "Error: cannot find the NightShift bundle beside $SCRIPT_DIR" >&2
  exit 1
fi

if [ "$#" -ne 1 ] || [ -z "${1:-}" ]; then
  echo "Usage: $0 <target_repository_path>" >&2
  exit 2
fi
if ! TARGET_DIR="$(cd -P -- "$1" 2>/dev/null && pwd -P)"; then
  echo "Error: target directory does not exist: $1" >&2
  exit 2
fi
PYTHON_BIN="${PYTHON_BIN:-python3}"
if [ "$TARGET_DIR" = "$SOURCE_ROOT" ] && [ "$SELF_DEPLOYED" -eq 0 ]; then
  echo "Refusing to install NightShift into its source repository." >&2
  exit 2
fi

IS_UPDATE=0
if [ -d "$TARGET_DIR/nightshift" ] || [ -f "$TARGET_DIR/worker.sh" ] || [ -d "$TARGET_DIR/.agents/skills/nightshift" ]; then
  IS_UPDATE=1
fi
printf 'NightShift %s: %s\n' "$([ "$IS_UPDATE" -eq 1 ] && echo update || echo install)" "$TARGET_DIR"

mkdir -p "$TARGET_DIR/nightshift/adapters"
# Migrate consumer configuration before retiring legacy locations.
if [ ! -f "$TARGET_DIR/nightshift/adapters/agents.json" ]; then
  for registry in "$TARGET_DIR/scripts/adapters/agents.json" "$TARGET_DIR/agents/agents.json"; do
    if [ -f "$registry" ]; then
      cp "$registry" "$TARGET_DIR/nightshift/adapters/agents.json"
      break
    fi
  done
fi
if [ ! -f "$TARGET_DIR/nightshift/code-index.toml" ] && [ -f "$TARGET_DIR/.nightshift/code-index.toml" ]; then
  cp "$TARGET_DIR/.nightshift/code-index.toml" "$TARGET_DIR/nightshift/code-index.toml"
fi
# Deploy only listed runtime files, never the source bundle or its development docs.
"$PYTHON_BIN" - "$BUNDLE_SOURCE" "$TARGET_DIR/nightshift" <<'PYDEPLOY'
import json
from pathlib import Path
import shutil
import sys
source, target = map(Path, sys.argv[1:])
manifest = json.loads((source / "install-manifest.json").read_text(encoding="utf-8"))
for relative in manifest["runtime"]:
    origin, destination = source / relative, target / relative
    if origin.resolve() == destination.resolve():
        continue
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(origin, destination)
PYDEPLOY
if [ "$SCRIPT_DIR/install.sh" != "$TARGET_DIR/nightshift/install.sh" ]; then
  cp -f "$SCRIPT_DIR/install.sh" "$TARGET_DIR/nightshift/install.sh"
fi
for script in worker.sh extract_skeleton.sh probe_line.sh reorder_queue.sh status.sh watch_worker.sh install.sh tools/setup.sh; do
  chmod +x "$TARGET_DIR/nightshift/$script"
done

# Move old root-level project settings/state into the NightShift-owned folder.
if [ ! -f "$TARGET_DIR/nightshift/code-index.toml" ]; then
  cp "$BUNDLE_SOURCE/code-index.toml" "$TARGET_DIR/nightshift/code-index.toml"
fi
"$PYTHON_BIN" - "$TARGET_DIR/nightshift/code-index.toml" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
text = text.replace("tools/nightshift-code-index/**", "nightshift/tools/**")
path.write_text(text, encoding="utf-8")
PY
rm -f "$TARGET_DIR/.nightshift/code-index.toml"
rmdir "$TARGET_DIR/.nightshift" 2>/dev/null || true
if [ ! -f "$TARGET_DIR/nightshift/adapters/agents.json" ] && [ -f "$TARGET_DIR/scripts/adapters/agents.json" ]; then
  mkdir -p "$TARGET_DIR/nightshift/adapters"
  mv "$TARGET_DIR/scripts/adapters/agents.json" "$TARGET_DIR/nightshift/adapters/agents.json"
fi
state_files=(tasks.txt tasks.done progress.log worker.status .worker.log .worker.pid .worker.notes.md \
  .worker-score.json .worker-agent-events.jsonl .worker-agent-events.cursor.json .worker-agent-events.acks.jsonl \
  .worker-dispatch-gate.json .worker-agent-state.default.json .worker-ghost-state.default.json PAUSE)
for state_file in "${state_files[@]}"; do
  if [ -f "$TARGET_DIR/$state_file" ] && [ ! -e "$TARGET_DIR/nightshift/$state_file" ]; then
    mv "$TARGET_DIR/$state_file" "$TARGET_DIR/nightshift/$state_file"
  fi
done
for state_file in "$TARGET_DIR"/.worker-agent-state.*.json "$TARGET_DIR"/.worker-ghost-state.*.json \
  "$TARGET_DIR"/worker.*.status "$TARGET_DIR"/.worker.*.log "$TARGET_DIR"/.worker.*.pid; do
  [ -f "$state_file" ] || continue
  name="$(basename "$state_file")"
  [ -e "$TARGET_DIR/nightshift/$name" ] || mv "$state_file" "$TARGET_DIR/nightshift/$name"
done
for state_dir in .worker-agent-scores .worker-ghosts; do
  if [ -d "$TARGET_DIR/$state_dir" ] && [ ! -e "$TARGET_DIR/nightshift/$state_dir" ]; then
    mv "$TARGET_DIR/$state_dir" "$TARGET_DIR/nightshift/$state_dir"
  fi
done
for state_dir in tasks.txt.lock .worker-score.json.lock .worker-dispatch-gate.json.lock \
  .worker-agent-events.jsonl.lock .worker-agent-events.cursor.json.lock; do
  if [ -d "$TARGET_DIR/$state_dir" ] && [ ! -e "$TARGET_DIR/nightshift/$state_dir" ]; then
    mv "$TARGET_DIR/$state_dir" "$TARGET_DIR/nightshift/$state_dir"
  fi
done

# Older Worker versions wrote snapshots to the project root. Keep their history
# when upgrading, but move it into the NightShift-owned runtime directory.
if [ -d "$TARGET_DIR/.scratch" ]; then
  mkdir -p "$TARGET_DIR/nightshift/.scratch"
  "$PYTHON_BIN" - "$TARGET_DIR/.scratch" "$TARGET_DIR/nightshift/.scratch" <<'PY'
import shutil
import sys
from pathlib import Path

source, target = map(Path, sys.argv[1:])
for child in source.iterdir():
    destination = target / child.name
    if child.is_dir() and not child.is_symlink():
        if destination.exists() and destination.is_dir():
            shutil.copytree(child, destination, dirs_exist_ok=True)
            shutil.rmtree(child)
        elif not destination.exists():
            child.rename(destination)
        else:
            child.rename(target / f"legacy-root-{child.name}")
    elif destination.exists():
        alternate = target / f"legacy-root-{child.name}"
        index = 2
        while alternate.exists():
            alternate = target / f"legacy-root-{index}-{child.name}"
            index += 1
        child.rename(alternate)
    else:
        child.rename(destination)
source.rmdir()
PY
fi

# Remove only exact legacy NightShift files after their user data has been moved.
rm -f "$TARGET_DIR/worker.sh" "$TARGET_DIR/update.sh" "$TARGET_DIR/.worker.env.example" \
  "$TARGET_DIR/scripts/worker_runtime.py" "$TARGET_DIR/scripts/adapters/agy.py" \
  "$TARGET_DIR/scripts/adapters/codex.py" "$TARGET_DIR/scripts/adapters/generic.py" \
  "$TARGET_DIR/scripts/adapters/agents.json" "$TARGET_DIR/scripts/extract_skeleton.sh" \
  "$TARGET_DIR/scripts/probe_line.sh" "$TARGET_DIR/scripts/reorder_queue.sh" \
  "$TARGET_DIR/scripts/status.sh" "$TARGET_DIR/scripts/watch_worker.sh"
rm -rf "$TARGET_DIR/tools/nightshift-code-index" "$TARGET_DIR/test/nightshift_worker"
rm -f "$TARGET_DIR/nightshift/tools/bin/code-index" "$TARGET_DIR/nightshift/tools/bin/code-index.cmd"
rmdir "$TARGET_DIR/nightshift/tools/bin" 2>/dev/null || true
rmdir "$TARGET_DIR/scripts/adapters" 2>/dev/null || true
rmdir "$TARGET_DIR/.nightshift" 2>/dev/null || true

# Preserve the existing agent registry; otherwise the bundled default is ready to use.
if [ ! -f "$TARGET_DIR/nightshift/adapters/agents.json" ]; then
  mkdir -p "$TARGET_DIR/nightshift/adapters"
  cp "$BUNDLE_SOURCE/adapters/agents.json" "$TARGET_DIR/nightshift/adapters/agents.json"
fi
"$PYTHON_BIN" - "$TARGET_DIR/nightshift/adapters/agents.json" <<'PY'
import json
from pathlib import Path
import sys
registry = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
if not isinstance(registry, dict):
    raise SystemExit("agent registry must be a JSON object keyed by agent name")
migrated = False
for name, profile in registry.items():
    if not isinstance(profile, dict) or "agent" in profile:
        continue
    if not {"adapter", "command"}.intersection(profile):
        continue
    config = profile.get("config", {})
    if not isinstance(config, dict):
        raise SystemExit(f"legacy agent {name} config must be an object")
    config = dict(config)
    adapter = profile.get("adapter", "")
    old_launcher = (Path(profile.get("command", "")).name in ("python", "python3")
                    and bool(profile.get("args"))
                    and Path(profile["args"][0]).name == f"{adapter}.py")
    command = config.pop("executable", None) or profile.get("command", "")
    if old_launcher and command == profile.get("command", ""):
        command = adapter
    args = config.pop("args", [] if old_launcher else profile.get("args", []))
    env = config.pop("env", profile.get("env", {}))
    worker = profile.get("worker", {"task_timeout_seconds": 1800})
    registry[name] = {"default": bool(profile.get("default", False)), "worker": worker,
                      "agent": {"adapter": adapter, "command": command, "args": args,
                                "env": env, "config": config}}
    migrated = True
if migrated:
    Path(sys.argv[1]).write_text(json.dumps(registry, indent=2, ensure_ascii=False) + "\n",
                                 encoding="utf-8")
PY

# Skill sources and installed skills share the host-discovered root layout.
"$PYTHON_BIN" - "$BUNDLE_SOURCE" "$SOURCE_ROOT" "$TARGET_DIR" <<'PYSKILLS'
import json
from pathlib import Path
import shutil
import sys
bundle, root, target = map(Path, sys.argv[1:])
manifest = json.loads((bundle / "install-manifest.json").read_text(encoding="utf-8"))
def copy_asset(origin, destination):
    if origin.resolve() == destination.resolve():
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(origin, destination)
for asset in manifest["skill_files"]:
    origin = root / asset["source"]
    copy_asset(origin, target / asset["target"])
for category in manifest["shared_skill_categories"]:
    category_root = root / ".agents/skills" / category
    for entry in sorted(category_root.glob("*/SKILL.md")):
        for origin in sorted(entry.parent.rglob("*")):
            if not origin.is_file() or origin.name == ".DS_Store" or "__pycache__" in origin.parts:
                continue
            copy_asset(origin, target / ".agents/skills" / category / origin.relative_to(category_root))
catalog = root / ".agents/skills.json"
if catalog.is_file() and not (target / ".agents/skills.json").exists():
    copy_asset(catalog, target / ".agents/skills.json")
# Exact retired assets only: keep project notes, queues, registries and scratch data.
parents = set()
for relative in manifest["retired_files"]:
    path = target / "nightshift" / relative
    if path.is_file() or path.is_symlink():
        path.unlink()
    parents.update(path.parents)
for directory in sorted(parents, key=lambda path: len(path.parts), reverse=True):
    if directory == target / "nightshift" or target / "nightshift" not in directory.parents:
        continue
    try:
        directory.rmdir()
    except OSError:
        pass
PYSKILLS

# Create NightShift state only when missing; existing queues and history survive updates.
mkdir -p "$TARGET_DIR/nightshift"
if [ ! -f "$TARGET_DIR/nightshift/tasks.txt" ]; then
  cat > "$TARGET_DIR/nightshift/tasks.txt" <<'QUEUE'
# Task Queue (FIFO) - Format: TASK-XXX | LEVEL: 1-3 | [AFTER: <id>] | TARGET: <paths> | ACTION: <logic> | VERIFY: <test_cmd> | FORBIDDEN_SHORTCUTS: <hacks> | CONSTRAINTS: <limits>
QUEUE
fi
for state_file in tasks.done progress.log; do touch "$TARGET_DIR/nightshift/$state_file"; done
if [ ! -f "$TARGET_DIR/nightshift/.worker.notes.md" ]; then
  cat > "$TARGET_DIR/nightshift/.worker.notes.md" <<'NOTES'
# Worker Repository Notes & Compiler Quirks
# Keep reusable build/toolchain quirks here; do not record task history or secrets.
NOTES
fi
# Keep generated runtime state and the private parser venv out of Git.
touch "$TARGET_DIR/.gitignore"
IGNORE_HEADER="# NightShift runtime artifacts (stored under nightshift/)"
IGNORE_ENTRIES=(
  "nightshift/tasks.done" "nightshift/tasks.done.*" "nightshift/worker*.status"
  "nightshift/.worker*.log" "nightshift/.worker*.pid" "nightshift/.worker-agent-state*.json"
  "nightshift/.worker-score*.json" "nightshift/.worker-agent-scores/" "nightshift/.worker-agent-events*.json*"
  "nightshift/.worker-dispatch-gate.json" "nightshift/.worker*.lock/" "nightshift/PAUSE" "nightshift/tasks.txt.lock/"
  "nightshift/.worker-ghosts/" "nightshift/.worker-ghost-state*.json" "nightshift/.scratch/" "nightshift/tools/.venv/"
  ".trials/"
)
LEGACY_IGNORE_ENTRIES=(
  "worker*.status" ".worker*.log" ".worker*.pid" "PAUSE" ".scratch/"
  "tasks.txt.lock/" ".worker-agent-state*.json" ".worker-score*.json"
  ".worker-score*.json.lock/" "tasks.done" "tasks.done.*" ".worker-agent-scores/"
  ".worker-agent-events.jsonl" ".worker-agent-events.jsonl.lock/" ".worker-agent-events*.json*"
  ".worker-dispatch-gate.json" ".worker-dispatch-gate.json.lock/"
  ".worker-ghost-state*.json" ".worker-ghost-state*.json.lock/" ".worker-ghosts/"
)
if [ "${#LEGACY_IGNORE_ENTRIES[@]}" -gt 0 ]; then
  "$PYTHON_BIN" - "$TARGET_DIR/.gitignore" "${LEGACY_IGNORE_ENTRIES[@]}" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
legacy = set(sys.argv[2:])
if path.exists():
    lines = path.read_text(encoding="utf-8").splitlines()
    path.write_text("".join(line + "\n" for line in lines if line not in legacy), encoding="utf-8")
PY
fi
if ! grep -qxF "$IGNORE_HEADER" "$TARGET_DIR/.gitignore"; then
  [ ! -s "$TARGET_DIR/.gitignore" ] || [ "$(tail -c 1 "$TARGET_DIR/.gitignore")" = $'\n' ] || printf '\n' >> "$TARGET_DIR/.gitignore"
  printf '%s\n' "$IGNORE_HEADER" >> "$TARGET_DIR/.gitignore"
fi
for entry in "${IGNORE_ENTRIES[@]}"; do
  grep -qxF "$entry" "$TARGET_DIR/.gitignore" || printf '%s\n' "$entry" >> "$TARGET_DIR/.gitignore"
done

# Persist generated-artifact exclusions outside tracked .gitignore. Attempt
# rollback may reset an uncommitted .gitignore before cleaning task outputs.
if git -C "$TARGET_DIR" rev-parse --git-dir >/dev/null 2>&1; then
  EXCLUDE_FILE=$(git -C "$TARGET_DIR" rev-parse --git-path info/exclude)
  case "$EXCLUDE_FILE" in /*) ;; *) EXCLUDE_FILE="$TARGET_DIR/$EXCLUDE_FILE" ;; esac
  "$PYTHON_BIN" - "$EXCLUDE_FILE" "${IGNORE_ENTRIES[@]}" <<'PYEXCLUDE'
from pathlib import Path
import sys
path = Path(sys.argv[1])
old = path.read_text(encoding="utf-8") if path.exists() else ""
entries = old.splitlines()
missing = ["/" + item for item in sys.argv[2:] if "/" + item not in entries]
if missing:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as output:
        output.write(("\n" if old and not old.endswith("\n") else "") +
                     "\n# NightShift generated state survives tracked-ignore rollback.\n" +
                     "\n".join(missing) + "\n")
PYEXCLUDE
fi

# Install all parser dependencies during both fresh installs and updates.
PYTHON_BIN="$PYTHON_BIN" "$TARGET_DIR/nightshift/tools/setup.sh"
"$TARGET_DIR/nightshift/tools/.venv/bin/python" "$TARGET_DIR/nightshift/tools/index.py" --help >/dev/null

# Validate deployed entry points and runtime files before reporting success.
bash -n "$TARGET_DIR/nightshift/"*.sh
sh -n "$TARGET_DIR/nightshift/tools/setup.sh"
"$PYTHON_BIN" -c 'import ast, pathlib, sys; ast.parse(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))' "$TARGET_DIR/nightshift/worker_runtime.py"
"$PYTHON_BIN" -c 'import ast, pathlib, sys; ast.parse(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))' "$TARGET_DIR/nightshift/tools/index.py"

printf '\nNightShift %s complete.\n' "$([ "$IS_UPDATE" -eq 1 ] && echo update || echo install)"
printf '  Worker: %s\n  Queue:  %s\n  Index:  %s\n' \
  "$TARGET_DIR/nightshift/worker.sh" "$TARGET_DIR/nightshift/tasks.txt" \
  "$TARGET_DIR/nightshift/.scratch/code-index/INDEX.md"
