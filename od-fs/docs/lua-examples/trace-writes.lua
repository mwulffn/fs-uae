-- Logs every CPU write to a range of memory, with the instruction doing it.
--
-- Send it to a running FS-UAE after setting the range:
--
--   fsuae_lua.py 'trace_first, trace_last = 0x7f000, 0x7f003'
--   fsuae_lua.py --file trace-writes.lua
--
-- The writes are printed to the FS-UAE log, and sent to socket clients as
-- "print" events. Run mem.tap_remove(trace_tap) to stop.

assert(trace_first and trace_last, "set trace_first and trace_last first")

trace_tap = mem.tap_write(trace_first, trace_last, function(address, value, size, pc)
    local instruction = cpu.disasm(pc)[1].text
    print(string.format("%08x  %-30s  %08x <- %0" .. size * 2 .. "x", pc, instruction, address, value))
end)

return trace_tap
