import contextlib
import dataclasses
import fcntl
import hashlib
import math
import os
import pathlib
import re
import shutil
import subprocess
import tempfile
import time
from typing import Iterator, Optional, Sequence, Set, Tuple

try:
    from tests.fw_resource_support import ResourcePool
except ImportError:
    try:
        from fw_resource_support import ResourcePool  # type: ignore
    except ImportError:
        import sys
        _root = str(pathlib.Path(__file__).resolve().parent.parent)
        _parent = str(pathlib.Path(__file__).resolve().parent)
        for _p in (_root, _parent):
            if _p not in sys.path:
                sys.path.insert(0, _p)
        try:
            from tests.fw_resource_support import ResourcePool
        except ImportError:
            from fw_resource_support import ResourcePool  # type: ignore


_REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
_DEFAULT_CACHE_DIR = _REPO_ROOT / ".cache" / "cpp_builds"

# Standard library headers reference (retained for backward compatibility)
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


DEFAULT_COMPILE_MEMORY_MIB: int = 1024
DEFAULT_EXEC_MEMORY_MIB: int = 256
DEFAULT_RESOURCE_TIMEOUT_SEC: float = 30.0


@dataclasses.dataclass(frozen=True)
class ResourcePolicy:
    pool: Optional[ResourcePool] = None
    compile_memory_mib: int = DEFAULT_COMPILE_MEMORY_MIB
    exec_memory_mib: int = DEFAULT_EXEC_MEMORY_MIB
    timeout: float = DEFAULT_RESOURCE_TIMEOUT_SEC

    @contextlib.contextmanager
    def reserve(self, cpu_slots: int, memory_mib: int) -> Iterator[None]:
        if self.pool is None:
            yield
        else:
            with self.pool.reserve(
                cpu_slots=cpu_slots,
                memory_mib=memory_mib,
                timeout=self.timeout,
            ):
                yield

    @contextlib.contextmanager
    def reserve_compile(self) -> Iterator[None]:
        with self.reserve(cpu_slots=1, memory_mib=self.compile_memory_mib):
            yield

    @contextlib.contextmanager
    def reserve_exec(self) -> Iterator[None]:
        with self.reserve(cpu_slots=1, memory_mib=self.exec_memory_mib):
            yield


def _parse_positive_int_env(name: str, default: int) -> int:
    val = os.environ.get(name)
    if val is None:
        return default
    val_str = val.strip()
    try:
        iv = int(val_str)
    except (ValueError, TypeError):
        raise ValueError(f"Invalid {name}: {val!r}. Must be a positive integer.")
    if iv <= 0:
        raise ValueError(f"Invalid {name}: {val!r}. Must be a positive integer.")
    return iv


def _parse_positive_float_env(name: str, default: float) -> float:
    val = os.environ.get(name)
    if val is None:
        return default
    val_str = val.strip()
    try:
        fv = float(val_str)
    except (ValueError, TypeError):
        raise ValueError(f"Invalid {name}: {val!r}. Must be a finite positive number.")
    if not math.isfinite(fv) or fv <= 0:
        raise ValueError(f"Invalid {name}: {val!r}. Must be a finite positive number.")
    return fv


def _resolve_resource_policy(policy: Optional[ResourcePolicy] = None) -> ResourcePolicy:
    if policy is not None:
        return policy

    compile_mem = _parse_positive_int_env("FW_CPP_COMPILE_MEMORY_MIB", DEFAULT_COMPILE_MEMORY_MIB)
    exec_mem = _parse_positive_int_env("FW_CPP_EXEC_MEMORY_MIB", DEFAULT_EXEC_MEMORY_MIB)
    timeout = _parse_positive_float_env("FW_TEST_RESOURCE_TIMEOUT", DEFAULT_RESOURCE_TIMEOUT_SEC)

    pool_env = os.environ.get("FW_TEST_RESOURCE_POOL")
    pool: Optional[ResourcePool] = None
    if pool_env is not None:
        if not pool_env.strip():
            raise ValueError(f"Invalid FW_TEST_RESOURCE_POOL: {pool_env!r}. Cannot be empty or whitespace.")
        pool_path = pathlib.Path(pool_env)
        if not pool_path.exists():
            raise ValueError(f"Invalid FW_TEST_RESOURCE_POOL: {pool_env!r} does not exist.")
        pool = ResourcePool.open(pool_path)

    return ResourcePolicy(
        pool=pool,
        compile_memory_mib=compile_mem,
        exec_memory_mib=exec_mem,
        timeout=timeout,
    )


def _resolve_build_config(config: Optional[CppBuildConfig] = None) -> CppBuildConfig:
    if config is not None:
        return config

    flags = ("-std=c++20",)
    opt = os.environ.get("FW_CPP_TEST_OPT")
    if opt is not None:
        if opt in ("O0", "O2"):
            flags = flags + (f"-{opt}",)
        else:
            raise ValueError(f"Invalid FW_CPP_TEST_OPT: {opt!r}. Expected 'O0' or 'O2'.")

    cache_dir: Optional[pathlib.Path] = _DEFAULT_CACHE_DIR
    cache_root = os.environ.get("FW_CPP_CACHE_ROOT")
    if cache_root is not None:
        if not cache_root.strip():
            raise ValueError("Invalid FW_CPP_CACHE_ROOT: cannot be empty or whitespace.")
        p = pathlib.Path(cache_root)
        if p.exists() and not p.is_dir():
            raise ValueError(f"Invalid FW_CPP_CACHE_ROOT: {cache_root!r} is not a directory.")
        cache_dir = p

    return dataclasses.replace(
        CppBuildConfig(),
        flags=flags,
        cache_dir=cache_dir,
    )


def _get_compiler_identity(
    compiler_path: str,
    policy: Optional[ResourcePolicy] = None,
) -> str:
    policy = _resolve_resource_policy(policy)
    p = pathlib.Path(shutil.which(compiler_path) or compiler_path)
    real_path = str(p.resolve()) if p.exists() else compiler_path
    content_hash = "missing"
    if p.exists():
        try:
            content_hash = hashlib.sha256(p.read_bytes()).hexdigest()
        except OSError:
            stat = p.stat()
            content_hash = f"stat:{stat.st_size}:{stat.st_mtime_ns}"
    version = ""
    with policy.reserve_compile():
        try:
            proc = subprocess.run(
                [compiler_path, "--version"],
                capture_output=True,
                text=True,
                timeout=10,
            )
            version = proc.stdout.strip()
        except Exception:
            version = "unknown"
    return f"path:{compiler_path}|real:{real_path}|hash:{content_hash}|ver:{version}"


def _get_compiler_dependencies(
    source: str,
    config: CppBuildConfig,
    policy: Optional[ResourcePolicy] = None,
) -> Tuple[bool, Sequence[pathlib.Path]]:
    """Query compiler for actual dependencies using -M.

    Returns (can_cache, resolved_header_paths).
    If dependency extraction fails (e.g. syntax error or missing header),
    returns (False, []).
    """
    policy = _resolve_resource_policy(policy)
    cmd = [config.compiler]
    cmd.extend(config.flags)
    for inc in config.include_dirs:
        cmd.append(f"-I{inc}")
    cmd.extend(["-M", "-x", "c++", "-"])

    with policy.reserve_compile():
        try:
            proc = subprocess.run(
                cmd,
                input=source,
                capture_output=True,
                text=True,
                timeout=config.compile_timeout,
            )
            if proc.returncode != 0:
                return False, []
        except Exception:
            return False, []

    # Parse makefile dependency output
    raw = proc.stdout
    colon_idx = raw.find(":")
    body = raw[colon_idx + 1 :] if colon_idx != -1 else raw
    body = body.replace("\\\n", " ").replace("\\\r\n", " ")

    tokens = []
    tok = []
    in_esc = False
    for ch in body:
        if in_esc:
            tok.append(ch)
            in_esc = False
        elif ch == "\\":
            in_esc = True
        elif ch in " \t\n\r":
            if tok:
                tokens.append("".join(tok))
                tok = []
        else:
            tok.append(ch)
    if tok:
        tokens.append("".join(tok))

    deps: Set[pathlib.Path] = set()
    for token in tokens:
        p = pathlib.Path(token).resolve()
        if p.is_file():
            deps.add(p)
        else:
            # Cannot prove missing dependency path
            return False, []
    return True, sorted(deps)


def _compute_cache_key(
    source: str,
    compiler_id: str,
    config: CppBuildConfig,
    header_paths: Sequence[pathlib.Path],
) -> str:
    h = hashlib.sha256()
    h.update(b"v2_proven_dep_cache\x00")
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
            h.update(hashlib.sha256(hdr.read_bytes()).digest())
        except OSError:
            return ""  # Unreadable dependency cannot be safely cached
        h.update(b"\x00")
    return h.hexdigest()


@contextlib.contextmanager
def _acquire_cross_process_lock(lock_file_path: pathlib.Path, timeout: float = 60.0):
    lock_file_path.parent.mkdir(parents=True, exist_ok=True)
    f = open(lock_file_path, "a+")
    deadline = time.monotonic() + timeout
    acquired = False
    try:
        while time.monotonic() < deadline:
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
    policy: Optional[ResourcePolicy] = None,
) -> None:
    policy = _resolve_resource_policy(policy)
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
        with policy.reserve_compile():
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


@contextlib.contextmanager
def get_or_compile_cpp(
    source: str,
    config: Optional[CppBuildConfig] = None,
    policy: Optional[ResourcePolicy] = None,
) -> Iterator[Tuple[pathlib.Path, bool]]:
    """Own a build during a with-block; yield (binary_path, from_cache).

    Bypassed builds are deleted on every context exit, including execution errors.
    Persistent cache entries survive exit even when from_cache is False (cold build).
    The returned binary is guaranteed available only within this context.
    """
    config = _resolve_build_config(config)
    policy = _resolve_resource_policy(policy)

    can_cache = False
    header_paths = []
    if config.cache_dir is not None:
        can_cache, header_paths = _get_compiler_dependencies(source, config, policy=policy)

    if not can_cache:
        with tempfile.TemporaryDirectory(prefix="cpp_run_") as directory:
            binary = pathlib.Path(directory) / "app"
            _compile_to_binary(source, binary, config, policy=policy)
            yield binary, False
        return

    compiler_id = _get_compiler_identity(config.compiler, policy=policy)
    cache_key = _compute_cache_key(source, compiler_id, config, header_paths)
    if not cache_key:
        with tempfile.TemporaryDirectory(prefix="cpp_run_") as directory:
            binary = pathlib.Path(directory) / "app"
            _compile_to_binary(source, binary, config, policy=policy)
            yield binary, False
        return

    cache_dir = pathlib.Path(config.cache_dir)
    cache_dir.mkdir(parents=True, exist_ok=True)

    binary = cache_dir / f"bin_{cache_key}"
    lock_path = cache_dir / f".lock_{cache_key}"

    from_cache = binary.exists()
    if not from_cache:
        with _acquire_cross_process_lock(lock_path, timeout=config.lock_timeout):
            from_cache = binary.exists()
            if not from_cache:
                _compile_to_binary(source, binary, config, policy=policy)
    # Release the compile lock before exposing an executable to the caller.
    yield binary, from_cache


def run_cpp(
    source: str,
    timeout: float = 5,
    config: Optional[CppBuildConfig] = None,
    policy: Optional[ResourcePolicy] = None,
) -> subprocess.CompletedProcess[str]:
    """Compile C++20 source and run the resulting binary under a timeout.

    Uses a dependency-correct compilation cache across runs with atomic
    publication and bounded same-key locking. Preserves subprocess.CompletedProcess
    return behavior, CalledProcessError compatibility, and TimeoutExpired behavior.
    """
    config = _resolve_build_config(config)
    policy = _resolve_resource_policy(policy)

    with get_or_compile_cpp(source, config, policy=policy) as (bin_file, _):
        with policy.reserve_exec():
            return subprocess.run(
                [str(bin_file)],
                capture_output=True,
                text=True,
                timeout=timeout,
            )
