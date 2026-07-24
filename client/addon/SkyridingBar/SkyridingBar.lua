-- Horizon Skyriding charge bar (flying mount).
-- Server: HORIZON_SKY  CHG\t...  MODE\t0|1  ANIM\t0|1  LAND\t1  STALLLOCK\t1  VERT\t0|1  TURN\t0.4
-- VERT: 0 = block Space/X ascend/descend (WXL), 1 = allow classic vertical
-- TURN: mouse-yaw inertia multiplier while airborne (1.0=normal, 0.75=−25%)
-- Cast Surge / Skyward from action bar while on a flying mount.
-- Vigor pool is separate from flight takeoff charges (98052); server default recharge 0.5s for testing.

local PREFIX = "HORIZON_SKY"

local charges = 6
local maxCharges = 6
local rechargeMs = 0
local active = false
local lastFlapMs = 0

if RegisterAddonMessagePrefix then
    RegisterAddonMessagePrefix(PREFIX)
end

local frame = CreateFrame("Frame", "HorizonSkyridingBar", UIParent)
frame:SetWidth(180)
frame:SetHeight(36)
frame:SetPoint("BOTTOM", UIParent, "BOTTOM", 0, 180)
frame:SetBackdrop({
    bgFile = "Interface\\Tooltips\\UI-Tooltip-Background",
    edgeFile = "Interface\\Tooltips\\UI-Tooltip-Border",
    tile = true, tileSize = 8, edgeSize = 12,
    insets = { left = 2, right = 2, top = 2, bottom = 2 }
})
frame:SetBackdropColor(0, 0, 0, 0.7)
frame:Hide()

local title = frame:CreateFontString(nil, "OVERLAY", "GameFontNormalSmall")
title:SetPoint("TOP", frame, "TOP", 0, -4)
title:SetText("Skyriding")

local pipRow = CreateFrame("Frame", nil, frame)
pipRow:SetPoint("TOP", title, "BOTTOM", 0, -2)
pipRow:SetWidth(168)
pipRow:SetHeight(12)

local pips = {}
for i = 1, 6 do
    local pip = pipRow:CreateTexture(nil, "ARTWORK")
    pip:SetWidth(22)
    pip:SetHeight(10)
    pip:SetPoint("LEFT", pipRow, "LEFT", (i - 1) * 28, 0)
    pip:SetTexture("Interface\\Buttons\\WHITE8X8")
    pip:SetVertexColor(0.2, 0.7, 1.0, 1)
    pips[i] = pip
end

local cdText = frame:CreateFontString(nil, "OVERLAY", "GameFontHighlightSmall")
cdText:SetPoint("BOTTOM", frame, "BOTTOM", 0, 4)
cdText:SetText("")

local hint = frame:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
hint:SetPoint("TOP", frame, "BOTTOM", 0, -2)
hint:SetText("Surge / Skyward on action bar")

-- Debug speed readout (~2–3 Hz from server SPD). Visible while bar active.
local spdText = UIParent:CreateFontString(nil, "OVERLAY", "GameFontNormalLarge")
spdText:SetPoint("BOTTOM", frame, "TOP", 0, 8)
spdText:SetTextColor(1, 0.95, 0.4, 1)
spdText:SetText("")
spdText:Hide()

local function Refresh()
    for i = 1, 6 do
        if i <= charges then
            pips[i]:SetVertexColor(0.2, 0.85, 1.0, 1)
        else
            pips[i]:SetVertexColor(0.15, 0.15, 0.15, 0.8)
        end
    end
    if charges < maxCharges and rechargeMs > 0 then
        cdText:SetText(string.format("+1 in %.1fs", rechargeMs / 1000))
    else
        cdText:SetText(string.format("%d / %d", charges, maxCharges))
    end
    if active then
        frame:Show()
        spdText:Show()
    else
        frame:Hide()
        spdText:Hide()
        spdText:SetText("")
    end
end

local function TriggerFlap(kind, source)
    local now = GetTime()
    if (now - lastFlapMs) < 0.6 then
        return
    end
    lastFlapMs = now
    if type(WXL_SkyridingFlap) == "function" then
        WXL_SkyridingFlap(kind)
    elseif type(WXL_PlayMountAnim) == "function" then
        WXL_PlayMountAnim(kind == 1 and 1702 or 1680)
    end
end

local function HandleSkyPayload(message)
    if not message or message == "" then
        return
    end
    if message:sub(1, #PREFIX + 1) == (PREFIX .. "\t") then
        message = message:sub(#PREFIX + 2)
    end
    local cmd, a, b, c, d = strsplit("\t", message)
    if cmd == "CHG" then
        charges = tonumber(a) or charges
        maxCharges = tonumber(b) or maxCharges
        rechargeMs = tonumber(c) or 0
        active = (tonumber(d) or 0) == 1
        Refresh()
    elseif cmd == "RATE" then
        local rate = tonumber(a) or 0
        if type(WXL_SkyridingSetFlightRate) == "function" then
            WXL_SkyridingSetFlightRate(rate)
        end
        if type(WXL_SkyridingSetStalled) == "function" then
            WXL_SkyridingSetStalled(rate < 0 and 1 or 0)
        end
    elseif cmd == "SPD" then
        -- SPD\trate\tpitch\tband\tyardsPerSec
        local rate = tonumber(a) or 0
        local pitch = tonumber(b) or 0
        local band = tostring(c or "?")
        local yps = tonumber(d) or 0
        spdText:SetText(string.format("spd %.2f | pitch %.2f | %s | %.0f y/s",
            rate, pitch, band, yps))
        if active then
            spdText:Show()
        end
        if type(WXL_SkyridingSetCoastSpeed) == "function" and yps > 0 then
            WXL_SkyridingSetCoastSpeed(yps)
        end
        if type(WXL_SkyridingSetStalled) == "function" then
            WXL_SkyridingSetStalled(band == "stall" and 1 or 0)
        end
    elseif cmd == "ANIM" then
        TriggerFlap(tonumber(a) or 0, "addon_ANIM")
    elseif cmd == "LAND" then
        if type(WXL_SkyridingSetGroundLock) == "function" then
            WXL_SkyridingSetGroundLock(2000)
        elseif type(WXL_SkyridingLand) == "function" then
            WXL_SkyridingLand()
        end
    elseif cmd == "GLOCK" then
        local ms = tonumber(a) or 2000
        if type(WXL_SkyridingSetGroundLock) == "function" then
            WXL_SkyridingSetGroundLock(ms)
        end
    elseif cmd == "SETTLE" then
        -- Legacy no-op.
    elseif cmd == "STALLLOCK" then
        -- Mid-air login/teleport: stall until land (no flaps / upright sink).
        if type(WXL_SkyridingStallLock) == "function" then
            WXL_SkyridingStallLock()
        end
    elseif cmd == "MODE" then
        if type(WXL_SkyridingSetMode) == "function" then
            WXL_SkyridingSetMode((tonumber(a) or 0) == 1 and 1 or 0)
        end
    elseif cmd == "VERT" then
        -- Server Skyriding.ClassicVertical: 0 = block Space/X, 1 = allow
        local allow = tonumber(a) or 0
        if type(WXL_SkyridingSetClassicVertical) == "function" then
            WXL_SkyridingSetClassicVertical(allow)
        end
    elseif cmd == "TURN" then
        local rate = tonumber(a) or 0.75
        if type(WXL_SkyridingSetTurnRate) == "function" then
            WXL_SkyridingSetTurnRate(rate)
        end
    end
end

local function OnAddonMessage(prefix, message)
    if prefix == PREFIX then
        HandleSkyPayload(message)
        return
    end
    if type(message) == "string" and message:find("^" .. PREFIX .. "\t") then
        HandleSkyPayload(message)
    end
end

frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("CHAT_MSG_WHISPER")
frame:RegisterEvent("CHAT_MSG_WHISPER_INFORM")
frame:RegisterEvent("UNIT_SPELLCAST_START")
frame:RegisterEvent("UNIT_SPELLCAST_SUCCEEDED")
frame:RegisterEvent("PLAYER_ENTERING_WORLD")
frame:RegisterEvent("PLAYER_LEAVING_WORLD")
frame:SetScript("OnEvent", function(self, event, ...)
    if event == "CHAT_MSG_ADDON" then
        local prefix, message = ...
        OnAddonMessage(prefix, message)
    elseif event == "CHAT_MSG_WHISPER" or event == "CHAT_MSG_WHISPER_INFORM" then
        local message = ...
        if type(message) == "string" and message:find(PREFIX, 1, true) then
            HandleSkyPayload(message)
        end
    elseif event == "UNIT_SPELLCAST_START" then
        -- Instant skyriding spells usually skip START; flap on SUCCEEDED + server ANIM.
        return
    elseif event == "UNIT_SPELLCAST_SUCCEEDED" then
        -- Flap only via server ANIM (one shot). SUCCEEDED+ANIM double-fired mid-chain.
        return
    elseif event == "PLAYER_LEAVING_WORLD" then
        -- Soft logout/relog: stop Tick/stall/SetBone before unit teardown (crash).
        if type(WXL_SkyridingSetMode) == "function" then
            WXL_SkyridingSetMode(0)
        end
        active = false
    elseif event == "PLAYER_ENTERING_WORLD" then
        -- World reload (login / LFG / taxi): wipe WXL edges then ask server for MODE.
        if type(WXL_SkyridingSetMode) == "function" then
            WXL_SkyridingSetMode(0)
        end
        if SendAddonMessage then
            SendAddonMessage(PREFIX, "RESYNC\t1", "WHISPER", UnitName("player"))
        end
        Refresh()
    end
end)

frame:SetScript("OnUpdate", function(self, elapsed)
    if not active then
        return
    end
    if charges < maxCharges and rechargeMs > 0 then
        rechargeMs = math.max(0, rechargeMs - elapsed * 1000)
        cdText:SetText(string.format("+1 in %.1fs", rechargeMs / 1000))
    end
end)

-- Always-on driver: hidden bar frames do not OnUpdate; map/FSM still need a tick.
local tickDriver = CreateFrame("Frame", "HorizonSkyridingTickDriver", UIParent)
local lastBrk = -1
tickDriver:SetScript("OnUpdate", function()
    if type(WXL_SkyridingTick) == "function" then
        WXL_SkyridingTick()
    end
    if active and type(WXL_SkyridingIsBraking) == "function" then
        local brk = WXL_SkyridingIsBraking() or 0
        if brk ~= lastBrk then
            lastBrk = brk
            if SendAddonMessage then
                SendAddonMessage(PREFIX, "BRK\t" .. tostring(brk), "WHISPER", UnitName("player"))
            end
        end
    end
    if active and type(WXL_SkyridingConsumeWallImpact) == "function" then
        if (WXL_SkyridingConsumeWallImpact() or 0) == 1 then
            if SendAddonMessage then
                SendAddonMessage(PREFIX, "WALL\t1", "WHISPER", UnitName("player"))
            end
        end
    end
    if active and type(WXL_SkyridingConsumeLand) == "function" then
        if (WXL_SkyridingConsumeLand() or 0) == 1 then
            if SendAddonMessage then
                SendAddonMessage(PREFIX, "LAND\t1", "WHISPER", UnitName("player"))
            end
        end
    end
end)
