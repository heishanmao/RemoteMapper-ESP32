"""Model a shared USB interrupt-mask race without accessing USB hardware.

Run: python -m unittest discover -s tools/tests -p test_dwc2_mask_race.py -v

This exhaustively checks read/write interleavings of independent endpoint
updates. It demonstrates the lost-update mechanism and what mutual exclusion
prevents; it does not verify the firmware lock boundaries or the controller.
"""

from dataclasses import dataclass
import itertools
import unittest


@dataclass(frozen=True)
class Update:
    name: str
    bit: int
    enable: bool

    def apply(self, value):
        return value | self.bit if self.enable else value & ~self.bit


def interleavings(updates):
    """Enumerate schedules preserving each actor's read-before-write order."""
    def visit(done, prefix):
        if all(count == 2 for count in done):
            yield tuple(prefix)
            return
        for actor, count in enumerate(done):
            if count < 2:
                next_done = list(done)
                next_done[actor] += 1
                yield from visit(next_done, prefix + [(actor, count)])

    yield from visit([0] * len(updates), [])


def execute(initial, updates, schedule, protected=()):
    """Return final mask/history, or None when a shared lock blocks a schedule.

    Each protected actor holds the same lock across its read and write.
    Unprotected actors ignore that lock, deliberately modelling incomplete
    coverage. This is a synchronization model, not a portMUX implementation.
    """
    protected = frozenset(protected)
    owner = None
    mask = initial
    snapshots = {}
    history = []
    for actor, phase in schedule:
        if actor in protected:
            if phase == 0:
                if owner is not None:
                    return None
                owner = actor
            elif owner != actor:
                raise AssertionError("write without owning the model lock")
        if phase == 0:
            snapshots[actor] = mask
        else:
            mask = updates[actor].apply(snapshots[actor])
            if actor in protected:
                owner = None
        history.append((updates[actor].name, "read" if phase == 0 else "write", mask))
    return mask, tuple(history)


def serial_result(initial, updates):
    """Independent endpoint updates commute, so serial order is immaterial."""
    mask = initial
    for update in updates:
        mask = update.apply(mask)
    return mask


class SharedInterruptMaskRace(unittest.TestCase):
    def setUp(self):
        self.updates = (
            Update("HID submit", 1 << 1, True),
            Update("UAC FIFO finished", 1 << 3, False),
        )
        self.initial = 1 << 3

    def test_concrete_lost_hid_enable_matches_fault_mask(self):
        # ISR reads 8; HID reads 8 and writes 10; ISR writes its stale 0.
        schedule = ((1, 0), (0, 0), (0, 1), (1, 1))
        final, history = execute(self.initial, self.updates, schedule)
        self.assertEqual(final, 0)
        self.assertEqual(serial_result(self.initial, self.updates), 1 << 1)
        self.assertEqual([entry[2] for entry in history], [8, 8, 10, 0])
        # A shared lock cannot execute that same overlapping history.
        self.assertIsNone(execute(self.initial, self.updates, schedule, (0, 1)))

    def test_all_two_actor_schedules_have_unlocked_counterexamples(self):
        results = [execute(self.initial, self.updates, schedule)[0]
                   for schedule in interleavings(self.updates)]
        self.assertEqual(len(results), 6)
        self.assertIn(0, results)  # HID enable disappears.
        self.assertIn(10, results)  # UAC disable disappears instead.

    def test_locking_only_submit_or_only_isr_is_insufficient(self):
        wanted = serial_result(self.initial, self.updates)
        for protected in ((0,), (1,)):
            with self.subTest(protected=protected):
                finals = [result[0] for schedule in interleavings(self.updates)
                          if (result := execute(self.initial, self.updates,
                                                schedule, protected)) is not None]
                self.assertTrue(any(final != wanted for final in finals))

    def test_shared_lock_preserves_all_independent_endpoint_updates(self):
        # Cover set/set, clear/clear, set/clear, every initial mask, and three
        # endpoints (including EP0). Preserve unrelated bit 12 as well.
        accepted = 0
        for count in (2, 3):
            for initial_low in range(1 << count):
                initial = initial_low | (1 << 12)
                for enables in itertools.product((False, True), repeat=count):
                    updates = tuple(Update(f"EP{n}", 1 << n, enable)
                                    for n, enable in enumerate(enables))
                    wanted = serial_result(initial, updates)
                    legal = 0
                    for schedule in interleavings(updates):
                        result = execute(initial, updates, schedule, range(count))
                        if result is not None:
                            self.assertEqual(result[0], wanted)
                            self.assertTrue(result[0] & (1 << 12))
                            legal += 1
                    self.assertEqual(legal, 2 if count == 2 else 6)
                    accepted += legal
        self.assertEqual(accepted, 416)

    def test_later_rearm_does_not_remove_the_original_lost_update(self):
        schedule = ((1, 0), (0, 0), (0, 1), (1, 1))
        failed_mask, history = execute(self.initial, self.updates, schedule)
        rearmed_mask = self.updates[0].apply(failed_mask)
        self.assertEqual(rearmed_mask, serial_result(self.initial, self.updates))
        self.assertEqual(history[-1][2], 0)
        self.assertNotEqual(failed_mask, rearmed_mask)
        # Eventual recovery repairs the symptom after an observable gap;
        # mutual exclusion prevents the gap from occurring in this model.
        locked_finals = [result[0] for schedule in interleavings(self.updates)
                         if (result := execute(self.initial, self.updates,
                                               schedule, (0, 1))) is not None]
        self.assertEqual(locked_finals, [1 << 1, 1 << 1])


if __name__ == "__main__":
    unittest.main()
