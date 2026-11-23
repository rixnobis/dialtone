-- Plays dialtone with the pad: dials the first station by hand, presses redial and buy, cycles the shop,
-- tunes up once it can. Screenshots at the vsyncs listed in SHOTS (comma separated) into SHOTDIR,
-- quits at QUIT_AT.
-- pcsx-redux -run -lua_stdout -dofile tools/autoplay.lua -loadexe dialtone.ps-exe
local B = { Select = 0, Up = 4, Right = 5, Down = 6, Left = 7, L1 = 10, R1 = 11, Tri = 12, Circle = 13, Cross = 14, Square = 15 }
local shotdir = os.getenv('SHOTDIR') or '.'
local quitAt = tonumber(os.getenv('QUIT_AT') or '18000')
local shots = {}
for s in string.gmatch(os.getenv('SHOTS') or '', '[^,]+') do shots[tonumber(s)] = true end

-- 555-0188 from the '1' key: cursor walks the 3x4 pad
local dial = { 'Right', 'Down', 'Cross', 'Cross', 'Cross', 'Down', 'Down', 'Cross',
  'Up', 'Up', 'Up', 'Left', 'Cross', 'Right', 'Down', 'Down', 'Cross', 'Cross',
  'Up', 'Up', 'Left' }
local plan = {}
for _, b in ipairs(dial) do table.insert(plan, b) end
-- redial and buy, and every other pass step the shop cursor on so the stats get bought too
for _ = 1, 4 do
  for _, b in ipairs({ 'Tri', 'Square', 'Tri', 'Square', 'Select' }) do table.insert(plan, b) end
end

-- BUY_ONLY=1: press Square a few times, then leave the pad alone (the autodialer test)
local buyOnly = os.getenv('BUY_ONLY') == '1'
if buyOnly then plan = { 'Square' } end

local n, step, held = 0, 1, nil
listener = PCSX.Events.createEventListener('GPU::Vsync', function()
  n = n + 1
  local pad = PCSX.SIO0.slots[1].pads[1]
  if n > 120 and n % 10 == 0 and not (buyOnly and n > 300) then
    if held then pad.clearOverride(B[held]); held = nil
    else
      held = plan[step]
      -- after the first station opens up, flip to it every few cycles and back
      if n > 3600 and step == 1 and n % 1200 < 10 then held = 'R1' end
      pad.setOverride(B[held])
      step = step % #plan + 1
    end
  end
  if shots[n] then
    local ss = PCSX.GPU.takeScreenShot()
    local fh = Support.File.open(string.format('%s/v%05d.raw', shotdir, n), 'TRUNCATE')
    fh:writeMoveSlice(ss.data); fh:close()
    print(string.format('SHOT %d %dx%d bpp=%s', n, tonumber(ss.width), tonumber(ss.height), tostring(ss.bpp)))
  end
  if n == quitAt then PCSX.quit(0) end
end)
