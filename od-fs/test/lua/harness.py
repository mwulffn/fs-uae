"""Starts FS-UAE with the Lua socket enabled, for the tests in this directory.

The tests need a Kickstart ROM, given with the environment variable
FSUAE_TEST_KICKSTART. The matching Amiga model (A1200 or A500) is given with
FSUAE_TEST_MODEL, and defaults to A1200. FSUAE_TEST_BINARY overrides the
path to the fs-uae executable, which defaults to od-fs/fs-uae.
"""

import os
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

OD_FS_DIR = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(OD_FS_DIR / "scripts"))

from fsuae_lua import LuaClient, LuaError, to_bytes  # noqa: E402, F401

MODEL_OPTIONS = {
    "A500": {"chipset": "ocs", "cpu_model": "68000", "chipmem_size": "1"},
    "A1200": {"chipset": "aga", "cpu_model": "68020", "chipmem_size": "4"},
}

# A boot block program which takes over the machine and, once per frame:
# - adds 1 to the long word at DATA_OFFSET (the frame counter)
# - copies JOY1DAT to the word at DATA_OFFSET + 4
# - copies the word at DATA_OFFSET + 6 to COLOR00 (the colour of the screen)
# The offsets are relative to the start of the code (see find_test_program).
TEST_PROGRAM = bytes.fromhex(
    "4bf900dff000"  # 00 lea $dff000,a5
    "3b7c7fff009a"  # 06 move.w #$7fff,$9a(a5)   (INTENA: interrupts off)
    "3b7c7fff0096"  # 0c move.w #$7fff,$96(a5)   (DMACON: DMA off)
    "41fa004a"  # 12 lea data(pc),a0
    "7000"  # 16 moveq #0,d0
    "222d0004"  # 18 wait1: move.l 4(a5),d1    (VPOSR and VHPOSR)
    "c2bc0001ff00"  # 1c and.l #$1ff00,d1
    "b2bc0000c800"  # 22 cmp.l #$c800,d1         (wait for line 200)
    "66ee"  # 28 bne.s wait1
    "222d0004"  # 2a wait2: move.l 4(a5),d1
    "c2bc0001ff00"  # 2e and.l #$1ff00,d1
    "b2bc0000c800"  # 34 cmp.l #$c800,d1         (wait for the line after)
    "67ee"  # 3a beq.s wait2
    "5280"  # 3c addq.l #1,d0
    "2080"  # 3e move.l d0,(a0)
    "342d000c"  # 40 move.w $c(a5),d2          (JOY1DAT)
    "31420004"  # 44 move.w d2,4(a0)
    "3b6800060180"  # 48 move.w 6(a0),$180(a5)   (COLOR00)
    "60c8"  # 4e bra.s wait1
    "4e714e714e714e714e714e714e71"  # 50 (padding)
    "00000000"  # 5e data: frame counter
    "0000"  # 62 JOY1DAT copy
    "0f00"  # 64 colour (red)
)
LOOP_OFFSET = 0x18
ADDQ_OFFSET = 0x3C
DATA_OFFSET = 0x5E
COUNTER_OFFSET = DATA_OFFSET
JOYSTICK_OFFSET = DATA_OFFSET + 4
COLOUR_OFFSET = DATA_OFFSET + 6


def boot_block_checksum(block: bytes) -> int:
    total = 0
    for (value,) in struct.iter_unpack(">I", block):
        total += value
        if total > 0xFFFFFFFF:
            total = (total + 1) & 0xFFFFFFFF
    return ~total & 0xFFFFFFFF


def create_test_disk(path: Path) -> None:
    """Write an ADF disk image which boots TEST_PROGRAM."""
    block = bytearray(1024)
    block[0:4] = b"DOS\0"
    block[8:12] = struct.pack(">I", 880)
    block[12 : 12 + len(TEST_PROGRAM)] = TEST_PROGRAM
    block[4:8] = struct.pack(">I", boot_block_checksum(bytes(block)))
    path.write_bytes(bytes(block) + bytes(901120 - 1024))


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Emulator:
    """A running FS-UAE with a connected LuaClient in self.lua."""

    def __init__(self, options: dict[str, str] | None = None, test_disk: bool = False) -> None:
        kickstart = os.environ.get("FSUAE_TEST_KICKSTART")
        if not kickstart:
            raise unittest.SkipTest("FSUAE_TEST_KICKSTART is not set")
        binary = os.environ.get("FSUAE_TEST_BINARY", str(OD_FS_DIR / "fs-uae"))
        model = os.environ.get("FSUAE_TEST_MODEL", "A1200")

        self.directory = tempfile.TemporaryDirectory(prefix="fsuae-lua-test-")
        self.path = Path(self.directory.name)
        self.port = free_port()
        config = {"kickstart_rom_file": kickstart, "lua_port": str(self.port)}
        config.update(MODEL_OPTIONS[model])
        if test_disk:
            create_test_disk(self.path / "test.adf")
            config["floppy0"] = str(self.path / "test.adf")
        config.update(options or {})
        config_path = self.path / "test.uae"
        config_path.write_text("".join(f"{key}={value}\n" for key, value in config.items()))

        self.lua: LuaClient | None = None
        self.log = open(self.path / "log.txt", "w")
        self.process = subprocess.Popen(
            [binary, str(config_path)], stdout=self.log, stderr=subprocess.STDOUT
        )
        try:
            self.lua = self.connect()
        except Exception:
            self.stop()
            raise

    def connect(self) -> LuaClient:
        deadline = time.monotonic() + 30
        while True:
            try:
                return LuaClient(self.port)
            except ConnectionRefusedError:
                if self.process.poll() is not None:
                    raise RuntimeError("FS-UAE exited while starting") from None
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)

    def stop(self) -> None:
        if self.lua is not None:
            self.lua.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.log.close()
        self.directory.cleanup()

    def find_test_program(self) -> int:
        """Wait until the test disk has booted and return the address of TEST_PROGRAM."""
        self.lua.call("emu.warp(true)")
        try:
            # The program is running when the frame counter in the boot block
            # (which is 0 on the disk) has been changed.
            code = "".join(f"\\x{byte:02x}" for byte in TEST_PROGRAM[:DATA_OFFSET])
            deadline = time.monotonic() + 60
            while time.monotonic() < deadline:
                address = self.lua.eval(
                    f"local s = mem.read_range(0, 0x100000):find('{code}', 1, true) "
                    f"if s and mem.peek_u32(s - 1 + {COUNTER_OFFSET}) ~= 0 then return s - 1 end"
                )
                if address is not None:
                    return address
                self.lua.call("emu.wait_frames(25)")
            raise RuntimeError("The test disk did not boot")
        finally:
            self.lua.call("emu.warp(false)")


class EmulatorTestCase(unittest.TestCase):
    """Base class for tests which share one emulator per test class."""

    options: dict[str, str] = {}
    test_disk = False
    emulator: Emulator
    lua: LuaClient

    @classmethod
    def setUpClass(cls) -> None:
        cls.emulator = Emulator(cls.options, cls.test_disk)
        cls.lua = cls.emulator.lua

    @classmethod
    def tearDownClass(cls) -> None:
        cls.emulator.stop()
