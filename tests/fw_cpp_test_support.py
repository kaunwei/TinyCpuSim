import contextlib
import dataclasses
import fcntl
import hashlib
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import time
from typing import Optional, Sequence, Set, Tuple


_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
_DEFAULT_CACHE_DIR = _REPO_ROOT / ".cache" / "cpp_builds"

# Standard library headers that do not depend on local repo or external files
_STD_HEADERS = {
    "algorithm", "any", "array", "atomic", "barrier", "bit", "bitset", "charconv",
    "chrono", "codecvt", "compare", "complex", "concepts", "condition_variable",
    "coroutine", "cassert", "cctype", "cerrno", "cfenv", "cfloat", "cinttypes",
    "ciso646", "climits", "clocale", "cmath", "csetjmp", "csignal", "cstdalign",
    "cstdarg", "cstdbool", "cstddef", "cstdint", "cstdio", "cstdlib", "cstring",
    "ctgmath", "ctime", "cuchar", "cwchar", "cwctype", "deque", "exception",
    "execution", "expected", "filesystem", "format", "forward_list", "fstream",
    "functional", "future", "generator", "initializer_list", "iomanip", "ios",
    "iosfwd", "iostream", "istream", "iterator", "latch", "limits", "list",
    "locale", "map", "mdspan", "memory", "memory_resource", "mutex", "new",
    "numbers", "numeric", "optional", "ostream", "print", "queue", "random",
    "ranges", "ratio", "regex", "scoped_allocator", "semaphore", "set",
    "shared_mutex", "source_location", "span", "spanstream", "sstream", "stack",
    "stacktrace", "stdexcept", "stdfloat", "stop_token", "streambuf", "string",
    "string_view", "syncstream", "system_error", "thread", "tuple", "type_traits",
    "typeindex", "typeinfo", "unordered_map", "unordered_set", "utility", "valarray",
    "variant", "vector", "version",
}


class CompileError(subprocess.CalledProcessError):
    """CalledProcessError variant with compiler stderr exposed in the message string."""

    def __init__(
        self,
        returncode: int,
        cmd: Sequence[str],
        output: Optional[str] = None,
        stderr: Optional[str] = None,
    ) -> None:
        super().__init__(returncode, cmd, output=output, stderr=stderr)

    def __str__(self) -> str:
        base = super().__str__()
        if self.stderr:
            return f"{base}\nCompiler stderr:\n{self.stderr.strip()}"
        return base


@dataclasses.dataclass(frozen=True)
class CppBuildConfig:
    compiler: str = "/usr/bin/c++"
    flags: Tuple[str, ...] = ("-std=c++20",)
    include_dirs: Tuple[str, ...] = (str(_REPO_ROOT / "include"),)
    cache_dir: Optional[pathlib.Path] = _DEFAULT_CACHE_DIR
    compile_timeout: float = 30.0
    lock_timeout: float = 60.0


def _get_compiler_identity(compiler_path: str) -> str:
    try:
        proc = subprocess.run(
            [compiler_path, "--version"],
            capture_output=True,
            text=True,
            timeout=10,
            check=True,
        )
        return proc.stdout.strip()
    except Exception:
        stat = os.stat(compiler_path) if os.path.exists(compiler_path) else None
        return f"{compiler_path}:{stat.st_mtime if stat else 'unknown'}"


def _find_included_headers(source: str) -> Set[str]:
    pattern = re.compile(r'^[ \t]*#[ \t]*include[ \t]+([<"][^>"]+[>"])', re.MULTILINE)
    return set(pattern.findall(source))


def _resolve_header_dependencies(
    headers: Set[str], include_dirs: Sequence[str]
) -> Tuple[bool, Sequence[pathlib.Path]]:
    """Resolves header paths.

    Returns (can_cache, resolved_paths).
    If any non-std header cannot be resolved in include_dirs, can_cache is False.
    """
    resolved: Set[pathlib.Path] = set()
    for h in headers:
        header_name = h[1:-1]
        if h.startswith("<") and header_name in _STD_HEADERS:
            continue
        found = False
        for inc_dir in include_dirs:
            p = pathlib.Path(inc_dir) / header_name
            if p.is_file():
                found = True
                _collect_transitive_headers(p, include_dirs, resolved)
                break
        if not found:
            return False, []
    return True, sorted(resolved)


def _collect_transitive_headers(
    header_path: pathlib.Path, include_dirs: Sequence[str], visited: Set[pathlib.Path]
) -> None:
    if header_path in visited:
        return
    visited.add(header_path)
    try:
        content = header_path.read_text(encoding="utf-8", errors="ignore")
    except Exception:
        return
    child_headers = _find_included_headers(content)
    for ch in child_headers:
        ch_name = ch[1:-1]
        if ch.startswith("<") and ch_name in _STD_HEADERS:
            continue
        # Check relative to current header directory first
        local_p = header_path.parent / ch_name
        if local_p.is_file():
            _collect_transitive_headers(local_p.resolve(), include_dirs, visited)
            continue
        # Then search include_dirs
        for inc_dir in include_dirs:
            p = pathlib.Path(inc_dir) / ch_name
            if p.is_file():
                _collect_transitive_headers(p.resolve(), include_dirs, visited)
                break


def _compute_cache_key(
    source: str,
    compiler_id: str,
    config: CppBuildConfig,
    header_paths: Sequence[pathlib.Path],
) -> str:
    h = hashlib.sha256()
    h.update(source.encode("utf-8"))
    h.update(b"\x00")
    h.update(compiler_id.encode("utf-8"))
    h.update(b"\x00")
    for flag in config.flags:
        h.update(flag.encode("utf-8"))
        h.update(b"\x00")
    for inc in config.include_dirs:
        h.update(str(pathlib.Path(inc).resolve()).encode("utf-8"))
        h.update(b"\x00")
    for hdr in header_paths:
        h.update(str(hdr.resolve()).encode("utf-8"))
        h.update(b"\x00")
        try:
            stat = hdr.stat()
            h.update(f"{stat.st_size}:{stat.st_mtime_ns}".encode("utf-8"))
        except OSError:
            h.update(b"missing")
        h.update(b"\x00")
    return h.hexdigest()


@contextlib.contextmanager
def _acquire_cross_process_lock(lock_file_path: pathlib.Path, timeout: float = 60.0):
    lock_file_path.parent.mkdir(parents=True, exist_ok=True)
    f = open(lock_file_path, "a+")
    deadline = time.time() + timeout
    acquired = False
    try:
        while time.time() < deadline:
            try:
                fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                acquired = True
                break
            except (BlockingIOError, OSError):
                time.sleep(0.02)
        if not acquired:
            raise TimeoutError(f"Timed out waiting for lock on {lock_file_path}")
        yield
    finally:
        if acquired:
            try:
                fcntl.flock(f.fileno(), fcntl.LOCK_UN)
            except OSError:
                pass
        f.close()


def _compile_to_binary(
    source: str,
    output_bin: pathlib.Path,
    config: CppBuildConfig,
) -> None:
    output_bin.parent.mkdir(parents=True, exist_ok=True)
    temp_bin = output_bin.parent / f"{output_bin.name}.tmp.{os.getpid()}.{time.time_ns()}"
    src_file = output_bin.parent / f"{output_bin.name}.tmp.{os.getpid()}.{time.time_ns()}.cpp"

    cmd = [config.compiler]
    cmd.extend(config.flags)
    for inc in config.include_dirs:
        cmd.append(f"-I{inc}")
    cmd.extend([str(src_file), "-o", str(temp_bin)])

    try:
        src_file.write_text(source, encoding="utf-8")
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=config.compile_timeout,
        )
        if proc.returncode != 0:
            raise CompileError(
                proc.returncode,
                cmd,
                output=proc.stdout,
                stderr=proc.stderr,
            )
        # Atomically publish to destination
        os.replace(temp_bin, output_bin)
    except subprocess.CalledProcessError as e:
        if not isinstance(e, CompileError):
            raise CompileError(e.returncode, e.cmd, output=e.output, stderr=e.stderr) from e
        raise
    finally:
        if src_file.exists():
            try:
                src_file.unlink()
            except OSError:
                pass
        if temp_bin.exists():
            try:
                temp_bin.unlink()
            except OSError:
                pass


def get_or_compile_cpp(
    source: str,
    config: Optional[CppBuildConfig] = None,
) -> Tuple[pathlib.Path, bool]:
    """Retrieve compiled binary from cache or compile it.

    Returns (binary_path, from_cache).
    """
    if config is None:
        config = CppBuildConfig()

    if config.cache_dir is None:
        # Cache explicitly bypassed
        tmp_dir = pathlib.Path(tempfile.mkdtemp(prefix="cpp_run_"))
        bin_file = tmp_dir / "app"
        _compile_to_binary(source, bin_file, config)
        return bin_file, False

    # Check header dependencies
    headers = _find_included_headers(source)
    can_cache, header_paths = _resolve_header_dependencies(headers, config.include_dirs)
    if not can_cache:
        # External or unresolvable header - safely bypass cache
        tmp_dir = pathlib.Path(tempfile.mkdtemp(prefix="cpp_run_"))
        bin_file = tmp_dir / "app"
        _compile_to_binary(source, bin_file, config)
        return bin_file, False

    compiler_id = _get_compiler_identity(config.compiler)
    cache_key = _compute_cache_key(source, compiler_id, config, header_paths)
    cache_dir = pathlib.Path(config.cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)

    bin_path = cache_dir / f"bin_{cache_key}"
    lock_path = cache_dir / f".lock_{cache_key}"

    if bin_path.exists():
        return bin_path, True

    # Acquire bounded cross-process lock for this specific key
    with _acquire_cross_process_lock(lock_path, timeout=config.lock_timeout):
        # Double check if another process finished compilation while we waited
        if bin_path.exists():
            return bin_path, True
        _compile_to_binary(source, bin_path, config)
        return bin_path, False


def run_cpp(
    source: str,
    timeout: float = 5,
    config: Optional[CppBuildConfig] = None,
) -> subprocess.CompletedProcess[str]:
    """Compile C++20 source and run the resulting binary under a timeout.

    Uses a dependency-correct compilation cache across runs with atomic
    publication and bounded same-key locking. Preserves subprocess.CompletedProcess
    return behavior, CalledProcessError compatibility, and TimeoutExpired behavior.
    """
    if config is None:
        config = CppBuildConfig()

    bin_file, _ = get_or_compile_cpp(source, config)

    return subprocess.run(
        [str(bin_file)],
        capture_output=True,
        text=True,
        timeout=timeout,
    )
