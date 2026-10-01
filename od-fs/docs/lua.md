# Lua scripting

FS-UAE can run Lua scripts which inspect and control the emulated Amiga: read and write memory and
CPU registers, set breakpoints, send input, take screenshots and save states. Scripts are either
loaded when the emulation starts, or sent to a running FS-UAE over a local socket, which makes it
possible for another program to drive the emulation step by step.

Lua support is built by default (configure with `--disable-lua` to leave it out). It does nothing
unless one of the options below is used.

## Running scripts

Two configuration options (in the `.uae` file) enable Lua:

```
lua=/path/to/script.lua
lua_port=5600
```

- `lua` loads a script when the emulation starts. The option can be given up to 16 times.
- `lua_port` makes FS-UAE listen on this TCP port, on 127.0.0.1 only, for Lua code to run.

All scripts and all code received on the socket share one Lua state, so a global variable set by
one request can be used by the next. The standard Lua 5.4 libraries are available, including `io`
and `os`.

**Lua code is not sandboxed.** Anything that can connect to the port can run code with the access
of the FS-UAE process, so only enable `lua_port` on a machine where you trust the local users.

### The socket protocol

A request is one line of JSON, and so is the reply:

```
-> {"id": 7, "code": "return cpu.pc, mem.read_u16(0xdff004)"}
<- {"id": 7, "ok": true, "results": [16515298, 8236], "output": ""}
-> {"id": 8, "code": "error('no')"}
<- {"id": 8, "ok": false, "error": "remote:1: no", "output": ""}
```

- `code` is first tried as an expression, so `"cpu.pc"` works like `"return cpu.pc"`.
- `results` holds the returned values. Tables become JSON arrays or objects, and functions and other
  values which JSON cannot hold become strings describing them.
- `output` is what the code printed with `print`.
- Lua strings are byte strings. Bytes above 127 are sent as the characters U+0080 to U+00FF, so the
  receiver gets the original bytes by encoding the string as Latin-1.
- The reply is sent when the code has finished. Code which waits (for example with
  `emu.wait_frames`) is replied to later, and other requests are handled in the meantime.

FS-UAE also sends lines which are not replies. They have an `event` key instead of `id`:

- `{"event": "stopped", "reason": "breakpoint", "pc": ..., "id": ..., "address": ...}` when the
  emulation stops at a breakpoint, after a tap callback paused it, or after `dbg.step`.
- `{"event": "print", "text": "..."}` for `print` called outside a request, for example in a
  breakpoint callback.

### The client script

`od-fs/scripts/fsuae_lua.py` sends code to FS-UAE and prints the result. It only needs Python.

```sh
od-fs/scripts/fsuae_lua.py 'return string.format("%08x", cpu.pc)'
od-fs/scripts/fsuae_lua.py --file script.lua
od-fs/scripts/fsuae_lua.py --json 'cpu.disasm(cpu.pc, 5)'
```

The port defaults to 5600 and is changed with `--port`. The exit status is 1 if the code failed.
The file can also be imported: `LuaClient(port)` has `call(code)` returning the list of results,
`eval(code)` returning the first one, and `wait_event(name)`.

## Tasks, frames and pausing

Every script and every request runs as a *task*, which can wait while the emulation continues:

```lua
emu.wait_frames(50)        -- continue here 50 frames later
local info = dbg.wait()    -- continue here when the emulation stops
```

Tasks run between two emulated instructions, so the CPU registers are always consistent when Lua
code reads or changes them. Callbacks (frame callbacks, breakpoints and taps) are plain function
calls and cannot wait.

The emulation is either running or stopped. It stops when `emu.pause` is called, when a breakpoint
without a callback is reached, and when a frame or instruction step has finished. While it is
stopped, requests from the socket are still handled, so everything can be inspected and changed.
A request which waits for frames while the emulation is stopped is not answered until the
emulation runs again.

## API

Addresses and values are integers. Functions raise a Lua error when given invalid arguments.

### emu

| Function | Description |
| --- | --- |
| `emu.frame()` | Number of frames emulated since Lua was started. |
| `emu.cycles()` | The emulated time in cycles (see below). |
| `emu.beam()` | The line and the horizontal position (in colour clocks) the display has reached. |
| `emu.timing()` | `{lines = ..., cycles_per_line = ..., cycles_per_frame = ..., hz = ...}` for the current display mode. |
| `emu.wait_frames(n)` | Wait until `n` frames (default 1) have been emulated. |
| `emu.wait_next_frame()` | The same as `emu.wait_frames(1)`. |
| `emu.on_frame(f)` | Call `f()` after every frame. Returns an id. |
| `emu.remove_frame_callback(id)` | Remove a frame callback. |
| `emu.pause()` | Stop the emulation before the next instruction. |
| `emu.resume()` | Continue the emulation. |
| `emu.paused()` | True if the emulation is stopped. |
| `emu.step(n)` | Run `n` frames (default 1) and stop again. Returns when that is done. |
| `emu.warp(on)` | Turn warp mode (running as fast as possible) on or off. |
| `emu.reset(hard)` | Reset the Amiga. A hard reset also clears memory. |
| `emu.quit()` | Quit FS-UAE. |
| `emu.config_get(name)` | The value of a configuration option as a string, or nil. |
| `emu.config_set(name, value)` | Change a configuration option. Returns false if it was not accepted. |
| `emu.log(text)` | Write a line to the FS-UAE log. |

`print` writes to the output of the request, or to the log when called outside a request.

A cycle in `emu.cycles` is a cycle of the 7.09 MHz clock of a PAL Amiga: there are two per colour
clock, 454 per line and 142102 per PAL frame, and it is the unit in which the instruction times
of the 68000 are given. It is a whole number on a 68000; with a faster CPU, instructions also end
at fractions of a cycle. How much of a frame something takes is its cycles divided by
`emu.timing().cycles_per_frame`, and in raster lines by `cycles_per_line`.

### mem

| Function | Description |
| --- | --- |
| `mem.read_u8(a)`, `read_u16`, `read_u32` | Read like the CPU does. Reading a hardware register has the same side effects as on the Amiga. |
| `mem.write_u8(a, v)`, `write_u16`, `write_u32` | Write like the CPU does. |
| `mem.peek_u8(a)`, `peek_u16`, `peek_u32` | Read RAM or ROM directly. Fails for other addresses. |
| `mem.poke_u8(a, v)`, `poke_u16`, `poke_u32` | Write RAM or ROM directly. |
| `mem.read_range(a, length)` | The bytes as a string. Bytes which are not RAM or ROM are 0. |
| `mem.write_range(a, string)` | Write the bytes of the string to RAM or ROM. |
| `mem.custom.NAME` | The address of a custom chip register, for example `mem.custom.COLOR00`. |
| `mem.tap_read(first, last, f)` | Call `f(address, value, size, pc)` when the CPU reads from the range. Returns an id. |
| `mem.tap_write(first, last, f)` | The same when the CPU writes to the range. |
| `mem.tap_remove(id)` | Remove a tap, or all taps when called without an id. |

Writing memory from Lua also clears the instruction cache of the emulated CPU (68020 and later),
so changed code is run. The instruction at `cpu.pc` may already have been fetched; assign
`cpu.pc = cpu.pc` to fetch it again after changing it.

Notes on taps:

- If the callback returns an integer, that value is read or written instead.
- `pc` is the address of the instruction making the access. With 68030 or 68040 MMU emulation
  (the A3000 configuration has the first), it is the address of the instruction after it.
- The callback is called for each bus access. Depending on the CPU emulation, a long word is
  accessed as one long word or as two words.
- The callback runs in the middle of an instruction. It can read and write memory and call
  `emu.pause()`, which stops the emulation after the instruction.
- Memory accesses made from Lua do not run taps, and neither do accesses by the custom chips (DMA).
- There can be 20 taps. They make all accesses to the 64 KB blocks they are in slower.

### cpu

The registers are fields which can be read and assigned: `cpu.d0` to `cpu.d7`, `cpu.a0` to
`cpu.a7`, `cpu.pc`, `cpu.sr`, `cpu.usp`, `cpu.isp`, `cpu.msp` and `cpu.vbr`.

`cpu.disasm(address, count)` returns a list of `count` (default 1) instructions, each a table
`{address = ..., size = ..., text = "..."}`.

### dbg

| Function | Description |
| --- | --- |
| `dbg.bpset(address, f)` | Set a breakpoint and return its id. Without `f` the emulation stops there. With `f`, `f(address)` is called and the emulation continues, unless `f` calls `emu.pause()`. |
| `dbg.bpclear(id)` | Remove a breakpoint, or all breakpoints when called without an id. |
| `dbg.bplist()` | A list of `{id = ..., address = ...}`. |
| `dbg.exset(vector, f)` | Watch for a CPU exception and return an id. `vector` is the vector number, `"crash"` or `"halt"` (see below). Without `f` the emulation stops; with `f`, `f(vector, pc)` is called and the emulation continues, unless `f` calls `emu.pause()`. |
| `dbg.exclear(id)` | Remove an exception watch, or all of them when called without an id. |
| `dbg.go()` | Continue the emulation (the same as `emu.resume`). |
| `dbg.wait(frames)` | Wait until the emulation stops and return a table saying why. With `frames`, give up after that many frames and return nothing. |
| `dbg.step(n)` | Run `n` instructions (default 1) and stop. Returns the same as `dbg.wait`. |
| `dbg.stopped()` | The same table as `dbg.wait` returns if the emulation is stopped, otherwise false. |
| `dbg.command(text)` | Run a command in the built-in UAE debugger and return its output. |
| `dbg.measure(from, to, count, frames)` | Measure the time from one address to another (see below). |
| `dbg.load_symbols(path, name, frames)` | Load the symbols of a running program (see below). |
| `dbg.symbol(name)` | The address of a symbol, or nil. |
| `dbg.lookup(address)` | The name for an address inside a loaded program, such as `"update"` or `"update+$1a"`, or nil. |
| `dbg.unload_symbols(name)` | Forget the symbols of a program, or of all programs when called without a name. |

The table from `dbg.wait` has `reason` (`"breakpoint"`, `"tap"`, `"step"`, `"exception"`, `"halt"`
or `"pause"`) and `pc`. For breakpoints, taps and exceptions it also has `id` and `address`, and
for exceptions and halts `vector`.

#### Measuring time

`dbg.measure` uses breakpoints and `emu.cycles` to time a part of the program:

```lua
dbg.measure("update_player")                 -- a subroutine, from its start until it returns
dbg.measure(0x24a6c, 0x24b10)                -- from one address to another
dbg.measure("main_loop", "main_loop", 50)    -- between passes of one address, 50 times
```

- `from` and `to` are addresses, or symbol names when symbols are loaded. Without `to`, `from`
  must be the start of a subroutine entered with JSR or BSR.
- `count` is the number of measurements (default 1), and `frames` how many frames to wait for
  them (default 500). The emulation must be running.
- The result is `{count = ..., min = ..., max = ..., average = ..., samples = {...}}`, in the
  cycles of `emu.cycles`. Interrupts taken in between are part of the time.
- With cycle-exact 68000 emulation the times are exact: `ADDQ.L #1,D0` measures 8 cycles. The
  68020 and later are emulated with their instruction pipeline, where a short instruction can
  take no time of its own, and without cycle-exact emulation the times are approximate.

#### Symbols

When a program is started by AmigaDOS, it is loaded wherever there is free memory, so its
addresses differ from run to run. `dbg.load_symbols` works out the addresses of the names in the
program:

```lua
local program = dbg.load_symbols("/path/to/build/game", "game")
dbg.bpset("update_player")                  -- dbg.bpset takes symbol names
print(mem.peek_u16(program.symbols.lives))
print(dbg.lookup(cpu.pc))                   --> update_player+$1a
```

- `path` is the executable on the host: the same file which was put on the Amiga disk. It must be
  an Amiga executable (hunk format) which still has its symbols. vasm and vlink keep them by
  default; do not link with `-s`. Local labels are only there if the assembler was told to keep
  them.
- `name` is the name of the program on the Amiga, and defaults to the file name of `path`. The
  program must be running: it is found among the tasks of the Amiga, either as the command of a
  CLI or as a process with that name.
- `frames` makes the function wait up to that many frames for the program to start. Without it,
  the function fails if the program is not running.
- The result has `name`, `path`, `segments` (a list of `{address = ..., size = ...}`, one for each
  hunk of the executable) and `symbols` (name to address).
- C compilers put an underscore before the names from the source code. `program.symbols.main` and
  `dbg.symbol("main")` also find `_main`.
- When symbols are loaded, the table from `dbg.wait` and the `stopped` event also have `symbol`
  (the name for `pc`) and `address_symbol` (the name for `address`) where the addresses are inside
  a loaded program.

The symbols are those in the executable, so this does not help with programs which unpack or
move themselves after they are loaded, and overlays are not supported. Source files and line
numbers are not read. Load the symbols again after the program has been started again, as it will
be at another address.

Exception watches catch a program going wrong:

- `dbg.exset("crash")` matches the exceptions which normally mean that a program has crashed: bus
  error (vector 2), address error (3), illegal instruction (4), division by zero (5) and the
  unimplemented line A and line F instructions (10 and 11). It also matches the CPU halting,
  which is what happens after a double fault. Interrupts, TRAP instructions and privilege
  violations are not included, as the operating system uses them. On a 68040 or 68060, line F
  exceptions are also normal (unimplemented floating point instructions); watch the vectors you
  want by number there.
- The emulation stops at the first instruction of the exception handler, so the exception stack
  frame can be inspected. `address` in the stop information (and `pc` given to the callback) is
  the instruction which caused the exception.
- For a halt, `reason` is `"halt"` and `vector` holds the halt reason of the UAE core (2 is a
  double fault). When several exceptions happen within one instruction, there is one stop, and
  the last one is reported. Lua keeps working while the CPU is halted; `emu.reset()` or restoring
  a state gets it going again.
- A guru meditation which the operating system raises itself (by calling `Alert`) is not an
  exception, and is not caught unless it follows one of the exceptions above.

A breakpoint stops the emulation *before* the instruction at its address is run. While there are
breakpoints, the emulation is somewhat slower, because they are checked before every instruction
(see "Running fast").

`dbg.command` is meant for commands which show information, such as `r` (registers), `m` (memory),
`c` (CIA and custom chips) and `e` (custom registers). Commands which continue the emulation (`g`,
`t`, `f` and others) do not work from Lua; use the functions above instead.

### input

| Function | Description |
| --- | --- |
| `input.key(name, down)` | Press (`true`) or release (`false`) a key. |
| `input.type(text, frames)` | Type text on a US keyboard, holding each key for `frames` frames (default 2). |
| `input.joy(port, button, down)` | Press or release `"left"`, `"right"`, `"up"`, `"down"`, `"fire"`, `"fire2"` or `"fire3"`. Port 1 is the normal joystick port. |
| `input.mouse(dx, dy)` | Move the mouse in port 0. |
| `input.mouse_button(button, down)` | Press or release mouse button 1 (left), 2 (right) or 3 (middle). |
| `input.port_mode(port, mode)` | Connect `"joystick"`, `"mouse"`, `"cd32"` or `"none"` to a port. |

Key names are the ones in `inputevents.def` without `KEY_`, in any case: `a` to `z`, `0` to `9`,
`f1` to `f10`, `return`, `space`, `esc`, `tab`, `backspace`, `del`, `help`, `cursor_up`,
`cursor_down`, `cursor_left`, `cursor_right`, `shift_left`, `shift_right`, `ctrl`, `alt_left`,
`alt_right`, `amiga_left`, `amiga_right`, `np_0` to `np_9` and so on.

### video

| Function | Description |
| --- | --- |
| `video.size()` | The width and height of the frame in pixels. |
| `video.pixel(x, y)` | The red, green and blue values (0 to 255) of a pixel. |
| `video.pixels()` | The frame as a string with three bytes per pixel, and the width and height. |
| `video.screenshot(path)` | Save the frame as a PNG file. Returns the width and height. |

These read the buffer the display emulation draws into. Lines above the current beam position are
from the frame being drawn and the rest from the frame before, so take screenshots right after
`emu.wait_frames` or `emu.step` to get a whole frame. In warp mode not every frame is drawn, so
turn warp off and wait a few frames before taking a screenshot. Screens of graphics cards (RTG)
are not available.

### state

| Function | Description |
| --- | --- |
| `state.save(path)` | Save a state file. |
| `state.load(path)` | Load a state file. |
| `state.snapshot()` | Save a state in memory and return it. |
| `state.restore(snapshot)` | Load a state returned by `state.snapshot`. It can be loaded many times. |

FS-UAE saves and loads states at the end of a frame. The functions therefore let the emulation run
to the end of the current frame, also when it is paused, and loading returns one frame after the
state was loaded. Lua variables, breakpoints and taps are not part of the state and are kept.

States work with a directory on the host as hard drive, with one caveat. While the file system
of such a drive is handling a request from the Amiga, a state cannot be saved; `state.save` and
`state.snapshot` then wait and try again after each frame (for up to 100 frames). Loading a state
which was saved while a program was reading or writing files on such a drive can leave that
program waiting forever for an answer from the file system. Take snapshots at a point where the
program is not using the drive (for a game, after it has loaded), or use a floppy image or a
hard disk image file, which do not have this problem.

### media

| Function | Description |
| --- | --- |
| `media.insert(drive, path)` | Insert a disk image in drive 0 to 3. As with a real drive, it takes a moment before the Amiga sees the new disk. |
| `media.eject(drive)` | Eject the disk. |
| `media.path(drive)` | The path of the disk image in the drive, or an empty string. |

## Examples

Find out which instruction writes to an address:

```lua
mem.tap_write(0x7f000, 0x7f001, function(address, value, size, pc)
    print(string.format("%08x: write %04x to %08x", pc, value, address))
    print(cpu.disasm(pc)[1].text)
end)
```

Run to an address, look around, and try the next frames twice with different input:

```lua
dbg.bpset(0x24a6c)
local info = dbg.wait()
print(dbg.command("r"))
dbg.bpclear()

local before = state.snapshot()
input.joy(1, "fire", true)
emu.step(50)
video.screenshot("/tmp/with-fire.png")

state.restore(before)
input.joy(1, "fire", false)
emu.step(50)
video.screenshot("/tmp/without-fire.png")
```

Patch a game while it runs: give the player more lives whenever the counter is written.

```lua
mem.tap_write(0x3c012, 0x3c013, function() return 9 end)
```

There are more scripts in `od-fs/docs/lua-examples`.

## Tests

The tests in `od-fs/test/lua` start FS-UAE and control it through the socket. They need a
Kickstart ROM, and the path to it in an environment variable:

```sh
cd od-fs/test/lua
FSUAE_TEST_KICKSTART=/path/to/kickstart.rom ./run_tests.py
```

`run_tests.py` runs the test modules in parallel, each with its own FS-UAE instances, and prints
one line per module. `--jobs N` sets how many run at the same time, and module names can be given
to run only those. A single module or test can also be run with `python3 -m unittest test_debug`
or `python3 -m unittest test_debug.TapTest`.

`FSUAE_TEST_MODEL` selects the Amiga model matching the ROM (`A500`, `A500+`, `A600`, `A1200`,
`A3000` or `A4000`; the default is `A1200`), and `FSUAE_TEST_BINARY` the executable to test
(default `od-fs/fs-uae`). The model is set up with the `quickstart` option, so the tests run with
the standard configuration of that model. `FSUAE_TEST_OPTIONS` adds configuration options to all
tests, for example `FSUAE_TEST_OPTIONS=cpu_model=68040,fpu_model=68040`. Most tests boot a small
disk image which they create themselves. `test_public_disk.py` downloads the free operating system
EmuTOS and boots that. `test_symbols.py` builds a program with vasm and vlink and puts it on a
disk with xdftool (from amitools); it is skipped if those tools are not installed.

The tests start FS-UAE with `--headless`, so no windows appear. Set `FSUAE_TEST_FOREGROUND=1` to
see them; they are then started with SDL hints which keep them from taking the keyboard focus
(`SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0`, `SDL_WINDOW_ACTIVATE_WHEN_RAISED=0` and, on macOS,
`SDL_MAC_BACKGROUND_APP=1`).

## Running fast

`emu.warp(true)` runs the emulation as fast as the host allows. How fast that is depends mostly
on how exact the emulation is. Measured on an Apple Silicon Mac, with a demo running:

| Configuration | Speed in warp mode |
| --- | --- |
| A500 or A1200, standard (cycle-exact) | about 480 frames per second (9 times real time) |
| A500 with `cycle_exact=false` | about 800 frames per second |
| A1200 with `cycle_exact=false` and `cpu_compatible=false` | about 880 frames per second |

Use the exact settings when the timing of the program matters, and the faster ones for tests of
logic which does not depend on it. Other ways to save time:

- Take a snapshot (`state.snapshot`) when the program has started, and restore it for each test
  instead of booting again.
- Remove breakpoints when they are no longer needed. While any breakpoint is set, the addresses
  are checked before every instruction, which cost about 7% on an A500 and 20% on an A1200 with a
  program in a tight loop. The number of breakpoints does not matter. A tap only costs when memory
  in the same 64 KB block is accessed.

## Running without a visible window

With `--headless` on the command line, or `headless=true` in the `.uae` file, FS-UAE creates its
window without showing it:

```sh
od-fs/fs-uae --headless config.uae
```

The emulation runs as usual and at the same speed, and the `video` functions work, since they
read the picture from the emulation and not from the window. Sound is not affected; turn it off
separately if it should be quiet. The only way to control a headless FS-UAE is through Lua (the
`lua` and `lua_port` options), and it quits with `emu.quit()` or when its process is ended.

`headless` is the name WinUAE uses for the same thing. It is a hidden window, so FS-UAE still
needs a graphical session to start in; it cannot run on a machine without a display.

## Running several instances

FS-UAE only starts once per user: a second copy finds the lock file of the first in the directory
for temporary files and quits. To run several at the same time, for example to test in parallel,
give each its own directory in `TMPDIR` (and its own `lua_port`):

```sh
mkdir -p /tmp/fs-uae-1 /tmp/fs-uae-2
TMPDIR=/tmp/fs-uae-1 od-fs/fs-uae one.uae &
TMPDIR=/tmp/fs-uae-2 od-fs/fs-uae two.uae &
```

The test harness does this for every emulator it starts.

## Limitations

- FS-UAE needs a graphical session to run in, also with `--headless`, which only hides the window.
- While the emulation is stopped, the window shows the last frame. The emulated screen is not
  redrawn when memory is changed.
- After a state has been loaded (`state.load` or `state.restore`), the frame is two lines lower
  than before: `video.size()` returns 756x574 instead of 756x576 on a PAL A1200 configuration, and
  screenshots get that size. The lines which remain are the same, so compare screenshots taken
  before and after a load line by line from the top, not as whole files. The cause has not been
  found; it is in how the display is set up after a restore, not in the Lua functions.
- Lua runs on the emulation thread. A script which loops without waiting stops the emulation.
- Breakpoints and instruction steps have not been tested with the JIT compiler (x86 only).
- When the CPU runs at maximum speed (the default for the A3000 and A4000 configurations), how
  much the CPU gets done per frame depends on the host, so runs are not exactly repeatable. After
  restoring a state, a program can be a frame ahead of or behind where it was the last time.
- The tests have been run on macOS only, with these models and Kickstart ROMs: A500 (1.3 and
  3.1), A500+ (2.04), A600 (2.05), A1200 (3.1), A3000 (2.04) and A4000 (3.1). The A4000 was also
  run with a 68040 and a 68060, each with and without MMU emulation and the "more compatible"
  setting.
