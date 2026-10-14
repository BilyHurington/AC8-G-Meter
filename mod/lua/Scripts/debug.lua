-- Debug block of the frame (mod/flight/lua_frame.h, LuaDebugField), sent while debug recording
-- is on (Ctrl+F8 > Diagnostics). Every value is read defensively: NaN where the game does not
-- provide it. A function or property found missing is not tried again for 2 s (it may only be
-- missing while a plane spawns), so a missing one costs almost nothing per frame.
local M = {}

M.COUNT = 16  -- LuaDebugField::Count
local NAN = 0 / 0
local RETRY_S = 2.0
local missing, retry_at, plane = {}, 0, nil
local world_to_meters  -- constant per world: read once per plane

local function number(v)
    if type(v) == "number" then return v end
    if type(v) == "boolean" then return v and 1 or 0 end
    if type(v) == "userdata" or type(v) == "table" then  -- UE4SS wrappers expose :get()
        local ok, inner = pcall(function() return v:get() end)
        if ok then return number(inner) end
    end
    return NAN
end

-- fn() as a number, or NaN (and not tried again until the next retry) if it fails.
local function try(key, fn)
    if missing[key] then return NAN end
    local ok, v = pcall(fn)
    if ok then v = number(v) end
    if not ok or v ~= v then
        missing[key] = true
        return NAN
    end
    return v
end

-- Writes the block into out[first .. first + COUNT - 1].
-- component(struct, "X") reads a struct field whatever its spelling (see main.lua).
function M.fill(out, first, pawn, controller, statics, context, component)
    local address = pawn and pawn:GetAddress() or 0
    if address ~= plane then
        plane, world_to_meters = address, nil
        missing, retry_at = {}, os.clock() + RETRY_S
    elseif os.clock() >= retry_at then
        missing, retry_at = {}, os.clock() + RETRY_S
    end
    local i = first
    local function put(v)
        out[i] = v
        i = i + 1
    end
    local has_world = statics and statics:IsValid() and context
    put(has_world and try("real_time", function() return statics:GetRealTimeSeconds(context) end) or NAN)
    put(has_world and try("dilation", function() return statics:GetGlobalTimeDilation(context) end) or NAN)
    if not pawn then
        for _ = 3, M.COUNT do put(NAN) end
        return
    end
    put(try("custom_dilation", function() return pawn.CustomTimeDilation end))
    local rotation
    pcall(function() rotation = pawn:K2_GetActorRotation() end)
    for _, name in ipairs({ "Pitch", "Yaw", "Roll" }) do
        put(rotation and try("rot_" .. name, function() return component(rotation, name) end) or NAN)
    end
    local move_velocity
    if not missing.movement then
        local ok = pcall(function()
            local movement = pawn:GetMovementComponent()
            if movement and movement:IsValid() then move_velocity = movement.Velocity end
        end)
        if not ok or not move_velocity then missing.movement = true end
    end
    for _, name in ipairs({ "X", "Y", "Z" }) do
        put(move_velocity and try("move_" .. name, function() return component(move_velocity, name) end) or NAN)
    end
    put(try("speed_kmph", function() return pawn:GetSpeedKmph() end))
    put(try("speed_mps", function() return pawn:GetSpeedMps() end))
    put(try("altitude", function() return pawn:GetAltitudeMeters() end))
    put(try("impact_camera", function() return pawn.ImpactCamera.bIsActive end))
    put(controller and try("input_ignored", function() return controller:IsMoveInputIgnored() end) or NAN)
    put(controller and try("view_target", function()
        local target = controller:GetViewTarget()
        return target and target:IsValid() and target:GetAddress() == address
    end) or NAN)
    if not world_to_meters then
        world_to_meters = try("world_to_meters", function() return pawn:GetWorld():K2_GetWorldSettings().WorldToMeters end)
        if world_to_meters ~= world_to_meters then
            world_to_meters = try("world_to_meters_find", function() return FindFirstOf("WorldSettings").WorldToMeters end)
        end
        if world_to_meters ~= world_to_meters then world_to_meters = nil end  -- try again later
    end
    put(world_to_meters or NAN)
end

return M
