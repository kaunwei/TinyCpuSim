"""POSIX file-lock based resource-budget context manager for independent processes.

Enforces shared CPU slots and rounded memory reservations across processes
using kernel file locks (fcntl.flock) so stale PID bookkeeping is unnecessary.
"""

from __future__ import annotations

import contextlib
import errno
import fcntl
import math
import os
import pathlib
import time
from dataclasses import dataclass
from typing import Iterator, Optional


DEFAULT_MEMORY_QUANTUM_MIB: int = 64
DEFAULT_GUARD_TIMEOUT_SEC: float = 10.0
DEFAULT_RESERVE_TIMEOUT_SEC: float = 30.0


@dataclass(frozen=True)
class ResourceBudget:
    """Represents a total resource pool or reservation request."""
    cpu_slots: int
    memory_mib: int

    def __post_init__(self) -> None:
        if self.cpu_slots < 0:
            raise ValueError(f"cpu_slots must be non-negative, got {self.cpu_slots}")
        if self.memory_mib < 0:
            raise ValueError(f"memory_mib must be non-negative, got {self.memory_mib}")


class ResourcePool:
    """Manages shared CPU and memory slot locks backed by a directory."""

    def __init__(
        self,
        directory: str | os.PathLike[str],
        budget: Optional[ResourceBudget] = None,
        memory_quantum_mib: Optional[int] = None,
        guard_timeout: float = DEFAULT_GUARD_TIMEOUT_SEC,
    ) -> None:
        if memory_quantum_mib is not None and memory_quantum_mib <= 0:
            raise ValueError(f"memory_quantum_mib must be positive, got {memory_quantum_mib}")
        if guard_timeout <= 0:
            raise ValueError(f"guard_timeout must be positive, got {guard_timeout}")

        self.root = pathlib.Path(directory).resolve()
        self.root.mkdir(parents=True, exist_ok=True)

        self._guard_path = self.root / ".alloc_guard"
        self._meta_path = self.root / "pool.meta"

        # Serialize setup with bounded guard
        guard_fd = os.open(str(self._guard_path), os.O_CREAT | os.O_RDWR, 0o644)
        guard_acquired = False
        guard_deadline = time.monotonic() + guard_timeout
        try:
            while True:
                try:
                    fcntl.flock(guard_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    guard_acquired = True
                    break
                except (BlockingIOError, OSError) as e:
                    if isinstance(e, OSError) and e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                        raise
                    if time.monotonic() >= guard_deadline:
                        raise TimeoutError(
                            f"Timed out after {guard_timeout}s acquiring pool allocation guard in constructor"
                        )
                    time.sleep(0.005)

            if self._meta_path.exists():
                stored_budget, stored_quantum = self._read_meta()
                if budget is not None and stored_budget != budget:
                    raise ValueError(
                        f"Capacity mismatch: existing pool has {stored_budget}, requested {budget}"
                    )
                if memory_quantum_mib is not None and memory_quantum_mib != stored_quantum:
                    raise ValueError(
                        f"Memory quantum mismatch: existing pool has {stored_quantum} MiB, requested {memory_quantum_mib} MiB"
                    )
                self.budget = stored_budget
                self.quantum_mib = stored_quantum
            else:
                if budget is None:
                    raise ValueError(f"No existing pool found in {self.root} and no budget provided")
                self.quantum_mib = (
                    memory_quantum_mib if memory_quantum_mib is not None else DEFAULT_MEMORY_QUANTUM_MIB
                )
                self.budget = budget
                self._write_meta(self.budget, self.quantum_mib)
        finally:
            try:
                if guard_acquired:
                    fcntl.flock(guard_fd, fcntl.LOCK_UN)
            finally:
                os.close(guard_fd)

        self._cpu_dir = self.root / "cpu_slots"
        self._mem_dir = self.root / "mem_slots"
        self._cpu_dir.mkdir(parents=True, exist_ok=True)
        self._mem_dir.mkdir(parents=True, exist_ok=True)

        self.total_mem_quanta = self.budget.memory_mib // self.quantum_mib
        self.usable_memory_mib = self.total_mem_quanta * self.quantum_mib

        # Ensure slot files exist
        for i in range(self.budget.cpu_slots):
            slot_file = self._cpu_dir / f"slot_{i}.lock"
            if not slot_file.exists():
                slot_file.touch(mode=0o644, exist_ok=True)

        for i in range(self.total_mem_quanta):
            slot_file = self._mem_dir / f"slot_{i}.lock"
            if not slot_file.exists():
                slot_file.touch(mode=0o644, exist_ok=True)

    def _read_meta(self) -> tuple[ResourceBudget, int]:
        content = self._meta_path.read_text(encoding="utf-8").strip()
        lines = content.splitlines()
        meta: dict[str, int] = {}
        for line in lines:
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = int(v.strip())
        budget = ResourceBudget(
            cpu_slots=meta["cpu_slots"],
            memory_mib=meta["memory_mib"],
        )
        quantum = meta.get("memory_quantum_mib", meta.get("quantum_mib", DEFAULT_MEMORY_QUANTUM_MIB))
        return budget, quantum

    def _read_budget(self) -> ResourceBudget:
        budget, _ = self._read_meta()
        return budget

    def _write_meta(self, budget: ResourceBudget, quantum_mib: int) -> None:
        content = (
            f"cpu_slots={budget.cpu_slots}\n"
            f"memory_mib={budget.memory_mib}\n"
            f"memory_quantum_mib={quantum_mib}\n"
        )
        tmp = self._meta_path.with_suffix(".tmp")
        tmp.write_text(content, encoding="utf-8")
        tmp.replace(self._meta_path)

    def _write_budget(self, budget: ResourceBudget) -> None:
        self._write_meta(budget, self.quantum_mib)

    @classmethod
    def open(
        cls,
        directory: str | os.PathLike[str],
        memory_quantum_mib: Optional[int] = None,
        guard_timeout: float = DEFAULT_GUARD_TIMEOUT_SEC,
    ) -> ResourcePool:
        """Reopen an existing pool from its directory without providing budget."""
        return cls(
            directory=directory,
            budget=None,
            memory_quantum_mib=memory_quantum_mib,
            guard_timeout=guard_timeout,
        )

    @contextlib.contextmanager
    def reserve(
        self,
        cpu_slots: int = 1,
        memory_mib: int = 0,
        timeout: Optional[float] = DEFAULT_RESERVE_TIMEOUT_SEC,
    ) -> Iterator[ResourceBudget]:
        """Reserve resources for the duration of the context block.

        Uses monotonic bounded waits, releases on all exceptions and process death.
        Never retains partial reservations while waiting, never busy-waits.
        If timeout is None, defaults to DEFAULT_RESERVE_TIMEOUT_SEC (30.0s).
        """
        if cpu_slots < 0:
            raise ValueError(f"cpu_slots must be non-negative, got {cpu_slots}")
        if memory_mib < 0:
            raise ValueError(f"memory_mib must be non-negative, got {memory_mib}")
        if timeout is None:
            timeout = DEFAULT_RESERVE_TIMEOUT_SEC
        if timeout < 0:
            raise ValueError(f"timeout cannot be negative, got {timeout}")

        req_mem_quanta = math.ceil(memory_mib / self.quantum_mib) if memory_mib > 0 else 0

        # Reject oversized requests immediately
        if cpu_slots > self.budget.cpu_slots:
            raise ValueError(
                f"Requested cpu_slots ({cpu_slots}) exceeds pool capacity ({self.budget.cpu_slots})"
            )
        if req_mem_quanta > self.total_mem_quanta or memory_mib > self.budget.memory_mib:
            raise ValueError(
                f"Requested memory_mib ({memory_mib} MiB -> {req_mem_quanta} quanta) exceeds "
                f"usable pool capacity ({self.usable_memory_mib} MiB -> {self.total_mem_quanta} quanta, ceiling {self.budget.memory_mib} MiB)"
            )

        start_time = time.monotonic()
        deadline = start_time + timeout

        acquired_fds: list[int] = []

        def _cleanup(fds: list[int]) -> None:
            for fd in fds:
                try:
                    fcntl.flock(fd, fcntl.LOCK_UN)
                except OSError:
                    pass
                try:
                    os.close(fd)
                except OSError:
                    pass

        try:
            while True:
                # Calculate time left for guard acquisition
                guard_timeout = deadline - time.monotonic()
                if guard_timeout <= 0:
                    raise TimeoutError(
                        f"Timed out after {timeout}s attempting to reserve "
                        f"cpu_slots={cpu_slots}, memory_mib={memory_mib}"
                    )

                # Acquire allocation guard (short serialization to avoid partial locks)
                guard_fd = os.open(str(self._guard_path), os.O_CREAT | os.O_RDWR, 0o644)
                guard_acquired = False
                try:
                    while True:
                        try:
                            fcntl.flock(guard_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                            guard_acquired = True
                            break
                        except (BlockingIOError, OSError) as e:
                            if isinstance(e, OSError) and e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                                raise
                            if time.monotonic() >= deadline:
                                raise TimeoutError(
                                    f"Timed out after {timeout}s attempting to reserve "
                                    f"cpu_slots={cpu_slots}, memory_mib={memory_mib}"
                                )
                            time.sleep(0.005)

                    # Inside guard: attempt all-or-nothing slot acquisition
                    attempt_fds: list[int] = []
                    try:
                        # 1. Try acquire CPU slots
                        cpu_slots_acquired = 0
                        if cpu_slots > 0:
                            for i in range(self.budget.cpu_slots):
                                slot_path = self._cpu_dir / f"slot_{i}.lock"
                                slot_fd = os.open(str(slot_path), os.O_CREAT | os.O_RDWR, 0o644)
                                try:
                                    fcntl.flock(slot_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                                    attempt_fds.append(slot_fd)
                                    cpu_slots_acquired += 1
                                    if cpu_slots_acquired == cpu_slots:
                                        break
                                except (BlockingIOError, OSError) as e:
                                    os.close(slot_fd)
                                    if isinstance(e, OSError) and e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                                        raise
                                except BaseException:
                                    os.close(slot_fd)
                                    raise

                        cpu_acquired = cpu_slots_acquired == cpu_slots

                        # 2. Try acquire memory quanta
                        mem_quanta_acquired = 0
                        if cpu_acquired and req_mem_quanta > 0:
                            for i in range(self.total_mem_quanta):
                                slot_path = self._mem_dir / f"slot_{i}.lock"
                                slot_fd = os.open(str(slot_path), os.O_CREAT | os.O_RDWR, 0o644)
                                try:
                                    fcntl.flock(slot_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                                    attempt_fds.append(slot_fd)
                                    mem_quanta_acquired += 1
                                    if mem_quanta_acquired == req_mem_quanta:
                                        break
                                except (BlockingIOError, OSError) as e:
                                    os.close(slot_fd)
                                    if isinstance(e, OSError) and e.errno not in (errno.EAGAIN, errno.EWOULDBLOCK):
                                        raise
                                except BaseException:
                                    os.close(slot_fd)
                                    raise

                        mem_acquired = mem_quanta_acquired == req_mem_quanta

                        if cpu_acquired and mem_acquired:
                            # Success: retain all fds and break
                            acquired_fds = attempt_fds
                            attempt_fds = []
                            break
                        else:
                            # All-or-nothing: release any partially acquired fds before releasing guard
                            _cleanup(attempt_fds)
                            attempt_fds = []
                    except BaseException:
                        _cleanup(attempt_fds)
                        attempt_fds = []
                        raise
                finally:
                    if guard_acquired:
                        try:
                            fcntl.flock(guard_fd, fcntl.LOCK_UN)
                        except OSError:
                            pass
                    os.close(guard_fd)

                # Did not acquire all slots; check timeout
                now = time.monotonic()
                if now >= deadline:
                    raise TimeoutError(
                        f"Timed out after {timeout}s attempting to reserve "
                        f"cpu_slots={cpu_slots}, memory_mib={memory_mib}"
                    )

                # Wait before next attempt
                remaining = deadline - now
                time.sleep(min(0.015, remaining))

            # Yield reserved budget object
            yield ResourceBudget(
                cpu_slots=cpu_slots,
                memory_mib=req_mem_quanta * self.quantum_mib,
            )

        finally:
            # Release all held locks on block exit or exception
            _cleanup(acquired_fds)
