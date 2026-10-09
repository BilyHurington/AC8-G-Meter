-- AC8 G-Meter: samples the player's aircraft once per game tick on the game thread and hands
-- the numbers to ac8gmeter.dll, which computes G and draws the overlay.
-- Frame layout: mod/flight/lua_frame.h (LuaField). Keep both in step.
local FRAME_VERSION = 1
local START_PROTOCOL = 1

local directory = assert(debug.getinfo(1, "S").source:sub(2):match("^(.*[/\\])"))
local config = dofile(directory .. "config.lua")
local probe = dofile(directory .. "probe.lua")
local dll = directory .. "ac8gmeter.dll"
local start_native = assert(package.loadlib(dll, "ac8gmeter_start"))
local frame_native = assert(package.loadlib(dll, "ac8gmeter_frame"))

local last_notice
local function notice(message)
    if message ~= last_notice then
        print("[AC8GMeter] " .. message .. "\n")
        last_notice = message
    end
end

-- Reflected struct fields can come back with an unexpected spelling (AC8 exposes "pitch"
-- for FRotator::Pitch), so look the component up case-insensitively once and cache it.
local field_names = {}
local function component(struct, wanted)
    local key = field_names[wanted]
    if key then  -- fast path once the spelling is known (tick() runs under pcall anyway)
        local v = struct[key]
        if type(v) == "number" then return v end
    end
    local ok, v = pcall(function() return struct[wanted] end)
    if ok and type(v) == "number" then
        field_names[wanted] = wanted
        return v
    end
    if type(struct) == "table" then
        local lower = wanted:lower()
        for k, candidate in pairs(struct) do
            if type(k) == "string" and k:lower() == lower and type(candidate) == "number" then
                field_names[wanted] = k
                return candidate
            end
        end
    end
    error("vector has no component " .. wanted)
end

local function xyz(v)
    return component(v, "X"), component(v, "Y"), component(v, "Z")
end

local plane_class, statics, cached_controller
local next_search = 0  -- os.clock() time of the next FindAllOf scan while no plane is found

local function gameplay_statics()
    if not statics or not statics:IsValid() then
        statics = StaticFindObject("/Script/Engine.Default__GameplayStatics")
    end
    return statics
end

-- The plane possessed by the (single) local player controller, and that controller.
local function player_plane()
    if not plane_class or not plane_class:IsValid() then
        plane_class = StaticFindObject("/Script/Live.LivePlayerPlane")
        if not plane_class or not plane_class:IsValid() then return nil, nil end
    end
    if cached_controller and cached_controller:IsValid() then
        local pawn = cached_controller.Pawn
        if pawn and pawn:IsValid() and pawn:IsA(plane_class) then return pawn, cached_controller end
    else
        cached_controller = nil
    end
    -- No plane on the cached controller: another controller may have it (level travel).
    if os.clock() < next_search then return nil, cached_controller end
    next_search = os.clock() + 1.0  -- FindAllOf is expensive: at most once a second
    local found, count, any = nil, 0, nil
    for _, controller in ipairs(FindAllOf("LivePlayerController") or {}) do
        if controller:IsValid() then
            any = any or controller
            local pawn = controller.Pawn
            if pawn and pawn:IsValid() and pawn:IsA(plane_class) then
                found, count = controller, count + 1
            end
        end
    end
    if count == 1 then
        cached_controller = found
        return found.Pawn, found
    end
    cached_controller = cached_controller or any  -- still a world context for time and pause
    return nil, cached_controller
end

local frame = {}
for i = 1, 21 do frame[i] = 0 end  -- LuaField::Count
frame[1] = FRAME_VERSION

local last_clock = os.clock()  -- os.clock() is wall time with the MSVC runtime

local function tick()
    local now = os.clock()
    local wall_delta = math.min(now - last_clock, 0.25)
    last_clock = now
    local pawn, controller = player_plane()
    local gs = gameplay_statics()
    local context = pawn or controller
    local delta, world_time, paused = wall_delta, 0, false  -- while no world object is known
    if gs and gs:IsValid() and context then
        delta = gs:GetWorldDeltaSeconds(context)
        world_time = gs:GetTimeSeconds(context)
        paused = gs:IsGamePaused(context)
    end
    frame[2], frame[3], frame[4] = world_time, delta, paused and 1 or 0
    if pawn then
        frame[5], frame[6] = 1, pawn:GetAddress()
        frame[7], frame[8], frame[9] = xyz(pawn:K2_GetActorLocation())
        frame[10], frame[11], frame[12] = xyz(pawn:GetVelocity())
        frame[13], frame[14], frame[15] = xyz(pawn:GetActorForwardVector())
        frame[16], frame[17], frame[18] = xyz(pawn:GetActorRightVector())
        frame[19], frame[20], frame[21] = xyz(pawn:GetActorUpVector())
    else
        for i = 5, 21 do frame[i] = 0 end
    end
    if frame_native(table.unpack(frame)) ~= 1 then error("frame rejected (see ac8gmeter.log)") end
    probe.update(pawn, controller, gs, directory)
end

-- Ctrl+F7: start / stop a read-only capture of game state fields (probe.lua).
RegisterKeyBind(Key.F7, { ModifierKey.CONTROL }, function() probe.request() end)

if start_native(START_PROTOCOL, config.RecordCsv and 1 or 0) ~= 1 then
    notice("Not started; see ac8gmeter.log in the mod folder.")
elseif EngineTickAvailable == false or type(LoopInGameThreadAfterFrames) ~= "function" then
    notice("Disabled: UE4SS game-thread tick unavailable (HookEngineTick=1 is required).")
else
    local retry_at = 0
    LoopInGameThreadAfterFrames(1, function()
        if os.clock() < retry_at then return end
        local ok, err = pcall(tick)
        if not ok then
            cached_controller = nil
            retry_at = os.clock() + 1.0
            notice("Recovering after error: " .. tostring(err))
        end
    end)
    notice("Loaded. Ctrl+F8 settings, Ctrl+F9 move the overlay.")
end
