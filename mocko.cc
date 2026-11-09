local ReplicatedStorage = game:GetService("ReplicatedStorage")
local RunService        = game:GetService("RunService")
local Players           = game:GetService("Players")
local CoreGui           = game:GetService("CoreGui")

local LocalPlayer = Players.LocalPlayer
local Remotes     = ReplicatedStorage:WaitForChild("Remotes")
local Cars        = require(ReplicatedStorage.Systems.Cars)
local CarClient   = Cars.Client

if _G.MockoCleanup then pcall(_G.MockoCleanup) end
task.wait(); task.wait()

_G.MockoGeneration = (_G.MockoGeneration or 0) + 1
local gen = _G.MockoGeneration
local function alive() return _G.MockoGeneration == gen end

local connections = {}
local function track(c) connections[#connections + 1] = c; return c end
local function now() return os.clock() end

local function destroyGuis()
    if _G.MockoLibrary then
        pcall(function() _G.MockoLibrary.Unload() end)
        _G.MockoLibrary = nil
    end
    local playerGui = LocalPlayer:FindFirstChildOfClass("PlayerGui")
    local roots = { playerGui, CoreGui }
    if gethui then
        local ok, hui = pcall(gethui)
        if ok and hui ~= CoreGui and hui ~= playerGui then roots[#roots + 1] = hui end
    end
    for _, root in ipairs(roots) do
        if root then
            for _, child in ipairs(root:GetChildren()) do
                if child:IsA("ScreenGui") or child:IsA("Folder") then
                    local n = child.Name:lower()
                    if n:sub(1, 5) == "mocko" or n:sub(1, 8) == "obsidian" then
                        pcall(function() child:Destroy() end)
                    end
                end
            end
        end
    end
end
destroyGuis()

local Config = {
    flySpeed       = 450,
    maxStepSize    = 12,
    arriveDistance = 40,
    taxiJobId      = "Taxi",
    trailerJobId   = "TrailerDelivery",
}

local function newJobState()
    return {
        enabled = false, autoClaimEnabled = true,
        destinationPoint = nil, destPos = nil,
        hasActiveJob = false, step = "idle", stepStartedAt = 0,
        completeTries = 0, _completing = false,
    }
end

local function resetJob(job)
    job.hasActiveJob     = false
    job.destinationPoint = nil
    job.destPos          = nil
    job.completeTries    = 0
end

local taxi    = newJobState()
local trailer = newJobState()

local function getDrivetrain()
    local dt = rawget(CarClient, "drivetrain")
    if type(dt) ~= "table" then return nil end
    local v = rawget(dt, "vehicle")
    if type(v) ~= "table" then return nil end
    if rawget(v, "currentDriver") ~= LocalPlayer and rawget(v, "owner") ~= LocalPlayer then return nil end
    return dt, v
end

local function getCarRecord()
    local _, v = getDrivetrain()
    if not v then return nil end
    local m = rawget(v, "model")
    if typeof(m) == "Instance" and m.Parent then return v end
    return nil
end

local function getMainPart(record): BasePart?
    local m = rawget(record, "main")
    if typeof(m) == "Instance" and m:IsA("BasePart") then return m end
    return nil
end

local function getCarModel(record): Model?
    local m = rawget(record, "model")
    if typeof(m) == "Instance" and m:IsA("Model") then return m end
    return nil
end

local function getCarPos(record): Vector3?
    local main = getMainPart(record)
    return main and main.Position or nil
end

local function getNodePos(node): Vector3?
    if typeof(node) ~= "Instance" then return nil end
    local cf = node:GetAttribute("LocationCFrame")
    if typeof(cf) == "CFrame" then return cf.Position end
    return nil
end

local cachedMain: BasePart? = nil
local cachedWheels: { BasePart } = {}
local cachedPhysModel: Model? = nil
local cachedTrailerModel: Model? = nil
local cachedTrailerParts: { BasePart } = {}

local function resetTrailerCache()
    cachedTrailerModel = nil
    cachedTrailerParts = {}
end

local function clearPhysCache()
    cachedMain      = nil
    cachedWheels    = {}
    cachedPhysModel = nil
    resetTrailerCache()
end

local function rebuildPhysCache(model: Model)
    local main = model:FindFirstChild("Main")
    cachedMain = (main and main:IsA("BasePart")) and main or nil

    local wheels: { BasePart } = {}
    local wheelsFolder = model:FindFirstChild("Wheels")
    if wheelsFolder then
        for _, w in ipairs(wheelsFolder:GetChildren()) do
            if w:IsA("BasePart") then wheels[#wheels + 1] = w end
        end
    end
    cachedWheels    = wheels
    cachedPhysModel = model
    resetTrailerCache()
end

local function findHitchedTrailer(): Model?
    if not cachedPhysModel or not cachedMain then return nil end
    local tr = cachedPhysModel:FindFirstChild("Trailer")
    if not (tr and tr:IsA("Model")) then return nil end
    local hitch = cachedMain:FindFirstChild("Trailer")
    if not (hitch and hitch:IsA("Attachment")) then return nil end
    for _, d in ipairs(tr:GetDescendants()) do
        if d:IsA("BallSocketConstraint") and d.Enabled
            and (d.Attachment0 == hitch or d.Attachment1 == hitch) then
            return tr
        end
    end
    return nil
end

local function updateTrailerCache()
    local tr = findHitchedTrailer()
    if tr then
        if cachedTrailerModel ~= tr then
            cachedTrailerModel = tr
            cachedTrailerParts = {}
            for _, d in ipairs(tr:GetDescendants()) do
                if d:IsA("BasePart") then cachedTrailerParts[#cachedTrailerParts + 1] = d end
            end
            print(("[TRAILER] HITCHED (%d parts)"):format(#cachedTrailerParts))
        end
    elseif cachedTrailerModel then
        resetTrailerCache()
        print("[TRAILER] DETACHED")
    end
end

local function getTrailerPos(): Vector3?
    if cachedTrailerModel and cachedTrailerModel.Parent then
        return cachedTrailerModel:GetPivot().Position
    end
    return nil
end

local function getTrailerDestinationPos(): Vector3?
    if not cachedTrailerModel or not cachedTrailerModel.Parent then return nil end
    local tm = cachedTrailerModel:FindFirstChild("TrailerMain")
    local destVal = tm and tm:FindFirstChild("DestinationPoint")
    if not destVal or not destVal:IsA("ObjectValue") then return nil end
    return getNodePos(destVal.Value)
end

local function collectFreeTrailerPositions(): { Vector3 }
    local list = {}
    local jt = workspace:FindFirstChild("JobTrailers")
    if not jt then return list end
    for _, m in ipairs(jt:GetChildren()) do
        if m:IsA("Model") then
            local tm = m:FindFirstChild("TrailerMain")
            local dp = tm and tm:FindFirstChild("DestinationPoint")
            if dp and dp:IsA("ObjectValue") and dp.Value ~= nil then
                local ok, pivot = pcall(function() return m:GetPivot() end)
                if ok then list[#list + 1] = pivot.Position end
            end
        end
    end
    return list
end

local function zeroPart(p: BasePart)
    p.AssemblyLinearVelocity  = Vector3.zero
    p.AssemblyAngularVelocity = Vector3.zero
end

local function zeroVelocities()
    if cachedMain then zeroPart(cachedMain) end
    for _, w in ipairs(cachedWheels) do
        if w.Parent then zeroPart(w) end
    end
    for _, p in ipairs(cachedTrailerParts) do
        if p.Parent then zeroPart(p) end
    end
end

local function moveCarToModel(targetCF: CFrame)
    if not cachedPhysModel or not cachedMain then return end
    zeroVelocities()
    cachedPhysModel:PivotTo(targetCF)
    zeroVelocities()
end

local function syncPassenger(record)
    local model = getCarModel(record)
    local main  = getMainPart(record)
    if not model or not main then return end
    local pax = model:FindFirstChild("TaxiPassenger")
    if not pax or not pax:IsA("Model") then return end

    local part = pax.PrimaryPart
    if not part then
        part = pax:FindFirstChildWhichIsA("BasePart", true)
        if part then pax.PrimaryPart = part end
    end
    if part and (part.Position - main.Position).Magnitude > 12 then
        pcall(function() pax:PivotTo(main.CFrame * CFrame.new(0, 1, 0)) end)
        for _, ch in ipairs(pax:GetDescendants()) do
            if ch:IsA("BasePart") then zeroPart(ch) end
        end
    end
end

local raycastParams = RaycastParams.new()
raycastParams.FilterType  = Enum.RaycastFilterType.Exclude
raycastParams.IgnoreWater = true

local EXCLUDE_REFRESH = 2.0
local lastExcludeRefresh = 0

local function refreshRaycast(record)
    if now() - lastExcludeRefresh < EXCLUDE_REFRESH then return end
    lastExcludeRefresh = now()
    local ex: { Instance } = {}
    local model = getCarModel(record)
    if model then ex[#ex + 1] = model end
    if LocalPlayer.Character then ex[#ex + 1] = LocalPlayer.Character end
    local cars = workspace:FindFirstChild("Cars")
    if cars then
        for _, c in ipairs(cars:GetChildren()) do
            if c:IsA("Model") then ex[#ex + 1] = c end
        end
    end
    local traffic = workspace:FindFirstChild("_LocalTraffic")
    if traffic then ex[#ex + 1] = traffic end
    raycastParams.FilterDescendantsInstances = ex
end

local ray = {
    fwdHit = nil, fwdTime = 0, fwdDir = nil, fwdPos = nil,
    upTop = nil, upTime = 0, upPos = nil,
}

local function clearRayCache()
    ray.fwdHit = nil; ray.fwdTime = 0; ray.fwdDir = nil; ray.fwdPos = nil
    ray.upTop = nil; ray.upTime = 0; ray.upPos = nil
end

local function castForward(pos: Vector3, dir: Vector3, length: number)
    local t = now()
    local dirDot   = ray.fwdDir and ray.fwdDir:Dot(dir) or -1
    local moved    = ray.fwdPos and (ray.fwdPos - pos).Magnitude or 999
    local interval = ray.fwdHit and 0.05 or 0.2
    if (t - ray.fwdTime > interval) or (dirDot < 0.92) or (ray.fwdHit and moved > 15) then
        ray.fwdHit  = workspace:Raycast(pos, dir * length, raycastParams)
        ray.fwdTime = t
        ray.fwdDir  = dir
        ray.fwdPos  = pos
    end
    return ray.fwdHit
end

local function castUp(hitPos: Vector3): number
    local t = now()
    local moved = ray.upPos and (ray.upPos - hitPos).Magnitude or 999
    if (t - ray.upTime > 0.2) or (moved > 6) then
        local up = workspace:Raycast(hitPos + Vector3.new(0, 2, 0), Vector3.new(0, 400, 0), raycastParams)
        ray.upTop  = if up then up.Position.Y else (hitPos.Y + 50)
        ray.upTime = t
        ray.upPos  = hitPos
    end
    return ray.upTop
end

local function teleportCarTo(record, targetPos: Vector3)
    local main = getMainPart(record)
    if not getCarModel(record) or not main then return end
    moveCarToModel(CFrame.new(targetPos) * main.CFrame.Rotation)
    syncPassenger(record)
    clearRayCache()
end

local function microStep(record, target: Vector3, dt: number)
    local main = getMainPart(record)
    if not getCarModel(record) or not main then return end

    local myPos = main.Position
    local toT = target - myPos
    local dist = toT.Magnitude
    if dist < 0.5 then return end

    local stepSize = math.min(Config.flySpeed * dt, Config.maxStepSize, dist)
    local hDir = Vector3.new(toT.X, 0, toT.Z)
    hDir = if hDir.Magnitude > 0.5 then hDir.Unit else Vector3.zero

    local move = toT.Unit * stepSize
    if hDir.Magnitude > 0 and myPos.Y <= 550 then
        local hit = castForward(myPos, hDir, stepSize * 3 + 15)
        if hit then
            local wallTop = castUp(hit.Position)
            move = hDir * (stepSize * 0.3) + Vector3.new(0, math.min(stepSize, wallTop - myPos.Y + 20), 0)
        end
    end

    moveCarToModel(CFrame.new(myPos + move) * main.CFrame.Rotation)
    syncPassenger(record)
end

local TRAILER_RANGE = 150

local function findNearestStart(pos: Vector3, jobId: string, requireTrailer: boolean)
    local jobs = workspace:FindFirstChild("Jobs")
    local jf = jobs and jobs:FindFirstChild(jobId)
    local folder = jf and jf:FindFirstChild("StartPoints")
    if not folder then return nil, nil end

    local freePositions
    if requireTrailer then
        freePositions = collectFreeTrailerPositions()
        if #freePositions == 0 then return nil, nil end
    end

    local nearest, nearestD
    for _, node in ipairs(folder:GetDescendants()) do
        local p = getNodePos(node)
        if p then
            local ok = not requireTrailer
            if requireTrailer then
                for _, fp in ipairs(freePositions) do
                    if (fp - p).Magnitude < TRAILER_RANGE then ok = true; break end
                end
            end
            if ok then
                local d = (p - pos).Magnitude
                if not nearestD or d < nearestD then nearestD, nearest = d, node end
            end
        end
    end
    return nearest, nearestD
end

local savedGarage = nil

local function blockGarage()
    if savedGarage then return end
    local ok, module = pcall(require, ReplicatedStorage.Systems.Gui.Views.Customization)
    if not ok or type(module) ~= "table" then return end
    savedGarage = { module = module, open = module.Open, start = module.Start }
    module.Open  = function() return nil end
    module.Start = function() return nil end
end

local function unblockGarage()
    if not savedGarage then return end
    savedGarage.module.Open  = savedGarage.open
    savedGarage.module.Start = savedGarage.start
    savedGarage = nil
end

local autoClaimConn: RBXScriptConnection? = nil

local function startAutoClaim()
    if autoClaimConn then return end
    local pg = LocalPlayer:FindFirstChildOfClass("PlayerGui")
    local jc = pg and pg:FindFirstChild("JobComplete")
    if not jc then return end

    local function firstHandler(signal)
        if not getconnections then return nil end
        local ok, c = pcall(getconnections, signal)
        return ok and c and c[1] and c[1].Function or nil
    end

    local function getHandlers()
        local startBtn = jc:FindFirstChild("StartButton", true)
        local closeBtn = jc:FindFirstChild("CloseButton", true)
        local startH, closeH
        if startBtn and startBtn:IsA("TextButton") then
            startH = firstHandler(startBtn.Activated)
        end
        if closeBtn and closeBtn:IsA("TextButton") then
            for _, s in ipairs({ "Activated", "MouseButton1Click", "MouseButton1Down" }) do
                closeH = firstHandler(closeBtn[s])
                if closeH then break end
            end
        end
        return startBtn, startH, closeH, closeBtn
    end

    local function getText(btn)
        if not btn or not btn:IsA("TextButton") then return "" end
        if btn.Text ~= "" then return btn.Text end
        local lbl = btn:FindFirstChild("Label", true)
        if lbl and lbl:IsA("TextLabel") then return lbl.Text end
        return ""
    end

    local function close(handler, btn)
        if handler then pcall(handler)
        elseif btn then pcall(function() btn:Activate() end)
        else pcall(function() jc.Enabled = false end) end
    end

    local busy = false
    local function handle()
        if busy then return end
        busy = true
        for _ = 1, 30 do
            task.wait(0.05)
            local startBtn, startH, closeH, closeBtn = getHandlers()
            local text = getText(startBtn)
            if text == "Claim" then
                if startH then pcall(startH) else pcall(function() startBtn:Activate() end) end
                task.wait(0.7)
                local _, _, closeH2, closeB2 = getHandlers()
                close(closeH2, closeB2 or closeBtn)
                break
            elseif text == "Replay" or text == "Exit" or text == "Close" then
                close(closeH, closeBtn)
                break
            end
        end
        task.wait(0.3)
        busy = false
    end

    autoClaimConn = jc:GetPropertyChangedSignal("Enabled"):Connect(function()
        if not (taxi.autoClaimEnabled or trailer.autoClaimEnabled) then return end
        if jc.Enabled then task.spawn(handle) end
    end)
    if jc.Enabled then task.spawn(handle) end
end

local function stopAutoClaim()
    if autoClaimConn then autoClaimConn:Disconnect(); autoClaimConn = nil end
end

track(Remotes.JobStarted.OnClientEvent:Connect(function(jobId, data)
    if jobId == Config.taxiJobId then
        taxi.hasActiveJob = true
        taxi.completeTries = 0
        if type(data) == "table" and type(data.JobInfo) == "table" then
            taxi.destinationPoint = data.JobInfo.DestinationPoint
            taxi.destPos = nil
        end
    elseif jobId == Config.trailerJobId then
        trailer.hasActiveJob = true
        trailer.completeTries = 0
        trailer.destinationPoint = nil
        trailer.destPos = nil
    end
end))

track(Remotes.JobCompleted.OnClientEvent:Connect(function(jobId)
    if not jobId or jobId == Config.taxiJobId then resetJob(taxi) end
    if not jobId or jobId == Config.trailerJobId then resetJob(trailer) end
end))

local tuning = {
    enabled = false, torque = 1.0, maxRpm = 8500, drag = 100, gearLen = 100,
}

local stock = {}
local function resetStock()
    stock.valid = false
    stock.dt = nil; stock.engine = nil; stock.config = nil; stock.model = nil
    stock.maxRpm = nil; stock.upgradeRpm = nil; stock.torqueCurve = {}
    stock.dragCoeff = nil; stock.finalDrive = nil; stock.gears = {}
    stock.mineGears = nil; stock.mineGearsSnap = {}
    stock.attrHolder = nil; stock.attrsSnap = {}
    stock.cachedGears = nil; stock.topSpeedsSnap = {}; stock.minSpeedsSnap = {}
    stock.calcTopSpeed = nil
end
resetStock()

local currentModel: Model? = nil
local pendingBind: thread? = nil

local function safeSet(t, k, v)
    if type(t) ~= "table" or type(v) ~= "number" then return end
    if v ~= v or v == math.huge or v == -math.huge then return end
    rawset(t, k, v)
end

local function snapshot(t)
    local c = {}
    if type(t) == "table" then for k, v in pairs(t) do c[k] = v end end
    return c
end

local function restoreInto(t, snap)
    if type(t) ~= "table" then return end
    for k, v in pairs(snap) do safeSet(t, k, v) end
end

local function snapEngine(engine)
    stock.engine      = engine
    stock.maxRpm      = rawget(engine, "MaxRPM")
    stock.upgradeRpm  = rawget(engine, "upgradeRPM")
    stock.torqueCurve = snapshot(rawget(engine, "TorqueCurve"))
end

local function snapGearboxAttrs(record, model)
    local folder = rawget(record, "tuning")
    if typeof(folder) ~= "Instance" then folder = model:FindFirstChild("Tuning") end
    if typeof(folder) ~= "Instance" or not folder:IsA("Folder") then return end
    local gb = folder:FindFirstChild("Gearbox")
    if not gb or not gb:IsA("Folder") then return end

    for _, sub in ipairs(gb:GetChildren()) do
        local attrs = sub:GetAttributes()
        local relevant = false
        for k in pairs(attrs) do
            if k == "FinalDrive" or tostring(k):match("^Gear%d+$") then relevant = true; break end
        end
        if relevant then
            stock.attrHolder = sub
            stock.attrsSnap = {}
            for k, v in pairs(attrs) do
                if type(v) == "number" then stock.attrsSnap[k] = v end
            end
            return
        end
    end
end

local function applyEngine()
    if not stock.valid or not stock.engine then return end
    safeSet(stock.engine, "MaxRPM", tuning.maxRpm)
    if rawget(stock.engine, "upgradeRPM") ~= nil and stock.maxRpm and stock.maxRpm > 0 then
        safeSet(stock.engine, "upgradeRPM", tuning.maxRpm / stock.maxRpm)
    end
    local tc = rawget(stock.engine, "TorqueCurve")
    if type(tc) == "table" then
        for k, orig in pairs(stock.torqueCurve) do
            if type(orig) == "number" then safeSet(tc, k, orig * tuning.torque) end
        end
    end
end

local function scaleGear(key, ratio, scale)
    local n = tonumber(key)
    if n and n > 0 then return ratio / scale end
    if n == 0 then return 0 end
    if n == -1 then return ratio * scale end
    return ratio
end

local function applyChassis()
    if not stock.valid or not stock.config then return end
    local scale = tuning.gearLen / 100

    safeSet(stock.config, "DragCoefficient", stock.dragCoeff * (tuning.drag / 100))
    safeSet(stock.config, "FinalDrive", stock.finalDrive / scale)

    local cfgGears = rawget(stock.config, "Gears")
    if type(cfgGears) == "table" then
        for k, orig in pairs(stock.gears) do
            if type(orig) == "number" then safeSet(cfgGears, k, scaleGear(k, orig, scale)) end
        end
    end

    if stock.mineGears then
        for k, orig in pairs(stock.mineGearsSnap) do
            if type(orig) == "number" then safeSet(stock.mineGears, k, orig / scale) end
        end
    end

    if stock.attrHolder then
        for k, orig in pairs(stock.attrsSnap) do
            pcall(function() stock.attrHolder:SetAttribute(k, orig / scale) end)
        end
    end

    if stock.cachedGears then
        local top = rawget(stock.cachedGears, "topSpeeds")
        if type(top) == "table" then
            for k, orig in pairs(stock.topSpeedsSnap) do
                if type(orig) == "number" then safeSet(top, k, orig * scale) end
            end
        end
    end
    if type(stock.calcTopSpeed) == "number" then
        local rec = getCarRecord()
        if rec then rawset(rec, "calculatedTopSpeed", stock.calcTopSpeed * scale) end
    end
end

local function restoreEngine()
    if not stock.valid or not stock.engine then return end
    safeSet(stock.engine, "MaxRPM", stock.maxRpm)
    if stock.upgradeRpm then safeSet(stock.engine, "upgradeRPM", stock.upgradeRpm) end
    restoreInto(rawget(stock.engine, "TorqueCurve"), stock.torqueCurve)
end

local function restoreChassis()
    if not stock.valid then return end
    if stock.config then
        safeSet(stock.config, "DragCoefficient", stock.dragCoeff)
        safeSet(stock.config, "FinalDrive", stock.finalDrive)
        restoreInto(rawget(stock.config, "Gears"), stock.gears)
    end
    if stock.mineGears then restoreInto(stock.mineGears, stock.mineGearsSnap) end
    if stock.attrHolder then
        for k, v in pairs(stock.attrsSnap) do
            pcall(function() stock.attrHolder:SetAttribute(k, v) end)
        end
    end
    if stock.cachedGears then
        restoreInto(rawget(stock.cachedGears, "topSpeeds"), stock.topSpeedsSnap)
        restoreInto(rawget(stock.cachedGears, "minSpeeds"), stock.minSpeedsSnap)
    end
    if type(stock.calcTopSpeed) == "number" then
        local rec = getCarRecord()
        if rec then rawset(rec, "calculatedTopSpeed", stock.calcTopSpeed) end
    end
end

local function restoreTuning()
    restoreEngine()
    restoreChassis()
end

local function reapply()
    applyChassis()
    if tuning.enabled and stock.engine then applyEngine() end
end

local function bindCar()
    local dt, record = getDrivetrain()
    if not record then return false end
    local model = getCarModel(record)
    if not model then return false end

    if currentModel == model and stock.valid then
        if stock.dt == dt then return true end
        stock.dt = dt
        local mg = rawget(record, "gears")
        if type(mg) == "table" then stock.mineGears = mg end
        local engine = rawget(dt, "engine")
        if type(engine) == "table" then snapEngine(engine) end
        reapply()
        return true
    end

    clearPhysCache()
    resetStock()

    local cfg = rawget(record, "config")
    if type(cfg) ~= "table" then return false end
    stock.config     = cfg
    stock.model      = model
    stock.dragCoeff  = rawget(cfg, "DragCoefficient")
    stock.finalDrive = rawget(cfg, "FinalDrive")
    stock.gears      = snapshot(rawget(cfg, "Gears"))

    local mineGears = rawget(record, "gears")
    if type(mineGears) == "table" then
        stock.mineGears = mineGears
        stock.mineGearsSnap = snapshot(mineGears)
    end

    snapGearboxAttrs(record, model)

    local cg = rawget(record, "cachedGears")
    if type(cg) == "table" then
        stock.cachedGears   = cg
        stock.topSpeedsSnap = snapshot(rawget(cg, "topSpeeds"))
        stock.minSpeedsSnap = snapshot(rawget(cg, "minSpeeds"))
    end
    stock.calcTopSpeed = rawget(record, "calculatedTopSpeed")

    local engine = rawget(dt, "engine")
    if type(engine) == "table" then
        snapEngine(engine)
        if tuning.maxRpm == 8500 and type(stock.maxRpm) == "number" then
            tuning.maxRpm = stock.maxRpm
        end
    end

    stock.dt    = dt
    stock.valid = true
    currentModel = model
    lastExcludeRefresh = 0
    clearRayCache()
    rebuildPhysCache(model)
    reapply()
    return true
end

local function activeJob()
    if trailer.enabled then return trailer, Config.trailerJobId, true end
    if taxi.enabled then return taxi, Config.taxiJobId, false end
    return nil, nil, false
end

local function resolveDestination(job, isTrailer): Vector3?
    if isTrailer then return getTrailerDestinationPos() end
    if not job.destPos and job.destinationPoint then
        job.destPos = getNodePos(job.destinationPoint)
    end
    return job.destPos
end

track(RunService.Stepped:Connect(function(_, dt)
    if not alive() then return end
    local job, _, isTrailer = activeJob()
    if not job or job.step ~= "toDestination" then return end

    local record = getCarRecord()
    if not record then return end
    refreshRaycast(record)
    if not getCarPos(record) then return end
    if isTrailer and not cachedTrailerModel then return end

    local dest = resolveDestination(job, isTrailer)
    if dest then microStep(record, dest + Vector3.new(0, 5, 0), dt) end
end))

local function syncJobWithServer(job, jobId)
    local pd = workspace:FindFirstChild("PlayerData")
    local me = pd and pd:FindFirstChild(LocalPlayer.Name)
    local jobs = me and me:FindFirstChild("Jobs")
    local cj = jobs and jobs:FindFirstChild("CurrentJob")
    local active = cj and cj:IsA("StringValue") and cj.Value == jobId
    if active and not job.hasActiveJob then
        job.hasActiveJob = true
    elseif not active and job.hasActiveJob then
        resetJob(job)
    end
end

local function tryComplete(job)
    job._completing   = true
    job.completeTries = job.completeTries + 1
    local tries = job.completeTries
    task.spawn(function()
        pcall(function() Remotes.CompleteJob:FireServer() end)
        local waited = 0
        while job.hasActiveJob and waited < 2.5 do
            task.wait(0.1); waited = waited + 0.1
        end
        job._completing = false
        if not alive() then return end
        if not job.hasActiveJob then
            pcall(function() Remotes.ClaimJobIncome:FireServer() end)
            job.step = "idle"; job.stepStartedAt = now() + 1
            job.completeTries = 0
        elseif tries >= 4 then
            job.step = "idle"; job.stepStartedAt = now() + 3
            resetJob(job)
        else
            job.step = "toDestination"; job.stepStartedAt = now() + 0.5
        end
    end)
end

local function watchCar(record)
    if record then
        local model = getCarModel(record)
        local dtNow = rawget(CarClient, "drivetrain")
        local needBind = model and (model ~= currentModel or (stock.valid and stock.dt ~= dtNow))
        if needBind and not pendingBind then
            pendingBind = task.delay(0.5, function()
                pendingBind = nil
                if alive() then bindCar() end
            end)
        end
        if model and model == currentModel then updateTrailerCache() end
    elseif currentModel and not pendingBind then
        currentModel = nil
        stock.valid = false
        clearRayCache()
        clearPhysCache()
    end
end

track(RunService.Heartbeat:Connect(function()
    if not alive() then return end

    local record = getCarRecord()
    watchCar(record)

    local job, jobId, isTrailer = activeJob()
    if not job or not record then return end
    local pos = getCarPos(record)
    if not pos then return end
    local elapsed = now() - job.stepStartedAt

    syncJobWithServer(job, jobId)

    if job.step == "idle" then
        job.step = job.hasActiveJob and "toDestination" or "toStartPoint"
        job.stepStartedAt = now()

    elseif job.step == "toStartPoint" then
        local node = findNearestStart(pos, jobId, isTrailer)
        if not node then
            if isTrailer then job.stepStartedAt = now() end
            return
        end
        local targetPos = getNodePos(node)
        if targetPos then teleportCarTo(record, targetPos + Vector3.new(0, 3, 0)) end
        task.wait(0.1)
        pcall(function() Remotes.StartJob:FireServer(jobId, node) end)
        job.step = "waitingStart"; job.stepStartedAt = now()

    elseif job.step == "waitingStart" then
        if job.hasActiveJob then
            job.step = "toDestination"; job.stepStartedAt = now()
        elseif elapsed > 5 then
            job.step = "toStartPoint"; job.stepStartedAt = now()
        end

    elseif job.step == "toDestination" then
        if isTrailer and not cachedTrailerModel then return end
        local dest = resolveDestination(job, isTrailer)
        if not dest then return end

        local arrive = (pos - dest).Magnitude < Config.arriveDistance
        if arrive and isTrailer then
            local trPos = getTrailerPos()
            arrive = trPos ~= nil and (trPos - dest).Magnitude < Config.arriveDistance
        end

        if arrive and not job._completing then tryComplete(job) end
    end
end))

local repo = "https://raw.githubusercontent.com/deividcomsono/Obsidian/main/"
local Library = loadstring(game:HttpGet(repo .. "Library.lua"))()
_G.MockoLibrary = Library

local Window = Library:CreateWindow({
    Title = "Mocko.cc",
    Footer = "Drive World",
    Icon = "rbxassetid://10734895501",
    NotifySide = "Right",
    Size = UDim2.fromOffset(660, 500),
    Resizable = true,
    ShowCustomCursor = false,
})

local Tabs = {
    Taxi     = Window:AddTab("Taxi", "car"),
    Trailers = Window:AddTab("Trailers", "truck"),
    Car      = Window:AddTab("Car", "gauge"),
}

local function buildJobTab(tab, job, other, label, prefix)
    local panel = tab:AddLeftGroupbox("Master")

    panel:AddToggle(prefix .. "On", {
        Text = "Enable Auto " .. label,
        Default = false,
        Callback = function(v)
            if not alive() then return end
            job.enabled = v
            if v then
                other.enabled = false
                job.step = "idle"; job.stepStartedAt = now()
                job._completing = false
                blockGarage()
                if taxi.autoClaimEnabled or trailer.autoClaimEnabled then startAutoClaim() end
            elseif not other.enabled then
                unblockGarage()
                stopAutoClaim()
            end
        end,
    })

    panel:AddToggle(prefix .. "Claim", {
        Text = "Auto Claim Reward",
        Default = true,
        Callback = function(v)
            if not alive() then return end
            job.autoClaimEnabled = v
            if v and job.enabled then startAutoClaim()
            elseif not v and not other.enabled then stopAutoClaim() end
        end,
    })

    panel:AddButton({
        Text = "Stop & Unblock All",
        Func = function()
            if not alive() then return end
            job.enabled = false
            job._completing = false
            if not other.enabled then
                unblockGarage()
                stopAutoClaim()
            end
        end,
    })
end

buildJobTab(Tabs.Taxi, taxi, trailer, "Taxi", "Taxi")
buildJobTab(Tabs.Trailers, trailer, taxi, "Trailer", "Trailer")

local carMaster = Tabs.Car:AddLeftGroupbox("Master")
carMaster:AddToggle("TuneOn", {
    Text = "Enable engine tuning",
    Default = false,
    Callback = function(v)
        if not alive() then return end
        tuning.enabled = v
        if v then bindCar(); applyEngine() else restoreEngine() end
    end,
})
carMaster:AddButton({
    Text = "Reset to stock",
    Func = function()
        if not alive() then return end
        tuning.torque  = 1.0
        tuning.drag    = 100
        tuning.gearLen = 100
        if stock.valid and type(stock.maxRpm) == "number" then tuning.maxRpm = stock.maxRpm end
        reapply()
    end,
})

local function onEngineChange()
    if tuning.enabled and stock.engine then applyEngine() end
end

local enginePanel = Tabs.Car:AddLeftGroupbox("Engine")
enginePanel:AddSlider("Torque", {
    Text = "Torque %", Default = 100, Min = 100, Max = 100000, Rounding = 0, Suffix = "%",
    Callback = function(v)
        if not alive() then return end
        tuning.torque = v / 100
        onEngineChange()
    end,
})
enginePanel:AddSlider("Rpm", {
    Text = "Max RPM", Default = 8500, Min = 3000, Max = 50000, Rounding = 0, Suffix = " rpm",
    Callback = function(v)
        if not alive() then return end
        tuning.maxRpm = v
        onEngineChange()
    end,
})

local chassisPanel = Tabs.Car:AddRightGroupbox("Chassis")
chassisPanel:AddSlider("Drag", {
    Text = "Drag %", Default = 100, Min = 10, Max = 150, Rounding = 0, Suffix = "%",
    Callback = function(v)
        if not alive() then return end
        tuning.drag = v
        applyChassis()
    end,
})
chassisPanel:AddSlider("GearLen", {
    Text = "Gear length %", Default = 100, Min = 100, Max = 500, Rounding = 0, Suffix = "%",
    Callback = function(v)
        if not alive() then return end
        tuning.gearLen = v
        applyChassis()
    end,
})

Library.ToggleKeybind = Enum.KeyCode.RightShift

_G.MockoCleanup = function()
    _G.MockoGeneration = (_G.MockoGeneration or 0) + 1
    taxi.enabled    = false; taxi._completing    = false
    trailer.enabled = false; trailer._completing = false
    tuning.enabled  = false
    stopAutoClaim()
    unblockGarage()
    restoreTuning()
    for _, c in ipairs(connections) do
        pcall(function() c:Disconnect() end)
    end
    connections = {}
    destroyGuis()
end
