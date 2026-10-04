#!/usr/bin/env python3
"""
Model tests for the core_diagnostics task registry.

The firmware module reads live FreeRTOS handles, which cannot run on the host.
These tests reimplement its registry semantics and check the logic that the
firmware cannot self-check at runtime: slot exhaustion, duplicate handles,
affinity classification and the per-core tally that backs the pinning contract.

They do not exercise the C++ code. Passing them shows the intended semantics
are well defined, not that the firmware matches.
"""

import sys
import unittest

MAX_TASKS = 16
NO_AFFINITY = 0x7FFFFFFF
NUM_CORES = 2

# Core affinity values FreeRTOS can return, per task.h.
AFF_CORE0 = 0
AFF_CORE1 = 1
AFF_UNBOUND = NO_AFFINITY


class Registry:
    """Mirrors src/core_diagnostics.cpp."""

    def __init__(self, max_tasks=MAX_TASKS):
        self.max_tasks = max_tasks
        self.slots = []

    def register(self, name, handle, expected_core):
        # A null handle means task creation failed; the firmware returns early
        # and stores nothing.
        if handle is None:
            return False
        if len(self.slots) >= self.max_tasks:
            return False
        self.slots.append(
            {"name": name, "handle": handle, "expected": expected_core}
        )
        return True

    def snapshot(self, affinity_of=None, live_name=None):
        """
        affinity_of(handle) -> core or tskNO_AFFINITY, standing in for
        xTaskGetAffinity(). live_name(handle) -> str or None, standing in for
        pcTaskGetName(); None means the task is gone and the recorded name is
        used instead.
        """
        tasks = []
        per_core = [0] * NUM_CORES
        unpinned = 0
        for slot in self.slots:
            affinity = affinity_of(slot["handle"]) if affinity_of else AFF_UNBOUND
            name = slot["name"]
            if live_name:
                seen = live_name(slot["handle"])
                if seen is not None:
                    name = seen
            # The firmware classifies affinity before tallying.
            if affinity is AFF_UNBOUND or affinity == AFF_UNBOUND:
                core = -1
                unpinned += 1
            elif 0 <= affinity < NUM_CORES:
                core = affinity
                per_core[affinity] += 1
            else:
                core = -2
            tasks.append(
                {
                    "name": name,
                    "core": core,
                    "expected_core": slot["expected"],
                    "as_expected": core == slot["expected"],
                }
            )
        return {"tasks": tasks, "per_core": per_core, "unpinned": unpinned}


class TestRegistration(unittest.TestCase):
    def test_failed_creation_is_not_registered(self):
        r = Registry()
        self.assertFalse(r.register("uac_push", None, 1))
        self.assertEqual(r.snapshot()["tasks"], [])

    def test_null_handle_leaves_earlier_entries_intact(self):
        r = Registry()
        r.register("ble_audio_task", 0xAAAA, 0)
        r.register("uac_push", None, 1)
        snap = r.snapshot()
        self.assertEqual(len(snap["tasks"]), 1)
        self.assertEqual(snap["tasks"][0]["name"], "ble_audio_task")

    def test_slots_are_not_overwritten_when_full(self):
        r = Registry(max_tasks=2)
        r.register("a", 0x1, 0)
        r.register("b", 0x2, 1)
        self.assertFalse(r.register("c", 0x3, 0))
        names = [t["name"] for t in r.snapshot()["tasks"]]
        self.assertEqual(names, ["a", "b"])

    def test_all_three_resident_tasks_fit(self):
        r = Registry()
        # The firmware registers exactly these three today.
        self.assertTrue(r.register("ble_audio_task", 0x1, 0))
        self.assertTrue(r.register("led_task", 0x2, 1))
        self.assertTrue(r.register("uac_push", 0x3, 1))
        self.assertEqual(len(r.snapshot()["tasks"]), 3)


class TestAffinityClassification(unittest.TestCase):
    def affinity_of(self, value):
        r = Registry()
        r.register("t", 0x1, 0)
        return r.snapshot(affinity_of=lambda h: value)["tasks"][0]["core"]

    def test_core0_and_core1_are_distinct(self):
        self.assertEqual(self.affinity_of(AFF_CORE0), 0)
        self.assertEqual(self.affinity_of(AFF_CORE1), 1)

    def test_no_affinity_is_negative_one(self):
        self.assertEqual(self.affinity_of(AFF_UNBOUND), -1)

    def test_out_of_range_is_flagged_not_silently_dropped(self):
        self.assertEqual(self.affinity_of(7), -2)
        self.assertEqual(self.affinity_of(-1), -2)

    def test_core_and_expected_core_comparison_is_exact(self):
        # -1 must not satisfy an expectation of 0: a task that silently became
        # unpinned is exactly the regression this module exists to catch.
        r = Registry()
        r.register("t", 0x1, 0)
        snap = r.snapshot(affinity_of=lambda h: AFF_UNBOUND)
        self.assertFalse(snap["tasks"][0]["as_expected"])


class TestPerCoreTally(unittest.TestCase):
    def test_counts_land_on_the_declared_core(self):
        r = Registry()
        r.register("ble_audio_task", 0x1, 0)
        r.register("led_task", 0x2, 1)
        r.register("uac_push", 0x3, 1)
        aff = {0x1: AFF_CORE0, 0x2: AFF_CORE1, 0x3: AFF_CORE1}
        snap = r.snapshot(affinity_of=lambda h: aff[h])
        self.assertEqual(snap["per_core"], [1, 2])
        self.assertTrue(all(t["as_expected"] for t in snap["tasks"]))

    def test_unpinned_task_is_excluded_from_both_cores(self):
        r = Registry()
        r.register("loopTask", 0x1, 1)
        snap = r.snapshot(affinity_of=lambda h: AFF_UNBOUND)
        self.assertEqual(snap["per_core"], [0, 0])
        self.assertEqual(snap["unpinned"], 1)

    def test_drift_from_expected_core_is_visible(self):
        # Simulates a framework upgrade moving loopTask off core 1: the tally
        # follows the live affinity while as_expected goes false.
        r = Registry()
        r.register("loopTask", 0x1, 1)
        snap = r.snapshot(affinity_of=lambda h: AFF_CORE0)
        self.assertEqual(snap["per_core"], [1, 0])
        self.assertFalse(snap["tasks"][0]["as_expected"])


class TestDeletedTask(unittest.TestCase):
    def test_deleted_task_reports_unknown_name(self):
        # The firmware cannot detect deletion without a kernel lock, so
        # pcTaskGetName() may come back null and the recorded name is used as
        # the fallback. Pin that: a recycled or deleted handle must still yield
        # a well-formed record rather than a crash or a null name.
        r = Registry()
        r.register("uac_push", 0x1, 1)
        snap = r.snapshot(affinity_of=lambda h: AFF_CORE1, live_name=lambda h: None)
        self.assertEqual(len(snap["tasks"]), 1)
        self.assertEqual(snap["tasks"][0]["name"], "uac_push")
        self.assertEqual(snap["tasks"][0]["core"], 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)