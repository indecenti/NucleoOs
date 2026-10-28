-- nucleo.lua — il kit di gioco di NucleoOS per LÖVE 11.x (GPL-3.0-or-later, parte di NucleoOS).
--
-- ANIMA lo mette dentro ogni gioco che crea; un gioco scritto a mano può usarlo uguale:
--     local N = require("nucleo")
-- Tutto è pensato per le console di NucleoOS: schermo 640x480 o 720x720 (lo si legge, non si presume),
-- pad fisico che arriva come TASTI (gptokeyb, /opt/love2d/love2d.gptk: su R39Max il joystick è
-- "play_joystick" e isGamepad() è false), con l'API gamepad SDL come ripiego dove c'è, un altoparlante
-- piccolo. L'imbracatura di ANIMA (main.lua) chiama N._frame e N._draw da sola: particelle,
-- tremolio, tween, timer e "pressed" funzionano senza che il gioco ci pensi.
local N = { version = 1 }

-- ── schermo ────────────────────────────────────────────────────────────────────────────────────────────
-- N.W, N.H: pixel veri. N.U: un'"unità" = 1/100 del lato corto: dimensioni in N.U escono giuste su
-- 640x480 come su 720x720. N.cx, N.cy: il centro.
function N.refresh()
  N.W, N.H = love.graphics.getDimensions()
  N.SW, N.SH = N.W, N.H
  N.U = math.min(N.W, N.H) / 100
  N.cx, N.cy = N.W / 2, N.H / 2
end
N.refresh()

-- Un mondo a dimensione fissa (vw x vh) ingrandito nello schermo, con bande se le proporzioni non coincidono:
--   N.fit(320, 240) una volta, poi in love.draw: N.push() ... disegna in 0..320 x 0..240 ... N.pop()
-- Dopo N.fit, N.W/N.H/N.U/N.cx/N.cy sono quelli del MONDO (vw x vh): il gioco disegna nelle sue coordinate e
-- N.text, le particelle e il tremolio escono in proporzione. Prima restavano quelli dello schermo vero e, dentro
-- N.push (che ingrandisce), ogni misura in N.U veniva ingrandita due volte: testo enorme e tagliato (provato,
-- 21/10/2026). Lo schermo vero resta in N.SW, N.SH.
function N.fit(vw, vh)
  N.SW, N.SH = love.graphics.getDimensions()
  N.VW, N.VH = vw, vh
  local s = math.min(N.SW / vw, N.SH / vh)
  N.FS, N.FX, N.FY = s, math.floor((N.SW - vw * s) / 2), math.floor((N.SH - vh * s) / 2)
  N.W, N.H, N.U, N.cx, N.cy = vw, vh, math.min(vw, vh) / 100, vw / 2, vh / 2
end
-- N._autofit: l'imbracatura di ANIMA applica lei l'ingrandimento a ogni fotogramma (anche quando il gioco si
-- dimentica N.push: provato, il mondo restava piccolo nell'angolo), e allora i N.push/N.pop del gioco non fanno niente.
N._autofit = false
function N.push()
  if N._autofit then return end
  love.graphics.push()
  love.graphics.translate(N.FX or 0, N.FY or 0)
  love.graphics.scale(N.FS or 1)
  if N.VW then love.graphics.setScissor(N.FX, N.FY, N.VW * N.FS, N.VH * N.FS) end
end
function N.pop()
  if N._autofit then return end
  love.graphics.setScissor()
  love.graphics.pop()
end

-- ── comandi ────────────────────────────────────────────────────────────────────────────────────────────
-- Nomi dei pulsanti come sono stampati sulla console: a b x y start select l1 r1 l2 r2 up down left right.
-- Tasti: quelli che gptokeyb produce sulle console (love2d.gptk) più le frecce e i tasti di un PC.
local KEYS = {
  a = { "z", "j" }, b = { "lshift", "rshift", "k" }, x = { "space" }, y = { "b" },
  start = { "return", "kpenter" }, select = { "escape", "backspace" },
  l1 = { "l", "q" }, r1 = { "r", "e" }, l2 = { "x" }, r2 = { "y" },
  up = { "w", "up" }, down = { "s", "down" }, left = { "a", "left" }, right = { "d", "right" },
}
local PADS = {
  a = "a", b = "b", x = "x", y = "y", start = "start", select = "back", l1 = "leftshoulder", r1 = "rightshoulder",
  up = "dpup", down = "dpdown", left = "dpleft", right = "dpright",
}
local now, before = {}, {}
local function pad()
  local js = love.joystick and love.joystick.getJoysticks() or {}
  for _, j in ipairs(js) do if j:isGamepad() then return j end end
end
local function raw(b)
  for _, k in ipairs(KEYS[b] or {}) do if love.keyboard.isDown(k) then return true end end
  local j = pad()
  if j then
    if PADS[b] and j:isGamepadDown(PADS[b]) then return true end
    if b == "l2" then return j:getGamepadAxis("triggerleft") > 0.5 end
    if b == "r2" then return j:getGamepadAxis("triggerright") > 0.5 end
    local ax, ay = j:getGamepadAxis("leftx"), j:getGamepadAxis("lefty")
    if b == "left" then return ax < -0.5 end
    if b == "right" then return ax > 0.5 end
    if b == "up" then return ay < -0.5 end
    if b == "down" then return ay > 0.5 end
  end
  return false
end
function N.down(b) return now[b] == true end                     -- tenuto premuto
function N.pressed(b) return now[b] == true and not before[b] end  -- appena premuto (un fotogramma)
function N.released(b) return before[b] == true and not now[b] end
-- direzione da croce o levetta: dx, dy in -1..1 (diagonale normalizzata)
function N.axis()
  local dx = (N.down("right") and 1 or 0) - (N.down("left") and 1 or 0)
  local dy = (N.down("down") and 1 or 0) - (N.down("up") and 1 or 0)
  local j = pad()
  if j and dx == 0 and dy == 0 then
    local ax, ay = j:getGamepadAxis("leftx"), j:getGamepadAxis("lefty")
    if math.abs(ax) > 0.2 then dx = ax end
    if math.abs(ay) > 0.2 then dy = ay end
  end
  local l = math.sqrt(dx * dx + dy * dy)
  if l > 1 then dx, dy = dx / l, dy / l end
  return dx, dy
end
function N.rumble(strength, sec)
  local j = pad()
  if j and j.setVibration then pcall(j.setVibration, j, strength or 0.5, strength or 0.5, sec or 0.15) end
end

-- ── suoni ──────────────────────────────────────────────────────────────────────────────────────────────
-- N.sfx("coin"): un effetto del catalogo (sfx/<id>.wav, generati da tools/gen-game-sfx.py). Più copie
-- possono suonare insieme. N.music("action"): un loop di music/; N.music() lo ferma.
-- Un id che non c'è non rompe il gioco: suona un bip sintetizzato al suo posto.
local cache, musicSrc = {}, nil
local function load(path)
  if cache[path] == nil then
    local ok, src = pcall(love.audio.newSource, path, "static")
    cache[path] = ok and src or false
  end
  return cache[path]
end
function N.sfx(id, vol, pitch)
  local s = load("sfx/" .. id .. ".wav")
  if not s then return N.beep(660, 0.08) end
  s = s:clone()
  s:setVolume(vol or 1); s:setPitch(pitch or 1); s:play()
  return s
end
function N.music(id, vol)
  if musicSrc then musicSrc:stop(); musicSrc = nil end
  if not id then return end
  local ok, src = pcall(love.audio.newSource, "music/" .. id .. ".wav", "stream")
  if not ok then return end
  src:setLooping(true); src:setVolume(vol or 0.6); src:play()
  musicSrc = src
end
-- Un bip sintetizzato al volo: wave = "square" | "sine" | "saw" | "noise"; slide = Hz/s (salita o discesa).
function N.beep(freq, dur, wave, vol, slide)
  local rate, n = 22050, math.floor(22050 * (dur or 0.1))
  local sd = love.sound.newSoundData(math.max(n, 1), rate, 16, 1)
  local ph, f = 0, freq or 440
  for i = 0, n - 1 do
    local t = i / rate
    local env = math.min(1, i / 60) * (1 - i / n)
    local v
    if wave == "sine" then v = math.sin(ph * 2 * math.pi)
    elseif wave == "saw" then v = 2 * (ph - math.floor(ph + 0.5))
    elseif wave == "noise" then v = love.math.random() * 2 - 1
    else v = (ph % 1 < 0.5) and 0.6 or -0.6 end
    sd:setSample(i, v * env * (vol or 0.5))
    ph = ph + (f + (slide or 0) * t) / rate
  end
  local s = love.audio.newSource(sd, "static")
  s:play()
  return s
end

-- ── testo e colori ─────────────────────────────────────────────────────────────────────────────────────
local fonts = {}
-- k: quanto il mondo è ingrandito (N.fit). Il carattere si crea già a quella grandezza e si disegna ridotto di
-- k: dentro N.push torna alla misura giusta ma resta nitido invece di sgranarsi.
function N.font(size, k)
  local px = math.max(8, math.floor((size or 4) * N.U * (k or 1)))
  if not fonts[px] then fonts[px] = love.graphics.newFont(px) end
  return fonts[px]
end
-- N.text("Ciao", x, y, 6, "center"): size in N.U; align "left"|"center"|"right" (x è il punto d'aggancio)
-- size in N.U (di solito 3..10). Da 14 in su la si prende per PIXEL: è come i modelli la scrivono di
-- solito (N.text("SNAKE", x, y, 40): provato, usciva un titolo largo tre schermi).
function N.text(s, x, y, size, align, color)
  local k = (N.VW and N.FS and N.FS > 1) and N.FS or 1
  if size and size >= 14 then size = size / N.U end
  local f = N.font(size, k)
  love.graphics.setFont(f)
  if color then N.color(color) end
  s = tostring(s)
  local w = f:getWidth(s) / k
  local ox = align == "center" and w / 2 or align == "right" and w or 0
  love.graphics.print(s, x - ox, y, 0, 1 / k, 1 / k)
end
-- 16 colori chiari e leggibili sullo schermo piccolo (palette di NucleoOS)
N.palette = {
  black = { 0.06, 0.07, 0.10 }, navy = { 0.11, 0.17, 0.33 }, purple = { 0.49, 0.15, 0.55 }, green = { 0.0, 0.53, 0.32 },
  brown = { 0.67, 0.32, 0.21 }, gray = { 0.37, 0.34, 0.31 }, silver = { 0.76, 0.76, 0.78 }, white = { 1.0, 0.95, 0.91 },
  red = { 1.0, 0.0, 0.30 }, orange = { 1.0, 0.64, 0.0 }, yellow = { 1.0, 0.93, 0.15 }, lime = { 0.0, 0.89, 0.21 },
  blue = { 0.16, 0.68, 1.0 }, lavender = { 0.51, 0.46, 0.61 }, pink = { 1.0, 0.47, 0.66 }, peach = { 1.0, 0.8, 0.67 },
}
-- N.strict: in prova (l'imbracatura di ANIMA lo accende) un argomento sbagliato è un errore chiaro A NOME DELLA
-- RIGA DEL GIOCO (error(..., 2)), così chi corregge vede game.lua:N e non nucleo.lua; nel gioco vero, invece, si
-- ripiega su qualcosa di sensato e si continua a giocare. Il caso vero: N.color(C_BLACK) con C_BLACK mai definito.
N.strict = false
function N.color(c, a)
  if type(c) == "string" then
    if not N.palette[c] and N.strict then error('N.color: no colour named "' .. c .. '" (use a palette name or {r,g,b})', 2) end
    c = N.palette[c] or N.palette.white
  end
  if type(c) ~= "table" then
    if N.strict then error("N.color: the colour is " .. tostring(c) .. " (an undefined variable? use a palette name or {r,g,b})", 2) end
    c = N.palette.white
  end
  local r, g, b, al = c[1] or c.r or 1, c[2] or c.g or 1, c[3] or c.b or 1, c[4] or c.a
  if r > 1 or g > 1 or b > 1 then r, g, b = r / 255, g / 255, b / 255; if al and al > 1 then al = al / 255 end end   -- 0..255
  love.graphics.setColor(r, g, b, a or al or 1)
end

-- ── disegno: le forme di love.graphics, con o senza "fill"/"line" ──────────────────────────────────────
-- I modelli le cercano qui (N.circle, N.rect...: provato, qwen3.5:9b le usava tre volte di fila anche dopo
-- la correzione). Costano niente e chiudono un'intera famiglia di errori. Senza modo = "fill".
-- Accettano anche un COLORE come ultimo argomento (nome della palette o {r,g,b[,a]}): N.rect(x,y,w,h,"red")
-- è il modo in cui i modelli le chiamano spontaneamente (provato: "bad argument #6 to 'rectangle'").
-- Un colore: un nome, {r,g,b[,a]} o {r=,g=,b=[,a=]} (0..1 o 0..255). Una tabella di 3-4 numeri non può essere
-- una lista di vertici (un poligono ne vuole almeno 6), quindi è un colore.
local function isColor(v)
  if type(v) == "string" then return v ~= "fill" and v ~= "line" end
  if type(v) ~= "table" then return false end
  if type(v.r) == "number" and type(v.g) == "number" and type(v.b) == "number" then return true end
  if #v ~= 3 and #v ~= 4 then return false end
  for i = 1, #v do if type(v[i]) ~= "number" then return false end end
  return true
end
local function shape(fn)
  return function(...)
    local a = { ... }
    if #a > 0 and isColor(a[#a]) then
      local c = table.remove(a)
      local r, g, b, al = love.graphics.getColor()
      N.color(c)
      local mode = (a[1] == "fill" or a[1] == "line") and table.remove(a, 1) or "fill"
      fn(mode, unpack(a))
      love.graphics.setColor(r, g, b, al)
      return
    end
    if a[1] == "fill" or a[1] == "line" then return fn(...) end
    return fn("fill", ...)
  end
end
N.rect = shape(function(m, x, y, w, h, r) love.graphics.rectangle(m, x, y, w, h, r or 0) end)
N.rectangle = N.rect
N.circle = shape(function(m, x, y, r) love.graphics.circle(m, x, y, r) end)
N.ellipse = shape(function(m, x, y, rx, ry) love.graphics.ellipse(m, x, y, rx, ry or rx) end)
N.polygon = shape(function(m, ...) love.graphics.polygon(m, ...) end)
N.triangle = shape(function(m, x1, y1, x2, y2, x3, y3) love.graphics.polygon(m, x1, y1, x2, y2, x3, y3) end)
-- N.line(x1,y1,x2,y2,... [,width] [,color]): "line"/"fill" ovunque si ignorano (provato: N.line(x,0,x,H,"line","gray")),
-- un numero dispari di valori vuol dire che l'ultimo è lo spessore.
function N.line(...)
  local a, color = {}, nil
  local args = { ... }
  if #args > 0 and isColor(args[#args]) then color = table.remove(args) end
  for _, v in ipairs(args) do
    if type(v) == "table" then for _, n in ipairs(v) do a[#a + 1] = n end
    elseif type(v) == "number" then a[#a + 1] = v end
  end
  local width
  if #a % 2 == 1 then width = table.remove(a) end
  if #a < 4 then return end
  local r, g, b, al = love.graphics.getColor()
  local lw = love.graphics.getLineWidth()
  if color then N.color(color) end
  if width then love.graphics.setLineWidth(width) end
  love.graphics.line(unpack(a))
  love.graphics.setLineWidth(lw)
  if color then love.graphics.setColor(r, g, b, al) end
end
function N.translate(x, y) love.graphics.translate(x, y) end
function N.rotate(a) love.graphics.rotate(a) end
function N.scale(sx, sy) love.graphics.scale(sx, sy or sx) end

-- ── matematica e collisioni ────────────────────────────────────────────────────────────────────────────
function N.time() return love.timer.getTime() end                -- secondi da un istante qualsiasi (per durate)
function N.clamp(v, a, b) return math.max(a, math.min(b, v)) end
function N.round(v) return math.floor(v + 0.5) end
function N.sign(v) return v > 0 and 1 or (v < 0 and -1 or 0) end
function N.lerp(a, b, t) return a + (b - a) * t end
function N.dist(x1, y1, x2, y2) return math.sqrt((x2 - x1) ^ 2 + (y2 - y1) ^ 2) end
function N.rand(a, b) if b then return a + love.math.random() * (b - a) end return love.math.random() * (a or 1) end
function N.choose(t) return t[love.math.random(#t)] end
-- rettangoli {x,y,w,h} o cerchi {x,y,r}
function N.overlap(a, b) return a.x < b.x + b.w and b.x < a.x + a.w and a.y < b.y + b.h and b.y < a.y + a.h end
function N.circles(a, b) return N.dist(a.x, a.y, b.x, b.y) < a.r + b.r end

-- ── tempo: timer e tween ───────────────────────────────────────────────────────────────────────────────
local timers, tweens = {}, {}
function N.after(sec, fn) local t = { t = sec, fn = fn }; table.insert(timers, t); return t end
function N.every(sec, fn) local t = { t = sec, fn = fn, every = sec }; table.insert(timers, t); return t end
function N.cancel(t) for i = #timers, 1, -1 do if timers[i] == t then table.remove(timers, i) end end end
local EASE = {
  linear = function(t) return t end,
  out = function(t) return 1 - (1 - t) ^ 3 end,
  inout = function(t) return t < 0.5 and 4 * t ^ 3 or 1 - (-2 * t + 2) ^ 3 / 2 end,
  back = function(t) local c = 1.70158; return 1 + (c + 1) * (t - 1) ^ 3 + c * (t - 1) ^ 2 end,
  bounce = function(t) local n, d = 7.5625, 2.75
    if t < 1 / d then return n * t * t elseif t < 2 / d then t = t - 1.5 / d; return n * t * t + 0.75
    elseif t < 2.5 / d then t = t - 2.25 / d; return n * t * t + 0.9375 end
    t = t - 2.625 / d; return n * t * t + 0.984375 end,
}
-- N.tween(obj, 0.4, {x = 100, alpha = 0}, "out", function() ... end)
function N.tween(obj, dur, target, ease, done)
  local from = {}
  for k in pairs(target) do from[k] = obj[k] end
  table.insert(tweens, { o = obj, d = dur, t = 0, from = from, to = target, e = EASE[ease or "out"] or EASE.out, done = done })
end

-- ── effetti: particelle e tremolio ─────────────────────────────────────────────────────────────────────
local parts, shake = {}, 0
-- N.burst(x, y, "orange", 20): scintille che si spargono e cadono
function N.burst(x, y, color, n, speed)
  local c = type(color) == "string" and (N.palette[color] or N.palette.white) or color or N.palette.white
  for _ = 1, n or 16 do
    local a, v = N.rand(0, math.pi * 2), N.rand(0.3, 1) * (speed or 40) * N.U
    table.insert(parts, { x = x, y = y, vx = math.cos(a) * v, vy = math.sin(a) * v, life = N.rand(0.3, 0.8), max = 0.8, c = c, s = N.rand(0.4, 1) * N.U })
  end
end
function N.shake(amount) shake = math.max(shake, (amount or 1) * N.U) end

-- ── i file del gioco: record, salvataggi, registro ─────────────────────────────────────────────────────
-- NON nella cartella di salvataggio di LÖVE: su queste console è un collegamento verso la scheda exFAT in
-- cui love.filesystem.write fallisce (torna false, senza errore: misurato su R39Max il 21/10/2026, LÖVE
-- 11.4). Si scrive con io accanto al gioco: <cartella del .love>/.anima/<identità>/ (sulla scheda, quindi
-- leggibile anche dal browser e dentro i backup).
N.DATA = (love.filesystem.getSourceBaseDirectory() or ".") .. "/.anima/" .. (love.filesystem.getIdentity() or "game")
local madeData = false
local function datadir()
  if not madeData then os.execute('mkdir -p "' .. N.DATA .. '" 2>/dev/null'); madeData = true end
  return N.DATA
end
function N.write(name, data, mode)
  local f = io.open(datadir() .. "/" .. name, mode or "wb")
  if not f then return false end
  f:write(data); f:close()
  return true
end
function N.read(name)
  local f = io.open(N.DATA .. "/" .. name, "rb")
  if not f then return nil end
  local s = f:read("*a"); f:close()
  return s
end
function N.save(key, value) return N.write("save-" .. key .. ".txt", tostring(value)) end
function N.load(key, default)
  local s = N.read("save-" .. key .. ".txt")
  if s == nil or s == "" then return default end
  return tonumber(s) or s
end

-- ── scene ──────────────────────────────────────────────────────────────────────────────────────────────
-- N.scene(menu) dove menu = { enter = fn, update = fn(dt), draw = fn }: l'imbracatura le chiama da sola
-- quando il gioco non definisce love.update / love.draw.
N.current = nil
function N.scene(s, ...)
  N.current = s
  if s and s.enter then s.enter(...) end
end

-- ── chiamati dall'imbracatura a ogni fotogramma ────────────────────────────────────────────────────────
-- pulsanti "premuti" da chi prova il gioco senza mani (l'imbracatura di ANIMA in modalità prova)
local injected = {}
function N._inject(b, frames) injected[b] = frames or 2 end
function N._input()
  before, now = now, {}
  for b in pairs(KEYS) do
    now[b] = raw(b)
    if injected[b] and injected[b] > 0 then now[b] = true; injected[b] = injected[b] - 1 end
  end
end
function N._frame(dt)
  for i = #timers, 1, -1 do
    local t = timers[i]
    t.t = t.t - dt
    if t.t <= 0 then
      t.fn()
      if t.every then t.t = t.t + t.every else table.remove(timers, i) end
    end
  end
  for i = #tweens, 1, -1 do
    local w = tweens[i]
    w.t = math.min(w.d, w.t + dt)
    local k = w.e(w.d > 0 and w.t / w.d or 1)
    for key, v in pairs(w.to) do w.o[key] = w.from[key] + (v - w.from[key]) * k end
    if w.t >= w.d then table.remove(tweens, i); if w.done then w.done() end end
  end
  for i = #parts, 1, -1 do
    local p = parts[i]
    p.life = p.life - dt
    p.vy = p.vy + 60 * N.U * dt
    p.x, p.y = p.x + p.vx * dt, p.y + p.vy * dt
    if p.life <= 0 then table.remove(parts, i) end
  end
  shake = math.max(0, shake - 30 * N.U * dt)
end
function N._predraw()
  if shake > 0 then love.graphics.translate(N.rand(-shake, shake), N.rand(-shake, shake)) end
end
function N._draw()
  for _, p in ipairs(parts) do
    love.graphics.setColor(p.c[1], p.c[2], p.c[3], math.max(0, p.life / p.max))
    love.graphics.rectangle("fill", p.x, p.y, p.s, p.s)
  end
  love.graphics.setColor(1, 1, 1, 1)
end

return N
