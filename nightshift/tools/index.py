#!/usr/bin/env python3
"""Build and query a bounded, source-backed, multi-language code index."""

from __future__ import annotations

import argparse
from collections import Counter
import fnmatch
import hashlib
from importlib.metadata import version as package_version
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

try:
    import fcntl
except ImportError:  # Windows
    fcntl = None
    import msvcrt

try:
    import tomllib
except ModuleNotFoundError:  # Python 3.9 and 3.10
    import tomli as tomllib

from tree_sitter_language_pack import get_parser


SCHEMA_VERSION = 5
SUMMARY_MAX_BYTES = 8192
IGNORED_PARTS = {
    ".git", ".build", ".swiftpm", "build", "DerivedData", ".worker-tree",
    ".preview", ".scratch", "nightshift/.scratch", ".trials", ".reports", ".venv", ".aider.tags.cache.v4",
}
TOOL_ROOT = Path(__file__).resolve().parent
LANGUAGE_REGISTRY = json.loads((TOOL_ROOT / "languages.json").read_text(encoding="utf-8"))
EXTENSIONS = {
    extension: language
    for language, spec in LANGUAGE_REGISTRY.items()
    for extension in spec["extensions"]
}
SPECIAL_FILENAMES = {
    "cmakelists.txt": "cmake",
    "makefile": "make",
    "gnumakefile": "make",
}
IDENTIFIER_TYPES = {
    language: set(spec["identifier_types"])
    for language, spec in LANGUAGE_REGISTRY.items()
}
DECLARATION_TYPES = {
    language: set(spec["declaration_types"])
    for language, spec in LANGUAGE_REGISTRY.items()
}
IMPORT_TYPES = {
    language: set(spec["import_types"])
    for language, spec in LANGUAGE_REGISTRY.items()
}
CALL_TYPES = {
    language: set(spec["call_types"])
    for language, spec in LANGUAGE_REGISTRY.items()
}


class IndexErrorMessage(Exception):
    """An actionable error that can be returned as machine-readable JSON."""


def git_output(root: Path, *args: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(root), *args], capture_output=True, text=True
    )
    if result.returncode:
        raise IndexErrorMessage(result.stderr.strip() or "Git query failed")
    return result.stdout.strip()


def repository_root(repository: Path) -> Path:
    root = git_output(repository.resolve(), "rev-parse", "--show-toplevel")
    return Path(root).resolve()


def index_location(root: Path) -> Path:
    return root / "nightshift" / ".scratch" / "code-index" / "index.json"


def summary_location(root: Path) -> Path:
    return index_location(root).with_name("INDEX.md")


def project_config(root: Path) -> dict:
    path = root / "nightshift" / "code-index.toml"
    legacy_path = root / ".nightshift" / "code-index.toml"
    if not path.is_file() and legacy_path.is_file():
        path = legacy_path
    if not path.is_file():
        return {"include": ["*", "**/*"], "exclude": [], "tool_paths": [], "project_paths": ["*", "**/*"]}
    try:
        parsed = tomllib.loads(path.read_text(encoding="utf-8"))
    except (OSError, tomllib.TOMLDecodeError) as exc:
        raise IndexErrorMessage(f"project config is invalid: {path}: {exc}") from exc
    project = parsed.get("project", {})
    scope = parsed.get("scope", {})
    return {
        "include": project.get("include", ["*", "**/*"]),
        "exclude": project.get("exclude", []),
        "tool_paths": scope.get("tool", []),
        "project_paths": scope.get("project", ["*", "**/*"]),
    }


def language_for_path(path: Path) -> str | None:
    return SPECIAL_FILENAMES.get(path.name.lower(), EXTENSIONS.get(path.suffix.lower()))


def source_files(root: Path) -> list[Path]:
    config = project_config(root)
    paths = []
    for path in root.rglob("*"):
        if not path.is_file():
            continue
        relative = path.relative_to(root).as_posix()
        parts = path.relative_to(root).parts
        if any(part in IGNORED_PARTS for part in parts):
            continue
        language = language_for_path(path)
        if not language:
            continue
        if not any(fnmatch.fnmatch(relative, pattern) for pattern in config["include"]):
            continue
        if any(fnmatch.fnmatch(relative, pattern) for pattern in config["exclude"]):
            continue
        paths.append(path)
    return sorted(paths)


def module_name(root: Path, path: Path) -> str | None:
    relative = path.relative_to(root).parts
    for folder in ("Sources", "Tests", "tests", "src"):
        if len(relative) > 2 and relative[0] == folder:
            return relative[1]
    return None


def scope_for_path(root: Path, path: Path) -> str:
    relative = path.relative_to(root).as_posix()
    config = project_config(root)
    if any(fnmatch.fnmatch(relative, pattern) for pattern in config["tool_paths"]):
        return "tool"
    if any(fnmatch.fnmatch(relative, pattern) for pattern in config["project_paths"]):
        return "project"
    return "unclassified"


def containing_type(node, types: set[str]):
    current = node
    while current is not None:
        if current.type in types:
            return current
        current = current.parent
    return None


def declaration_name_node(declaration):
    if declaration is None:
        return None
    name = declaration.child_by_field_name("name")
    if name is None:
        name = declaration.child_by_field_name("declarator")
        while name is not None:
            nested = name.child_by_field_name("declarator")
            if nested is None:
                nested = name.child_by_field_name("name")
            if nested is None:
                break
            name = nested
    return name


def is_declaration_name(node, language: str) -> bool:
    name = declaration_name_node(containing_type(node, DECLARATION_TYPES[language]))
    return name is not None and name.start_byte <= node.start_byte and node.end_byte <= name.end_byte


def declaration_kind(declaration) -> str:
    if declaration is None:
        return "symbol"
    for child in declaration.children:
        if child.type in {"class", "struct", "enum", "protocol", "interface", "actor", "trait", "namespace", "extension"}:
            return child.type
    for token, kind in (("constructor", "constructor"), ("initializer", "constructor"),
                        ("method", "method"), ("function", "function"), ("macro_def", "function"),
                        ("property", "property"), ("field", "property"), ("class", "class"),
                        ("struct", "struct"), ("enum", "enum"), ("type", "type"),
                        ("namespace", "namespace"), ("extension", "extension")):
        if token in declaration.type:
            return kind
    return "variable"


def qualified_symbol(node, language: str, raw: bytes) -> str:
    declaration = containing_type(node, DECLARATION_TYPES[language])
    owners = []
    current = declaration.parent if declaration is not None else None
    scope_kinds = {"class", "struct", "enum", "protocol", "interface", "actor", "trait",
                   "namespace", "extension", "function", "method", "constructor"}
    while current is not None:
        if (current.type in DECLARATION_TYPES[language]
                or current.type in {"namespace_definition", "namespace_declaration", "extension_declaration"}):
            name = declaration_name_node(current)
            if name is not None and declaration_kind(current) in scope_kinds:
                owners.append(node_text(name, raw))
        current = current.parent
    return ".".join([*reversed(owners), node_text(node, raw)])


def node_text(node, raw: bytes) -> str:
    return raw[node.start_byte:node.end_byte].decode("utf-8", errors="replace")


def import_names(node, identifiers, raw: bytes):
    for field in ("source", "path", "module", "name"):
        value = node.child_by_field_name(field)
        if value is not None:
            text = node_text(value, raw).strip().strip("\"'`")
            if text:
                return [(text, value)]
    descendants = [
        child for child in identifiers
        if node.start_byte <= child.start_byte and child.end_byte <= node.end_byte
    ]
    return [(node_text(child, raw), child) for child in descendants]


def callee_identifier(call, language: str):
    """Find a call target leaf, excluding receivers, arguments and type arguments."""
    target = None
    for field in ("function", "constructor", "name", "macro", "type"):
        target = call.child_by_field_name(field)
        if target is not None:
            break
    if target is None:
        target = next(iter(call.named_children), None)
    while target is not None:
        if target.type in IDENTIFIER_TYPES[language] and target.child_count == 0:
            return target
        if target.type in CALL_TYPES[language]:
            return None  # Calling a returned value has no directly named target.
        preferred = None
        for field in ("attribute", "property", "field", "name", "function", "constructor", "type"):
            preferred = target.child_by_field_name(field)
            if preferred is not None:
                break
        children = [child for child in target.named_children
                    if child.type not in {"type_arguments", "type_parameters", "arguments",
                                          "argument_list", "call_suffix"}]
        target = preferred if preferred is not None else (children[-1] if children else None)
    return None


def callable_declaration(node, language: str) -> bool:
    declaration = containing_type(node, DECLARATION_TYPES[language])
    return declaration is not None and any(part in declaration.type for part in
        ("function", "method", "constructor", "initializer", "class", "struct", "actor", "macro_def"))


def scan_file(root: Path, path: Path, include_references: bool = False,
              include_calls: bool = False) -> tuple[dict, bytes]:
    raw = path.read_bytes()
    relative = path.relative_to(root).as_posix()
    digest = hashlib.sha256(raw).hexdigest()
    language = language_for_path(path)
    try:
        raw.decode("utf-8")
        tree = get_parser(language).parse(raw)
    except (UnicodeDecodeError, LookupError, ValueError) as exc:
        return {
            "path": relative, "module": module_name(root, path), "language": language,
            "scope": scope_for_path(root, path), "sha256": digest,
            "imports": [], "definitions": [], "identifiers": [],
            "diagnostic": f"parse unavailable: {exc}",
        }, raw

    if tree.root_node.has_error:
        parse_diagnostic = "parser recovered from one or more syntax errors"
    else:
        parse_diagnostic = None

    imports, definitions, identifiers = [], [], []
    identifier_nodes = []
    stack = [tree.root_node]
    import_nodes = []
    call_nodes = []
    while stack:
        node = stack.pop()
        if (include_references or include_calls) and node.type in CALL_TYPES[language]:
            call_nodes.append(node)
        if node.type in IMPORT_TYPES[language]:
            import_nodes.append(node)
        if node.type in IDENTIFIER_TYPES[language] and node.child_count == 0:
            identifier_nodes.append(node)
        stack.extend(reversed(node.children))

    call_ranges = set()
    call_counts = Counter()
    for call in call_nodes:
        target = callee_identifier(call, language)
        if target is not None:
            call_ranges.add((target.start_byte, target.end_byte))
            call_counts[node_text(target, raw)] += 1

    imported_ranges = set()
    for import_node in import_nodes:
        for name, node in import_names(import_node, identifier_nodes, raw):
            imports.append({
                "name": name, "line": node.start_point.row + 1,
                "column": node.start_point.column + 1, "evidence": "ast:import",
            })
        imported_ranges.update(
            (node.start_byte, node.end_byte)
            for node in identifier_nodes
            if import_node.start_byte <= node.start_byte and node.end_byte <= import_node.end_byte
        )

    declaration_ranges = set()
    for node in identifier_nodes:
        if is_declaration_name(node, language):
            name = node_text(node, raw)
            definitions.append({
                "name": name, "line": node.start_point.row + 1,
                "column": node.start_point.column + 1, "evidence": "ast:declaration",
                "callable": callable_declaration(node, language),
                "qualified_name": qualified_symbol(node, language, raw),
                "kind": declaration_kind(containing_type(node, DECLARATION_TYPES[language])),
            })
            declaration_ranges.add((node.start_byte, node.end_byte))

    for node in (identifier_nodes if include_references else ()):
        identity = (node.start_byte, node.end_byte)
        if identity in imported_ranges or identity in declaration_ranges:
            continue
        name = node_text(node, raw)
        identifiers.append({
            "name": name, "line": node.start_point.row + 1,
            "column": node.start_point.column + 1,
            "is_call": identity in call_ranges,
            "evidence": "ast:identifier-reference",
        })
    return {
        "path": relative, "module": module_name(root, path), "language": language,
        "scope": scope_for_path(root, path), "sha256": digest,
        "imports": imports, "definitions": definitions, "identifiers": identifiers,
        "call_counts": dict(call_counts),
        **({"diagnostic": parse_diagnostic} if parse_diagnostic else {}),
    }, raw


def configuration_sha256(root: Path) -> str:
    digest = hashlib.sha256()
    candidates = set()
    for path in root.rglob("*"):
        if not path.is_file() or any(part in IGNORED_PARTS for part in path.relative_to(root).parts):
            continue
        if path.name in {
            "Package.swift", "project.pbxproj", "package.json", "tsconfig.json", "pyproject.toml",
            "Cargo.toml", "go.mod", "requirements.txt", "code-index.toml",
        } or path.suffix in {".xcconfig", ".xcscheme"}:
            candidates.add(path)
    config_path = root / "nightshift" / "code-index.toml"
    if not config_path.is_file():
        config_path = root / ".nightshift" / "code-index.toml"
    if config_path.is_file():
        candidates.add(config_path)
    for path in sorted(candidates):
        digest.update(path.relative_to(root).as_posix().encode() + b"\0")
        digest.update(hashlib.sha256(path.read_bytes()).digest())
    language_registry = TOOL_ROOT / "languages.json"
    digest.update(b"tool:languages.json\0")
    digest.update(hashlib.sha256(language_registry.read_bytes()).digest())
    requirements = TOOL_ROOT / "requirements.txt"
    digest.update(b"tool:requirements.txt\0")
    digest.update(hashlib.sha256(requirements.read_bytes()).digest())
    return digest.hexdigest()


def scan_repository(root: Path) -> tuple[list[dict], str, list[dict], str]:
    entries, diagnostics = [], []
    digest = hashlib.sha256()
    for path in source_files(root):
        entry, raw = scan_file(root, path)
        digest.update(entry["path"].encode() + b"\0" + raw + b"\0")
        if entry.get("diagnostic"):
            diagnostics.append({"path": entry["path"], "message": entry["diagnostic"]})
        entries.append(entry)
    return entries, digest.hexdigest(), diagnostics, configuration_sha256(root)


def generation_id(identity: dict) -> str:
    encoded = json.dumps(identity, sort_keys=True, separators=(",", ":")).encode()
    return hashlib.sha256(encoded).hexdigest()


def locked(path: Path, exclusive: bool = True):
    class Lock:
        def __enter__(self):
            lock_path = path.with_suffix(".lock")
            path.parent.mkdir(parents=True, exist_ok=True)
            self.handle = lock_path.open("a+b")
            if fcntl is not None:
                operation = fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH
                fcntl.flock(self.handle.fileno(), operation)
            else:
                self.handle.seek(0, os.SEEK_END)
                if self.handle.tell() == 0:
                    self.handle.write(b"\0")
                    self.handle.flush()
                self.handle.seek(0)
                # Windows byte-range locks are exclusive; reads remain safe and serialized.
                while True:
                    try:
                        msvcrt.locking(self.handle.fileno(), msvcrt.LK_NBLCK, 1)
                        break
                    except OSError:
                        time.sleep(0.1)
            return self

        def __exit__(self, exc_type, exc, traceback):
            if fcntl is not None:
                fcntl.flock(self.handle.fileno(), fcntl.LOCK_UN)
            else:
                self.handle.seek(0)
                msvcrt.locking(self.handle.fileno(), msvcrt.LK_UNLCK, 1)
            self.handle.close()
    return Lock()


def write_text_atomic(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary_name = tempfile.mkstemp(prefix="index-", suffix=".tmp", dir=path.parent)
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8") as output:
            output.write(text)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_name, path)
    finally:
        if os.path.exists(temporary_name):
            os.unlink(temporary_name)


def summary_text(data: dict) -> str:
    """Fit source paths first, then qualified symbol previews within the byte budget."""
    rows = []
    priority = {"project": 0, "tool": 1, "unclassified": 2}
    files = sorted(data["files"], key=lambda file: (priority.get(file["scope"], 3), file["path"]))
    def render():
        lines = []
        for file, selected in rows:
            symbols = "; ".join(f"{item['kind']} {item['qualified_name']}@{item['line']}" for item in selected)
            path = file["path"].replace("\n", "\\n")
            lines.append(f"[{file['scope']}] {path}" + (f": {symbols}" if symbols else "") + "\n")
        return (
            f"# Code index\nHEAD: {data['revision']}\nGeneration: {data['generation'][:12]}\n"
            "Syntax leads only; up to 3 definitions/file. Use code-index query for details.\n"
            f"Files: {len(rows)}/{len(files)}; omitted: {len(files) - len(rows)}\n\n"
            + "".join(lines)
        )
    for file in files:
        rows.append((file, []))
        if len(render().encode("utf-8")) > SUMMARY_MAX_BYTES:
            rows.pop()
    for position in range(3):
        for file, selected in rows:
            if position >= len(file["definitions"]):
                continue
            selected.append(file["definitions"][position])
            if len(render().encode("utf-8")) > SUMMARY_MAX_BYTES:
                selected.pop()
    return render()


def write_summary(path: Path, data: dict) -> None:
    text = summary_text(data)
    if path.is_file() and path.read_text(encoding="utf-8") == text:
        return
    write_text_atomic(path, text)


def write_index(path: Path, data: dict) -> None:
    write_summary(path.with_name("INDEX.md"), data)
    write_text_atomic(path, json.dumps(data, sort_keys=True, separators=(",", ":")) + "\n")


def build_data(root: Path) -> tuple[dict, int]:
    started = time.perf_counter_ns()
    revision_before = git_output(root, "rev-parse", "HEAD")
    entries, source_sha256, diagnostics, configuration_sha = scan_repository(root)
    identity = {
        "root": str(root), "revision": git_output(root, "rev-parse", "HEAD"),
        "source_sha256": source_sha256, "configuration_sha256": configuration_sha,
        "indexer_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        "parser_version": package_version("tree-sitter-language-pack"),
        "schema": SCHEMA_VERSION,
    }
    if identity["revision"] != revision_before:
        raise IndexErrorMessage("repository revision changed during index build; retry")
    _, verify_source, _, verify_configuration = scan_repository(root)
    if verify_source != source_sha256 or verify_configuration != configuration_sha:
        raise IndexErrorMessage("source or build settings changed during index build; retry")
    generation = generation_id(identity)
    for entry in entries:
        entry["definitions"] = sorted(entry["definitions"],
                                      key=lambda item: (not item.get("callable", False), item["line"]))[:3]
        entry.pop("imports", None)
        entry.pop("identifiers", None)
        entry.pop("call_counts", None)
        for definition in entry["definitions"]:
            definition.pop("callable", None)
    data = {
        **identity, "generation": generation, "files_seen": len(entries),
        "modules": sorted({entry["module"] for entry in entries if entry["module"]}),
        "coverage": "Tree-sitter syntax evidence across configured languages; references and callers are syntactic leads, not compiler-resolved bindings.",
        "languages": sorted({entry["language"] for entry in entries}),
        "files": entries, "diagnostics": diagnostics,
    }
    return data, time.perf_counter_ns() - started


def query_matches(data: dict, kind: str, term: str) -> list[dict]:
    matches = []
    call_sites = 0
    search_term = term.rsplit(".", 1)[-1] if kind == "symbol" else term
    for file in data["files"]:
        path = Path(data["root"]) / file["path"]
        if search_term.encode("utf-8") not in path.read_bytes():
            continue
        detailed, _ = scan_file(Path(data["root"]), path,
                                include_references=kind in {"references", "callers"},
                                include_calls=kind == "symbol")
        call_sites += detailed.get("call_counts", {}).get(search_term, 0)
        if detailed["sha256"] != file["sha256"]:
            raise IndexErrorMessage(f"source changed since index build: {file['path']}")
        if kind == "symbol":
            source_rows = detailed["definitions"]
        elif kind == "module":
            source_rows = detailed["imports"]
        elif kind in {"references", "callers"}:
            source_rows = detailed["identifiers"]
        else:
            raise IndexErrorMessage(f"unsupported query kind: {kind}")
        for item in source_rows:
            if item["name"] != term and not (kind == "symbol" and item.get("qualified_name") == term):
                continue
            if kind == "callers" and not item["is_call"]:
                continue
            matches.append({"path": file["path"], "module": file["module"],
                            "language": file["language"], "scope": file["scope"],
                            **item, "source_sha256": file["sha256"]})
    if kind == "symbol":
        for match in matches:
            if match.get("callable"):
                match["call_sites"] = call_sites
    return sorted(matches, key=lambda row: (row["path"], row["line"], row["name"]))


def cached_index(path: Path) -> dict | None:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
        return data if (isinstance(data, dict) and data.get("schema") == SCHEMA_VERSION
                        and isinstance(data.get("files"), list)) else None
    except (OSError, json.JSONDecodeError):
        return None


def indexed_worktree_changes(root: Path) -> list[str]:
    """List dirty paths that can affect the configured index, including deleted files."""
    result = subprocess.run(
        ["git", "-C", str(root), "status", "--porcelain=v1", "-z", "--untracked-files=all"],
        check=True, capture_output=True,
    )
    config = project_config(root)
    records = iter(result.stdout.split(b"\0"))
    changed = set()
    settings = {
        "Package.swift", "project.pbxproj", "package.json", "tsconfig.json", "pyproject.toml",
        "Cargo.toml", "go.mod", "requirements.txt", "code-index.toml",
    }
    for record in records:
        if not record:
            continue
        candidates = [os.fsdecode(record[3:])]
        if b"R" in record[:2] or b"C" in record[:2]:
            candidates.append(os.fsdecode(next(records)))
        for candidate in candidates:
            path = Path(candidate)
            if any(part in IGNORED_PARTS for part in path.parts):
                continue
            is_source = (
                language_for_path(path)
                and any(fnmatch.fnmatch(candidate, pattern) for pattern in config["include"])
                and not any(fnmatch.fnmatch(candidate, pattern) for pattern in config["exclude"])
            )
            if (is_source or path.name in settings
                    or path.suffix in {".xcconfig", ".xcscheme"}
                    or candidate in {"nightshift/tools/index.py", "nightshift/tools/languages.json"}):
                changed.add(candidate)
    return sorted(changed)


def require_clean_index_inputs(root: Path) -> None:
    changed = indexed_worktree_changes(root)
    if changed:
        raise IndexErrorMessage(
            "indexed inputs are dirty; index build skipped and existing index preserved: "
            + ", ".join(changed)
        )


def ensure_index(root: Path) -> tuple[dict, bool]:
    """Build only from clean inputs; reuse an existing index for this HEAD."""
    location = index_location(root)
    require_clean_index_inputs(root)
    revision = git_output(root, "rev-parse", "HEAD")
    cached = cached_index(location)
    if cached is not None and cached.get("root") == str(root) and cached.get("revision") == revision:
        write_summary(summary_location(root), cached)
        return cached, False
    with locked(location):
        require_clean_index_inputs(root)
        revision = git_output(root, "rev-parse", "HEAD")
        cached = cached_index(location)
        if cached is not None and cached.get("root") == str(root) and cached.get("revision") == revision:
            write_summary(summary_location(root), cached)
            return cached, False
        data, _ = build_data(root)
        require_clean_index_inputs(root)
        if git_output(root, "rev-parse", "HEAD") != data["revision"]:
            raise IndexErrorMessage("repository revision changed during index build; retry")
        write_index(location, data)
        return data, True


def run(args: argparse.Namespace) -> int:
    root = repository_root(args.repo)
    location = index_location(root)
    if args.command in {"build", "ensure"}:
        started = time.perf_counter_ns()
        data, rebuilt = ensure_index(root)
        print(json.dumps({
            "status": "rebuilt" if rebuilt else "current",
            "index_path": str(location), "summary_path": str(summary_location(root)),
            "summary_bytes": summary_location(root).stat().st_size,
            "index_bytes": location.stat().st_size, "generation": data["generation"],
            "revision": data["revision"],
            "source_sha256": data["source_sha256"], "files": data["files_seen"],
            "modules": data["modules"], "languages": data["languages"],
            "diagnostics": data["diagnostics"],
            "elapsed_ms": (time.perf_counter_ns() - started) / 1_000_000,
        }, sort_keys=True))
        return 0

    if args.limit < 0:
        raise IndexErrorMessage("--limit must be nonnegative")
    if args.kind not in {"symbol", "module", "references", "callers"}:
        raise IndexErrorMessage(f"unsupported query kind: {args.kind}")
    if args.kind in {"references", "callers"} and "." in args.term:
        raise IndexErrorMessage("reference/caller queries require a simple name; type ownership is unresolved")
    data, _ = ensure_index(root)
    matches = query_matches(data, args.kind, args.term)
    if args.scope != "all":
        matches = [row for row in matches if row["scope"] == args.scope]
    require_clean_index_inputs(root)
    if git_output(root, "rev-parse", "HEAD") != data["revision"]:
        raise IndexErrorMessage("repository revision changed during query; retry the query")
    omitted = max(0, len(matches) - args.limit)
    response = {
        "kind": args.kind, "term": args.term, "revision": data["revision"],
        "source_sha256": data["source_sha256"], "generation": data["generation"],
        "evidence": "syntactic; see per-match evidence and coverage",
        "coverage": data["coverage"], "diagnostics": data["diagnostics"],
        "unresolved": (
            "Compiler binding is unresolved. A match is a syntactic lead; same-name results "
        "do not prove semantic references, callers, or absence of callers."
        ) if args.kind in {"references", "callers"} else (
            "Call counts are same-name syntax candidates; 0 is not proof of unused. "
            "Dynamic dispatch and compiler bindings are unresolved." if args.kind == "symbol" else None
        ),
        "matches": matches[:args.limit], "truncated": omitted > 0, "omitted": omitted,
        "reference_files": len({row["path"] for row in matches}) if args.kind in {"references", "callers"} else None,
    }
    if args.output_format == "compact":
        lines = [
            f"index {data['generation'][:12]} revision {data['revision'][:12]} source {data['source_sha256'][:12]}",
            f"evidence: syntax-only; {response['unresolved'] or 'definitions/imports are not semantic binding'}",
            f"truncated: {str(omitted > 0).lower()} omitted: {omitted}",
            f"matches: {min(len(matches), args.limit)}/{len(matches)}",
        ]
        if response["reference_files"] is not None:
            lines.append(f"reference files: {response['reference_files']}")
        lines.extend(
            f"{row['path']}:{row['line']}:{row['column']} {(row['kind'] + ' ') if 'kind' in row else ''}{row.get('qualified_name', row['name'])} [{row['evidence']}; scope={row['scope']}]"
            + (f" calls={row['call_sites']}" if "call_sites" in row else "")
            for row in matches[:args.limit]
        )
        print("\n".join(lines))
        return 0
    print(json.dumps(response, sort_keys=True))
    return 0


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("--repo", required=True, type=Path, help="repository to index")
    commands = result.add_subparsers(dest="command", required=True)
    commands.add_parser("ensure", help="reuse a clean index for HEAD or rebuild it")
    commands.add_parser("build", help="build from clean indexed inputs unless the cached Git hash already matches HEAD")
    query = commands.add_parser("query", help="query definitions, modules, references, or callers")
    query.add_argument("kind", choices=("symbol", "module", "references", "callers"))
    query.add_argument("term", help="symbol/module name; symbol queries also accept owner-qualified names")
    query.add_argument("--limit", type=int, default=50, help="maximum returned matches")
    query.add_argument("--format", dest="output_format", choices=("json", "compact"), default="json")
    query.add_argument("--scope", choices=("all", "project", "tool", "unclassified"), default="all")
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        return run(args)
    except (IndexErrorMessage, OSError) as exc:
        print(json.dumps({"error": str(exc), "diagnostic": "index operation failed"}), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
