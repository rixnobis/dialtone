-- Plays dialtone with the pad: dials the first station by hand, presses redial and buy, cycles the shop,
-- tunes up once it can. Screenshots at the vsyncs listed in SHOTS (comma separated) into SHOTDIR,
-- quits at QUIT_AT.
-- SHOT_EVERY=N adds a screenshot every N vsyncs.
-- pcsx-redux -run -lua_stdout -dofile tools/autoplay.lua -loadexe dialtone.ps-exe
local B = { Select = 0, Start = 3, Up = 4, Right = 5, Down = 6, Left = 7, L2 = 8, R2 = 9, L1 = 10, R1 = 11, Tri = 12, Circle = 13, Cross = 14, Square = 15 }
local shotdir = os.getenv('SHOTDIR') or '.'
local quitAt = tonumber(os.getenv('QUIT_AT') or '18000')
local shotEvery = tonumber(os.getenv('SHOT_EVERY') or '0')
local shots = {}
for s in string.gmatch(os.getenv('SHOTS') or '', '[^,]+') do shots[tonumber(s)] = true end

-- 555-0188 from the '1' key: cursor walks the 3x4 pad
local dial = { 'Right', 'Down', 'Cross', 'Cross', 'Cross', 'Down', 'Down', 'Cross',
  'Up', 'Up', 'Up', 'Left', 'Cross', 'Right', 'Down', 'Down', 'Cross', 'Cross',
  'Up', 'Up', 'Left' }
local plan = {}
for _, b in ipairs(dial) do table.insert(plan, b) end
-- redial, open the shop, buy twice, step the cursor on so the stats get bought too, close it
for _ = 1, 4 do
  for _, b in ipairs({ 'Tri', 'Select', 'Square', 'Square', 'Down', 'Select', 'Tri' }) do table.insert(plan, b) end
end

-- BUY_ONLY=1: buy the desk items and autodialer levels, then leave the pad alone (the autodialer test)
local buyOnly = os.getenv('BUY_ONLY') == '1'
-- shop (SELECT opens it): 1 the next desk item, 2 the autodialer, then the stats
if buyOnly then plan = { 'Select', 'Square', 'Square', 'Down', 'Square', 'Square', 'Square', 'Square', 'Square', 'Select' } end

-- SEQ="400:Start,430:Cross": press these buttons at these vsyncs (held 6 frames); NOPLAN=1 turns
-- the dialing plan off so only SEQ touches the pad
local seq = {}
for v, b in string.gmatch(os.getenv('SEQ') or '', '(%d+):(%a+%d?)') do seq[tonumber(v)] = b end
local noPlan = os.getenv('NOPLAN') == '1'
local seqHeld = {}

local n, step, held = 0, 1, nil
listener = PCSX.Events.createEventListener('GPU::Vsync', function()
  n = n + 1
  local pad = PCSX.SIO0.slots[1].pads[1]
  -- START dismisses the how-to-play card
  if n == 100 then pad.setOverride(B.Start) elseif n == 106 then pad.clearOverride(B.Start) end
  if seq[n] then pad.setOverride(B[seq[n]]); seqHeld[n + 6] = seq[n]; print('PRESS ' .. n .. ' ' .. seq[n]) end
  if seqHeld[n] then pad.clearOverride(B[seqHeld[n]]); seqHeld[n] = nil end
  if n > 120 and n % 10 == 0 and not (buyOnly and n > 120 + 40 * #plan) and not noPlan then
    if held then pad.clearOverride(B[held]); held = nil
    else
      held = plan[step]
      -- after the first station opens up, flip to it every few cycles and back
      if n > 3600 and step == 1 and n % 1200 < 10 then held = 'R1' end
      pad.setOverride(B[held])
      step = step % #plan + 1
    end
  end
  if shots[n] or (shotEvery > 0 and n % shotEvery == 0) then
    local ss = PCSX.GPU.takeScreenShot()
    local fh = Support.File.open(string.format('%s/v%05d.raw', shotdir, n), 'TRUNCATE')
    fh:writeMoveSlice(ss.data); fh:close()
    print(string.format('SHOT %d %dx%d bpp=%s', n, tonumber(ss.width), tonumber(ss.height), tostring(ss.bpp)))
  end
  if n == quitAt then PCSX.quit(0) end
end)
