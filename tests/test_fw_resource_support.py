"""Unit tests for fw_resource_support."""

from __future__ import annotations

import fcntl
import multiprocessing as mp
import os
import pathlib
import shutil
import tempfile
import time
import unittest

from tests.fw_resource_support import (
    DEFAULT_GUARD_TIMEOUT_SEC,
    DEFAULT_MEMORY_QUANTUM_MIB,
    DEFAULT_RESERVE_TIMEOUT_SEC,
    ResourceBudget,
    ResourcePool,
)


def _worker_reserve_and_hold(pool_dir: str, cpu_slots: int, mem_mib: int, hold_sec: float, ready_event, release_event=None):
    pool = ResourcePool.open(pool_dir)
    with pool.reserve(cpu_slots=cpu_slots, memory_mib=mem_mib):
        ready_event.set()
        if release_event:
            release_event.wait(timeout=10.0)
        else:
            time.sleep(hold_sec)


def _worker_die_holding(pool_dir: str, cpu_slots: int, mem_mib: int, ready_event):
    pool = ResourcePool.open(pool_dir)
    # Enter context and forcibly terminate without normal context exit
    ctx = pool.reserve(cpu_slots=cpu_slots, memory_mib=mem_mib)
    ctx.__enter__()
    ready_event.set()
    # Hard exit simulation via os._exit
    os._exit(0)


def _worker_hold_guard(pool_dir: str, ready_event, release_event):
    guard_path = os.path.join(pool_dir, ".alloc_guard")
    guard_fd = os.open(guard_path, os.O_CREAT | os.O_RDWR, 0o644)
    fcntl.flock(guard_fd, fcntl.LOCK_EX)
    try:
        ready_event.set()
        release_event.wait(timeout=10.0)
    finally:
        try:
            fcntl.flock(guard_fd, fcntl.LOCK_UN)
        finally:
            os.close(guard_fd)


def _worker_attempt_incompatible_reopen(pool_dir: str, quantum_mib: int, queue: mp.Queue):
    try:
        ResourcePool.open(pool_dir, memory_quantum_mib=quantum_mib)
        queue.put("SUCCESS")
    except ValueError as e:
        queue.put(f"ValueError: {e}")
    except Exception as e:
        queue.put(f"{type(e).__name__}: {e}")


def _worker_reserve_is_a_directory(pool_dir: str, cpu_slots: int, mem_mib: int, queue: mp.Queue):
    try:
        pool = ResourcePool.open(pool_dir)
        with pool.reserve(cpu_slots=cpu_slots, memory_mib=mem_mib, timeout=0.5):
            pass
        queue.put("SUCCESS")
    except IsADirectoryError as e:
        queue.put(f"IsADirectoryError: {e}")
    except Exception as e:
        queue.put(f"{type(e).__name__}: {e}")


class TestFwResourceSupport(unittest.TestCase):

    def setUp(self):
        self.test_dir = tempfile.mkdtemp(prefix="res_pool_test_")

    def tearDown(self):
        shutil.rmtree(self.test_dir, ignore_errors=True)

    def test_invalid_parameters_and_mismatched_capacity(self):
        with self.assertRaises(ValueError):
            ResourceBudget(cpu_slots=-1, memory_mib=100)
        with self.assertRaises(ValueError):
            ResourceBudget(cpu_slots=2, memory_mib=-50)

        budget = ResourceBudget(cpu_slots=4, memory_mib=512)
        ResourcePool(self.test_dir, budget=budget)

        # Mismatched capacity reopen with different budget
        with self.assertRaises(ValueError):
            ResourcePool(self.test_dir, budget=ResourceBudget(cpu_slots=2, memory_mib=512))

        # Reopen non-existent pool without budget
        empty_dir = tempfile.mkdtemp(prefix="empty_pool_")
        try:
            with self.assertRaises(ValueError):
                ResourcePool.open(empty_dir)
        finally:
            shutil.rmtree(empty_dir, ignore_errors=True)

    def test_reopen_existing_pool(self):
        budget = ResourceBudget(cpu_slots=4, memory_mib=256)
        p1 = ResourcePool(self.test_dir, budget=budget)
        self.assertEqual(p1.budget, budget)

        # Reopen with .open()
        p2 = ResourcePool.open(self.test_dir)
        self.assertEqual(p2.budget, budget)
        self.assertEqual(p2.total_mem_quanta, 4)

    def test_oversized_reservation_rejected(self):
        budget = ResourceBudget(cpu_slots=2, memory_mib=128)
        pool = ResourcePool(self.test_dir, budget=budget)

        with self.assertRaises(ValueError):
            with pool.reserve(cpu_slots=3, memory_mib=64):
                pass

        with self.assertRaises(ValueError):
            with pool.reserve(cpu_slots=1, memory_mib=256):
                pass

    def test_cpu_binding_across_processes(self):
        budget = ResourceBudget(cpu_slots=2, memory_mib=1024)
        pool = ResourcePool(self.test_dir, budget=budget)

        ready = mp.Event()
        rel = mp.Event()
        # Reserve both CPU slots in worker process
        p = mp.Process(target=_worker_reserve_and_hold, args=(self.test_dir, 2, 0, 5.0, ready, rel))
        p.start()
        try:
            self.assertTrue(ready.wait(timeout=5.0))
            # Main process should not be able to acquire even 1 CPU slot
            start = time.monotonic()
            with self.assertRaises(TimeoutError):
                with pool.reserve(cpu_slots=1, memory_mib=0, timeout=0.1):
                    pass
            self.assertGreaterEqual(time.monotonic() - start, 0.09)

            # Signal worker to release
            rel.set()
            p.join(timeout=5.0)

            # Now main process can acquire both
            with pool.reserve(cpu_slots=2, memory_mib=0, timeout=1.0) as res:
                self.assertEqual(res.cpu_slots, 2)
        finally:
            rel.set()
            if p.is_alive():
                p.terminate()
                p.join()

    def test_memory_binding_with_spare_cpus(self):
        # 4 CPUs, 128 MiB (2 quanta of 64 MiB)
        budget = ResourceBudget(cpu_slots=4, memory_mib=128)
        pool = ResourcePool(self.test_dir, budget=budget)

        ready = mp.Event()
        rel = mp.Event()
        # Worker takes 1 CPU, all 128 MiB memory
        p = mp.Process(target=_worker_reserve_and_hold, args=(self.test_dir, 1, 128, 5.0, ready, rel))
        p.start()
        try:
            self.assertTrue(ready.wait(timeout=5.0))
            # There are 3 spare CPUs, but 0 spare memory
            # Trying to acquire 1 CPU with 64 MiB memory should time out
            with self.assertRaises(TimeoutError):
                with pool.reserve(cpu_slots=1, memory_mib=64, timeout=0.1):
                    pass

            # But reserving 2 CPUs with 0 memory should succeed immediately!
            with pool.reserve(cpu_slots=2, memory_mib=0, timeout=0.5) as res:
                self.assertEqual(res.cpu_slots, 2)
                self.assertEqual(res.memory_mib, 0)

            rel.set()
            p.join(timeout=5.0)
        finally:
            rel.set()
            if p.is_alive():
                p.terminate()
                p.join()

    def test_timeout_without_partial_leakage(self):
        # 2 CPUs, 128 MiB
        budget = ResourceBudget(cpu_slots=2, memory_mib=128)
        pool = ResourcePool(self.test_dir, budget=budget)

        ready = mp.Event()
        rel = mp.Event()
        # Worker holds 1 CPU and all 128 MiB memory
        p = mp.Process(target=_worker_reserve_and_hold, args=(self.test_dir, 1, 128, 5.0, ready, rel))
        p.start()
        try:
            self.assertTrue(ready.wait(timeout=5.0))

            # Main attempts to acquire 1 CPU and 64 MiB memory -> times out because memory is full.
            # Must NOT leak the 1 CPU slot it might have briefly examined!
            with self.assertRaises(TimeoutError):
                with pool.reserve(cpu_slots=1, memory_mib=64, timeout=0.1):
                    pass

            # Verify the 1 spare CPU is still available
            with pool.reserve(cpu_slots=1, memory_mib=0, timeout=0.1) as res:
                self.assertEqual(res.cpu_slots, 1)

            rel.set()
            p.join(timeout=5.0)
        finally:
            rel.set()
            if p.is_alive():
                p.terminate()
                p.join()

    def test_exception_release(self):
        budget = ResourceBudget(cpu_slots=2, memory_mib=128)
        pool = ResourcePool(self.test_dir, budget=budget)

        class CustomErr(Exception):
            pass

        try:
            with pool.reserve(cpu_slots=2, memory_mib=128):
                raise CustomErr("boom")
        except CustomErr:
            pass

        # All resources must be immediately re-acquirable
        with pool.reserve(cpu_slots=2, memory_mib=128, timeout=0.1) as res:
            self.assertEqual(res.cpu_slots, 2)
            self.assertEqual(res.memory_mib, 128)

    def test_terminated_owner_recovery(self):
        budget = ResourceBudget(cpu_slots=2, memory_mib=128)
        pool = ResourcePool(self.test_dir, budget=budget)

        ready = mp.Event()
        p = mp.Process(target=_worker_die_holding, args=(self.test_dir, 2, 128, ready))
        p.start()
        try:
            self.assertTrue(ready.wait(timeout=5.0))
            p.join(timeout=5.0)
            self.assertFalse(p.is_alive())

            # Because kernel file locks (flock) are tied to open file table descriptions,
            # process death automatically releases them. No stale PID bookkeeping required.
            with pool.reserve(cpu_slots=2, memory_mib=128, timeout=0.5) as res:
                self.assertEqual(res.cpu_slots, 2)
                self.assertEqual(res.memory_mib, 128)
        finally:
            if p.is_alive():
                p.kill()
                p.join()

    def test_floored_usable_memory_capacity_and_rounded_rejection(self):
        # 100 MiB budget with default 64 MiB quantum must floor usable capacity to 1 quantum (64 MiB)
        budget = ResourceBudget(cpu_slots=2, memory_mib=100)
        pool = ResourcePool(self.test_dir, budget=budget)
        self.assertEqual(pool.total_mem_quanta, 1)
        self.assertEqual(pool.usable_memory_mib, 64)

        # 64 MiB fits exactly
        with pool.reserve(cpu_slots=1, memory_mib=64, timeout=0.1) as res:
            self.assertEqual(res.cpu_slots, 1)
            self.assertEqual(res.memory_mib, 64)

        # Rounded reservation for 65 MiB is 128 MiB (2 quanta) -> cannot fit in 100 MiB ceiling
        with self.assertRaises(ValueError):
            with pool.reserve(cpu_slots=1, memory_mib=65):
                pass

        # 100 MiB request also rounds to 128 MiB (2 quanta) -> rejected
        with self.assertRaises(ValueError):
            with pool.reserve(cpu_slots=1, memory_mib=100):
                pass

        # Multi-process test: 1 worker takes 64 MiB, second worker cannot acquire any memory
        ready = mp.Event()
        rel = mp.Event()
        p = mp.Process(target=_worker_reserve_and_hold, args=(self.test_dir, 1, 64, 5.0, ready, rel))
        p.start()
        try:
            self.assertTrue(ready.wait(timeout=5.0))
            with self.assertRaises(TimeoutError):
                with pool.reserve(cpu_slots=1, memory_mib=64, timeout=0.1):
                    pass
        finally:
            rel.set()
            p.join(timeout=5.0)

    def test_quantum_persistence_and_incompatible_reopen_rejection(self):
        budget = ResourceBudget(cpu_slots=2, memory_mib=128)
        p1 = ResourcePool(self.test_dir, budget=budget, memory_quantum_mib=64)

        # Shared metadata must persist memory quantum
        meta_content = (pathlib.Path(self.test_dir) / "pool.meta").read_text(encoding="utf-8")
        self.assertIn("memory_quantum_mib=64", meta_content)

        # Default reopen adopts stored quantum
        p_reopen = ResourcePool.open(self.test_dir)
        self.assertEqual(p_reopen.quantum_mib, 64)
        self.assertEqual(p_reopen.total_mem_quanta, 2)

        # Reopen with matching explicit quantum succeeds
        p_matching = ResourcePool.open(self.test_dir, memory_quantum_mib=64)
        self.assertEqual(p_matching.quantum_mib, 64)

        # Reopen with incompatible quantum must fail (prevents overspending 128 pool by 254)
        with self.assertRaises(ValueError):
            ResourcePool.open(self.test_dir, memory_quantum_mib=1)

        with self.assertRaises(ValueError):
            ResourcePool(self.test_dir, budget=budget, memory_quantum_mib=32)

        # Multi-process test: separate process cannot reopen with incompatible quantum
        q = mp.Queue()
        p = mp.Process(target=_worker_attempt_incompatible_reopen, args=(self.test_dir, 1, q))
        p.start()
        p.join(timeout=5.0)
        msg = q.get(timeout=2.0)
        self.assertTrue(msg.startswith("ValueError: Memory quantum mismatch"))

    def test_cleanup_on_acquisition_exception_preserves_original_io_error(self):
        budget = ResourceBudget(cpu_slots=2, memory_mib=128)
        pool = ResourcePool(self.test_dir, budget=budget)

        # Turn slot_1.lock into a directory to provoke IsADirectoryError during os.open
        slot_1_path = pathlib.Path(self.test_dir) / "mem_slots" / "slot_1.lock"
        slot_1_path.unlink()
        slot_1_path.mkdir()

        # Reservation requesting 1 CPU slot and 128 MiB memory (2 quanta)
        # Should acquire CPU slot 0, mem slot 0, then raise IsADirectoryError on mem slot 1
        with self.assertRaises(IsADirectoryError):
            with pool.reserve(cpu_slots=1, memory_mib=128, timeout=0.5):
                pass

        # Descriptors must have been fully cleaned up: CPU slot 0 and mem slot 0 must not be leaked!
        # Restore slot_1.lock as normal file
        slot_1_path.rmdir()
        slot_1_path.touch()

        with pool.reserve(cpu_slots=2, memory_mib=128, timeout=0.5) as res:
            self.assertEqual(res.cpu_slots, 2)
            self.assertEqual(res.memory_mib, 128)

        # Multi-process test: separate process encounters IsADirectoryError and cleans up
        slot_1_path.unlink()
        slot_1_path.mkdir()
        q = mp.Queue()
        p = mp.Process(target=_worker_reserve_is_a_directory, args=(self.test_dir, 1, 128, q))
        p.start()
        p.join(timeout=5.0)
        msg = q.get(timeout=2.0)
        self.assertTrue(msg.startswith("IsADirectoryError:"))

        # Main process immediately verifies resources are not leaked by the child process
        slot_1_path.rmdir()
        slot_1_path.touch()
        with pool.reserve(cpu_slots=2, memory_mib=128, timeout=0.5) as res:
            self.assertEqual(res.cpu_slots, 2)
            self.assertEqual(res.memory_mib, 128)

    def test_finite_monotonic_default_timeout_and_bounded_constructor_guard(self):
        budget = ResourceBudget(cpu_slots=2, memory_mib=128)
        pool = ResourcePool(self.test_dir, budget=budget)

        self.assertIsInstance(DEFAULT_RESERVE_TIMEOUT_SEC, (int, float))
        self.assertGreater(DEFAULT_RESERVE_TIMEOUT_SEC, 0)
        self.assertIsInstance(DEFAULT_GUARD_TIMEOUT_SEC, (int, float))
        self.assertGreater(DEFAULT_GUARD_TIMEOUT_SEC, 0)

        # Verify documentation mentions finite monotonic default
        self.assertIn("DEFAULT_RESERVE_TIMEOUT_SEC", pool.reserve.__doc__)

        # Bounded constructor allocation-guard test across processes
        ready = mp.Event()
        rel = mp.Event()
        p = mp.Process(target=_worker_hold_guard, args=(self.test_dir, ready, rel))
        p.start()
        try:
            self.assertTrue(ready.wait(timeout=5.0))
            start = time.monotonic()
            with self.assertRaises(TimeoutError):
                ResourcePool(self.test_dir, budget=budget, guard_timeout=0.1)
            duration = time.monotonic() - start
            self.assertGreaterEqual(duration, 0.08)
            self.assertLess(duration, 1.0)
        finally:
            rel.set()
            p.join(timeout=5.0)

        # Guard released: constructor succeeds without delay
        p2 = ResourcePool.open(self.test_dir, guard_timeout=1.0)
        self.assertEqual(p2.budget, budget)


if __name__ == "__main__":
    unittest.main()
