"""Runs the tests which depend on CPU timing with cycle-exact emulation.

In cycle-exact modes the end of a frame is reached in the middle of an
instruction, which is the difficult case for the Lua engine.
"""

import unittest

import test_cpu
import test_debug
import test_mem
import test_state

CYCLE_EXACT = {"cycle_exact": "true"}


class CycleExactConfigTest(test_mem.EmulatorTestCase):
    options = CYCLE_EXACT

    def test_cycle_exact_is_enabled(self) -> None:
        self.assertEqual(self.lua.eval("emu.config_get('cycle_exact')"), "true")


class CycleExactMemTest(test_mem.MemTest):
    options = CYCLE_EXACT


class CycleExactCpuTest(test_cpu.CpuTest):
    options = CYCLE_EXACT


class CycleExactBreakpointTest(test_debug.BreakpointTest):
    options = CYCLE_EXACT


class CycleExactStepTest(test_debug.StepTest):
    options = CYCLE_EXACT


class CycleExactTapTest(test_debug.TapTest):
    options = CYCLE_EXACT


class CycleExactSnapshotTest(test_state.SnapshotTest):
    options = CYCLE_EXACT


if __name__ == "__main__":
    unittest.main()
