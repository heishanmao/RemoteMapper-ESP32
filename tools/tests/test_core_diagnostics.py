#!/usr/bin/env python3
"""
Registry model and source-contract tests for core_diagnostics.

The firmware module reads live FreeRTOS handles, which cannot run on the host.
These tests reimplement its registry semantics and check the logic that the
firmware cannot self-check at runtime: slot exhaustion,
affinity classification and the per-core tally that backs the pinning contract.

The model does not execute the C++ code. The source-contract tests additionally
inspect the actual firmware implementation to reject FreeRTOS queries, stack
scans and JSON work under its interrupt-disabling registry lock. Neither group
replaces a firmware build or hardware timing measurement.
"""

from pathlib import Path
import re
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

    def snapshot(self, affinity_of=None):
        """
        affinity_of(handle) -> core or tskNO_AFFINITY, standing in for
        xTaskGetAffinity(). All registered handles must remain alive; FreeRTOS
        offers no deleted-handle validity check through pcTaskGetName().
        """
        tasks = []
        per_core = [0] * NUM_CORES
        unpinned = 0
        for slot in self.slots:
            affinity = affinity_of(slot["handle"]) if affinity_of else AFF_UNBOUND
            name = slot["name"]
            # The firmware classifies affinity before tallying.
            if affinity is AFF_UNBOUND or affinity == AFF_UNBOUND:
                core = -1
                unpinned += 1
            elif 0 <= affinity < NUM_CORES:
                core = affinity
                per_core[affinity] += 1
            else:
                core = -2
                unpinned += 1
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


class TestRegisteredNames(unittest.TestCase):
    def test_snapshot_uses_the_registered_name(self):
        r = Registry()
        r.register("uac_push", 0x1, 1)
        snap = r.snapshot(affinity_of=lambda h: AFF_CORE1)
        self.assertEqual(len(snap["tasks"]), 1)
        self.assertEqual(snap["tasks"][0]["name"], "uac_push")
        self.assertEqual(snap["tasks"][0]["core"], 1)


class TestFirmwareCriticalSection(unittest.TestCase):
    """Regression checks against actual source, rather than the Python model."""

    @classmethod
    def setUpClass(cls):
        root = Path(__file__).resolve().parents[2]
        cls.source = (root / "src/core_diagnostics.cpp").read_text(encoding="utf-8")
        # Comments document forbidden operations, so do not treat them as calls.
        cls.code = re.sub(r"//[^\n]*|/\*.*?\*/", "", cls.source, flags=re.S)
        cls.json_code = cls.code.split("void core_diagnostics_json(JsonObject out)", 1)[1]

    @staticmethod
    def assert_copy_only(testcase, critical_body):
        # A conservative whitelist: this bounded copy is the whole allowed
        # interrupt-off body, so future helper calls or allocations need review.
        compact = re.sub(r"\s+", "", critical_body)
        testcase.assertEqual(
            compact,
            "for(inti=0;i<CORE_DIAGNOSTICS_MAX_TASKS;i++){snapshot[i]=s_entries[i];}",
        )

    def test_json_lock_contains_only_the_bounded_registry_copy(self):
        regions = re.findall(
            r"portENTER_CRITICAL\(&s_mux\);(.*?)portEXIT_CRITICAL\(&s_mux\);",
            self.json_code,
            flags=re.S,
        )
        self.assertEqual(len(regions), 1)
        self.assert_copy_only(self, regions[0])
        self.assertEqual(self.json_code.count("portENTER_CRITICAL"), 1)
        self.assertEqual(self.json_code.count("portEXIT_CRITICAL"), 1)

    def test_the_previous_long_critical_section_is_rejected(self):
        # These are the exact operations which used to extend the lock across
        # kernel queries and allocations. Ensure the guard fails for that code.
        with self.assertRaises(AssertionError):
            self.assert_copy_only(
                self,
                'JsonArray tasks = out["tasks"].to<JsonArray>(); '
                'uxTaskGetStackHighWaterMark(e.handle);',
            )

    def test_task_queries_and_json_happen_after_unlock(self):
        before_unlock, after_unlock = self.json_code.split("portEXIT_CRITICAL(&s_mux);", 1)
        for operation in (
            "xTaskGetAffinity(",
            "uxTaskPriorityGet(",
            "uxTaskGetStackHighWaterMark(",
            "JsonArray tasks",
            "tasks.add<JsonObject>()",
        ):
            self.assertNotIn(operation, before_unlock)
            self.assertIn(operation, after_unlock)
        # The JSON loop must use the immutable local snapshot, never a live
        # registry entry which another core can update after unlocking.
        self.assertIn("const Entry &e = snapshot[i];", after_unlock)
        self.assertNotIn("s_entries", after_unlock)

    def test_name_query_is_not_used_as_handle_validation(self):
        self.assertNotRegex(self.code, r"pcTaskGetName\s*\(")
        self.assertIn('o["name"] = e.name ? e.name : "unnamed";', self.json_code)

    def test_http_handler_initializes_the_json_root(self):
        root = Path(__file__).resolve().parents[2]
        web = (root / "src/web/web_server.cpp").read_text(encoding="utf-8")
        handler = web.split("static void handle_cores()", 1)[1].split(
            "static void handle_guard()", 1)[0]
        # A fresh document is null. as<JsonObject>() would pass a null view and
        # silently serialize null; to<JsonObject>() creates the writable root.
        self.assertIn("core_diagnostics_json(doc.to<JsonObject>());", handler)
        self.assertNotIn("core_diagnostics_json(doc.as<JsonObject>());", handler)


if __name__ == "__main__":
    unittest.main(verbosity=2)
