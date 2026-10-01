-- Boots in warp mode, saves a screenshot and quits.
--
-- Use it as a startup script (lua=/path/to/screenshot-after-boot.lua). The
-- number of frames to wait and the name of the screenshot can be changed
-- with the environment variables BOOT_FRAMES and SCREENSHOT.

local frames = tonumber(os.getenv("BOOT_FRAMES")) or 1500
local path = os.getenv("SCREENSHOT") or "screenshot.png"

emu.warp(true)
emu.wait_frames(frames)
-- Not every frame is drawn in warp mode, so draw a few at normal speed.
emu.warp(false)
emu.wait_frames(5)

local width, height = video.screenshot(path)
emu.log(string.format("Saved %s (%dx%d) at frame %d", path, width, height, emu.frame()))
emu.quit()
