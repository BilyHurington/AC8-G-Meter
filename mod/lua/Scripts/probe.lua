-- Read-only discovery of game state fields (Ctrl+F7 starts / stops, 180 s at most).
-- Lists the reflected properties of the aircraft, controller, HUD, game mode and game state
-- whose names suggest mission flow or flight data, then logs every change of their values at
-- 10 Hz to probe-<time>.txt in the mod folder. Used to find the mission start / results /
-- debriefing signals and any built-in G or blackout value. Never writes to the game.
local M = {}

local LIMIT_S, PERIOD_S, MAX_FIELDS = 180, 0.1, 512
local requested = false
local file, started, next_sample, watched, subject

local KEYWORDS = { "mission", "phase", "state", "result", "debrief", "clear", "fail", "complete", "pause",
    "black", "red", "highg", "load", "force", "accel", "stall", "speed", "altitude", "throttle", "time",
    "dead", "destroy", "respawn", "active", "hud", "mode", "sortie", "score", "ready", "start", "end" }

local function interesting(name)
    local s = name:lower()
    if s == "g" or s:find("^g_") or s:find("gforce") then return true end
    for _, k in ipairs(KEYWORDS) do
        if s:find(k, 1, true) then return true end
    end
    return false
end

local SCALAR = { BoolProperty = true, ByteProperty = true, EnumProperty = true, IntProperty = true,
    Int64Property = true, FloatProperty = true, DoubleProperty = true, NameProperty = true,
    ObjectProperty = true, WeakObjectProperty = true }

local function write(line) file:write(line, "\n") end

local function inspect(label, obj)
    if not obj or not obj:IsValid() then
        write("MISSING " .. label)
        return
    end
    write("OBJECT " .. label .. " " .. obj:GetFullName())
    local seen = {}
    local class = obj:GetClass()
    while class and class:IsValid() do
        write("CLASS " .. class:GetFullName())
        class:ForEachFunction(function(fn)
            if interesting(fn:GetFName():ToString()) then write("FUNCTION " .. fn:GetFullName()) end
        end)
        class:ForEachProperty(function(p)
            local name = p:GetFName():ToString()
            if seen[name] or not interesting(name) then return end
            seen[name] = true
            local full = p:GetFullName()
            write("PROPERTY " .. full)
            local kind = full:match("^(%w+) ")
            if SCALAR[kind] and #watched < MAX_FIELDS then
                watched[#watched + 1] = { object = obj, name = name, label = label .. "." .. name, kind = kind }
            end
        end)
        class = class:GetSuperStruct()
    end
end

local function value(field)
    if not field.object:IsValid() then return "<owner gone>" end
    local v = field.object[field.name]
    if v == nil then return "nil" end
    if field.kind == "WeakObjectProperty" then
        v = v:Get()
        if not v then return "<none>" end
    end
    if field.kind:find("Object") then return v:IsValid() and v:GetFullName() or "<none>" end
    if field.kind == "NameProperty" then return v:ToString() end
    if type(v) == "number" then
        if field.kind == "FloatProperty" or field.kind == "DoubleProperty" then return string.format("%.3f", v) end
        return tostring(v)
    end
    if type(v) == "boolean" or type(v) == "string" then return tostring(v) end
    local ok, n = pcall(function() return v:get() end)
    if ok and n ~= nil then return tostring(n) end
    return "<" .. type(v) .. ">"
end

local function stop(reason)
    if file then
        write("STOP " .. reason)
        file:close()
        file = nil
        print("[AC8GMeter] Probe stopped: " .. reason .. "\n")
    end
end

function M.request() requested = true end

function M.update(pawn, controller, statics, directory)
    if not requested and not file then return end
    local ok, err = pcall(function()
        if requested then
            requested = false
            if file then
                stop("Ctrl+F7")
                return
            end
            if not pawn then  -- without an aircraft a level load could free the watched objects
                print("[AC8GMeter] Probe needs an aircraft: start it in flight.\n")
                return
            end
            local path = directory .. "..\\probe-" .. os.date("%Y%m%d-%H%M%S") .. ".txt"
            file = assert(io.open(path, "w"))
            watched, started, next_sample = {}, os.clock(), 0
            subject = pawn:GetAddress()
            write("START " .. os.date("%Y-%m-%d %H:%M:%S") .. " read-only state probe")
            local context = pawn or controller
            inspect("plane", pawn)
            inspect("controller", controller)
            if controller and controller:IsValid() then
                inspect("hud", controller.MyHUD)
                inspect("camera", controller.PlayerCameraManager)
            end
            if statics and statics:IsValid() and context then
                inspect("gamemode", statics:GetGameMode(context))
                inspect("gamestate", statics:GetGameState(context))
            end
            write("WATCHING " .. #watched .. " fields")
            file:flush()
            print("[AC8GMeter] Probe started (" .. #watched .. " fields, up to " .. LIMIT_S .. " s; Ctrl+F7 stops).\n")
        end
        -- Watched objects can be destroyed by a level change: never touch them after one.
        if (pawn and pawn:GetAddress() or 0) ~= subject then
            stop("aircraft changed")
            return
        end
        local now = os.clock() - started
        if now >= LIMIT_S then
            stop("time limit")
            return
        end
        if now < next_sample then return end
        next_sample = now + PERIOD_S
        local changed = false
        for _, field in ipairs(watched) do
            if not field.failed then
                local read, result = pcall(value, field)
                if not read then
                    field.failed = true
                    result = "<read failed: " .. tostring(result) .. ">"
                end
                if result ~= field.last then
                    write(string.format("%.2f %s = %s", now, field.label, result))
                    field.last = result
                    changed = true
                end
            end
        end
        if changed then file:flush() end
    end)
    if not ok then stop("error: " .. tostring(err)) end
end

return M
