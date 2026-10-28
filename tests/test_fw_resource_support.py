"""Unit tests for fw_resource_support."""

from __future__ import annotations

import multiprocessing as mp
import os
import shutil
import tempfile
import time
import unittest

from tests.fw_resource_support import ResourceBudget, ResourcePool


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


if __name__ == "__main__":
    unittest.main()
