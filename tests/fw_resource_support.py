"""POSIX file-lock based resource-budget context manager for independent processes.

Enforces shared CPU slots and rounded memory reservations across processes
using kernel file locks (fcntl.flock) so stale PID bookkeeping is unnecessary.
"""

from __future__ import annotations

import contextlib
import fcntl
import math
import os
import pathlib
import time
from dataclasses import dataclass
from typing import Iterator, Optional


DEFAULT_MEMORY_QUANTUM_MIB: int = 64


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
        memory_quantum_mib: int = DEFAULT_MEMORY_QUANTUM_MIB,
    ) -> None:
        if memory_quantum_mib <= 0:
            raise ValueError(f"memory_quantum_mib must be positive, got {memory_quantum_mib}")
        self.quantum_mib = memory_quantum_mib
        self.root = pathlib.Path(directory).resolve()
        self.root.mkdir(parents=True, exist_ok=True)

        self._guard_path = self.root / ".alloc_guard"
        self._meta_path = self.root / "pool.meta"

        # Serialize setup with guard
        guard_fd = os.open(str(self._guard_path), os.O_CREAT | os.O_RDWR, 0o644)
        try:
            fcntl.flock(guard_fd, fcntl.LOCK_EX)
            if budget is not None:
                # Initializing or validating existing capacity
                if self._meta_path.exists():
                    existing_budget = self._read_budget()
                    if existing_budget != budget:
                        raise ValueError(
                            f"Capacity mismatch: existing pool has {existing_budget}, requested {budget}"
                        )
                else:
                    self._write_budget(budget)
                self.budget = budget
            else:
                if not self._meta_path.exists():
                    raise ValueError(f"No existing pool found in {self.root} and no budget provided")
                self.budget = self._read_budget()
        finally:
            try:
                fcntl.flock(guard_fd, fcntl.LOCK_UN)
            finally:
                os.close(guard_fd)

        self._cpu_dir = self.root / "cpu_slots"
        self._mem_dir = self.root / "mem_slots"
        self._cpu_dir.mkdir(parents=True, exist_ok=True)
        self._mem_dir.mkdir(parents=True, exist_ok=True)

        self.total_mem_quanta = math.ceil(self.budget.memory_mib / self.quantum_mib)

        # Ensure slot files exist
        for i in range(self.budget.cpu_slots):
            slot_file = self._cpu_dir / f"slot_{i}.lock"
            if not slot_file.exists():
                slot_file.touch(mode=0o644, exist_ok=True)

        for i in range(self.total_mem_quanta):
            slot_file = self._mem_dir / f"slot_{i}.lock"
            if not slot_file.exists():
                slot_file.touch(mode=0o644, exist_ok=True)

    def _read_budget(self) -> ResourceBudget:
        content = self._meta_path.read_text(encoding="utf-8").strip()
        lines = content.splitlines()
        meta: dict[str, int] = {}
        for line in lines:
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = int(v.strip())
        return ResourceBudget(
            cpu_slots=meta["cpu_slots"],
            memory_mib=meta["memory_mib"],
        )

    def _write_budget(self, budget: ResourceBudget) -> None:
        content = f"cpu_slots={budget.cpu_slots}\nmemory_mib={budget.memory_mib}\n"
        tmp = self._meta_path.with_suffix(".tmp")
        tmp.write_text(content, encoding="utf-8")
        tmp.replace(self._meta_path)

    @classmethod
    def open(
        cls,
        directory: str | os.PathLike[str],
        memory_quantum_mib: int = DEFAULT_MEMORY_QUANTUM_MIB,
    ) -> ResourcePool:
        """Reopen an existing pool from its directory without providing budget."""
        return cls(directory=directory, budget=None, memory_quantum_mib=memory_quantum_mib)

    @contextlib.contextmanager
    def reserve(
        self,
        cpu_slots: int = 1,
        memory_mib: int = 0,
        timeout: Optional[float] = None,
    ) -> Iterator[ResourceBudget]:
        """Reserve resources for the duration of the context block.

        Uses monotonic bounded waits, releases on all exceptions and process death.
        Never retains partial reservations while waiting, never busy-waits.
        """
        if cpu_slots < 0:
            raise ValueError(f"cpu_slots must be non-negative, got {cpu_slots}")
        if memory_mib < 0:
            raise ValueError(f"memory_mib must be non-negative, got {memory_mib}")
        if timeout is not None and timeout < 0:
            raise ValueError(f"timeout cannot be negative, got {timeout}")

        req_mem_quanta = math.ceil(memory_mib / self.quantum_mib) if memory_mib > 0 else 0

        # Reject oversized requests immediately
        if cpu_slots > self.budget.cpu_slots:
            raise ValueError(
                f"Requested cpu_slots ({cpu_slots}) exceeds pool capacity ({self.budget.cpu_slots})"
            )
        if req_mem_quanta > self.total_mem_quanta:
            raise ValueError(
                f"Requested memory_mib ({memory_mib} MiB -> {req_mem_quanta} quanta) exceeds "
                f"pool capacity ({self.budget.memory_mib} MiB -> {self.total_mem_quanta} quanta)"
            )

        start_time = time.monotonic()
        deadline = (start_time + timeout) if timeout is not None else None

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
                guard_timeout = None
                if deadline is not None:
                    guard_timeout = deadline - time.monotonic()
                    if guard_timeout <= 0:
                        raise TimeoutError("Timed out waiting to acquire resource allocation guard")

                # Acquire allocation guard (short serialization to avoid partial locks)
                guard_fd = os.open(str(self._guard_path), os.O_CREAT | os.O_RDWR, 0o644)
                guard_acquired = False
                try:
                    # Non-blocking lock attempt first, otherwise bounded retry
                    # Guard is held only for brief checking of available slots
                    # We can use a short poll or blocking lock with alarm/timeout.
                    # Since fcntl.flock has no timeout natively, we loop with small backoff
                    # or non-blocking tries bounded by remaining deadline.
                    while True:
                        try:
                            fcntl.flock(guard_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                            guard_acquired = True
                            break
                        except (BlockingIOError, OSError):
                            if deadline is not None and time.monotonic() >= deadline:
                                raise TimeoutError("Timed out acquiring allocation guard")
                            time.sleep(0.005)

                    # Inside guard: attempt all-or-nothing slot acquisition
                    attempt_fds: list[int] = []

                    # 1. Try acquire CPU slots
                    if cpu_slots > 0:
                        for i in range(self.budget.cpu_slots):
                            slot_path = self._cpu_dir / f"slot_{i}.lock"
                            slot_fd = os.open(str(slot_path), os.O_CREAT | os.O_RDWR, 0o644)
                            try:
                                fcntl.flock(slot_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                                attempt_fds.append(slot_fd)
                                if len(attempt_fds) == cpu_slots:
                                    break
                            except (BlockingIOError, OSError):
                                os.close(slot_fd)

                    cpu_acquired = len(attempt_fds) == cpu_slots

                    # 2. Try acquire memory quanta
                    mem_fds: list[int] = []
                    if cpu_acquired and req_mem_quanta > 0:
                        for i in range(self.total_mem_quanta):
                            slot_path = self._mem_dir / f"slot_{i}.lock"
                            slot_fd = os.open(str(slot_path), os.O_CREAT | os.O_RDWR, 0o644)
                            try:
                                fcntl.flock(slot_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                                mem_fds.append(slot_fd)
                                if len(mem_fds) == req_mem_quanta:
                                    break
                            except (BlockingIOError, OSError):
                                os.close(slot_fd)

                    mem_acquired = len(mem_fds) == req_mem_quanta

                    if cpu_acquired and mem_acquired:
                        # Success: retain all fds and break
                        acquired_fds = attempt_fds + mem_fds
                        break
                    else:
                        # All-or-nothing: release any partially acquired fds before releasing guard
                        _cleanup(attempt_fds)
                        _cleanup(mem_fds)

                finally:
                    if guard_acquired:
                        try:
                            fcntl.flock(guard_fd, fcntl.LOCK_UN)
                        except OSError:
                            pass
                    os.close(guard_fd)

                # Did not acquire all slots; check timeout
                if deadline is not None and time.monotonic() >= deadline:
                    raise TimeoutError(
                        f"Timed out after {timeout}s attempting to reserve "
                        f"cpu_slots={cpu_slots}, memory_mib={memory_mib}"
                    )

                # Wait before next attempt
                # Monotonic backoff interval without busy-spinning (5-20ms)
                now = time.monotonic()
                if deadline is not None:
                    remaining = deadline - now
                    if remaining <= 0:
                        raise TimeoutError(
                            f"Timed out after {timeout}s attempting to reserve "
                            f"cpu_slots={cpu_slots}, memory_mib={memory_mib}"
                        )
                    sleep_time = min(0.015, remaining)
                else:
                    sleep_time = 0.015
                time.sleep(sleep_time)

            # Yield reserved budget object
            yield ResourceBudget(
                cpu_slots=cpu_slots,
                memory_mib=req_mem_quanta * self.quantum_mib,
            )

        finally:
            # Release all held locks on block exit or exception
            _cleanup(acquired_fds)
