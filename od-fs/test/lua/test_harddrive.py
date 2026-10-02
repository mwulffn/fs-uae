"""Tests of save states with a directory on the host as hard drive.

The tests need vasm and vlink to build the programs, and are skipped if they
are not installed.
"""

import tempfile
import unittest
from pathlib import Path

from harness import Emulator, build_directory_drive


class DirectoryDriveTestCase(unittest.TestCase):
    """Boots the program given in the subclass from a directory hard drive."""

    program_name: str

    @classmethod
    def setUpClass(cls) -> None:
        cls.directory = tempfile.TemporaryDirectory(prefix="fsuae-lua-drive-")
        try:
            drive = build_directory_drive(Path(cls.directory.name), cls.program_name)
            cls.emulator = Emulator({"filesystem2": f"rw,DH0:Test:{drive},0"})
        except BaseException:
            cls.directory.cleanup()
            raise
        cls.lua = cls.emulator.lua
        try:
            cls.lua.call(
                f"emu.warp(true) program = dbg.load_symbols('{drive / cls.program_name}', nil, 5000) "
                "emu.warp(false)"
            )
        except Exception:
            cls.tearDownClass()
            raise

    @classmethod
    def tearDownClass(cls) -> None:
        cls.emulator.stop()
        cls.directory.cleanup()

    def counter(self) -> int:
        return self.lua.eval("mem.peek_u32(program.symbols.counter)")


class IdleDriveTest(DirectoryDriveTestCase):
    """The program does not use the drive after it has been loaded."""

    program_name = "testprog"

    def test_snapshot_and_restore(self) -> None:
        self.lua.call("emu.pause() snapshot = state.snapshot()")
        try:
            saved = self.counter()
            self.lua.call("emu.step(50)")
            self.assertEqual(self.counter(), saved + 50)
            self.lua.call("state.restore(snapshot)")
            self.assertLessEqual(abs(self.counter() - (saved + 1)), 1)
            # The program keeps running after the restore.
            self.lua.call("emu.step(10)")
            self.assertGreater(self.counter(), saved + 5)
        finally:
            self.lua.call("emu.resume()")

    def test_save_and_load_file(self) -> None:
        path = self.emulator.path / "drive.uss"
        self.lua.call(f"state.save('{path}')")
        saved = self.counter()
        self.lua.call(f"emu.wait_frames(30) state.load('{path}')")
        self.assertLess(self.counter(), saved + 10)


class BusyDriveTest(DirectoryDriveTestCase):
    """The program reads from the drive all the time."""

    program_name = "ioprog"

    def test_program_reads_files(self) -> None:
        before = self.counter()
        self.lua.call("emu.wait_frames(25)")
        self.assertGreater(self.counter(), before + 5)

    def test_snapshots_wait_for_the_file_system(self) -> None:
        # A state cannot be saved while the file system is handling a
        # request. state.snapshot then waits a frame and tries again, so it
        # does not fail.
        failures = self.lua.eval(
            "local failures = 0 for i = 1, 60 do "
            "if not pcall(state.snapshot) then failures = failures + 1 end end return failures"
        )
        self.assertEqual(failures, 0)

    def test_save_file_waits_for_the_file_system(self) -> None:
        path = self.emulator.path / "busy.uss"
        failures = self.lua.eval(
            f"local failures = 0 for i = 1, 30 do "
            f"if not pcall(state.save, '{path}') then failures = failures + 1 end end return failures"
        )
        self.assertEqual(failures, 0)
        self.assertGreater(path.stat().st_size, 1000)


if __name__ == "__main__":
    unittest.main()
