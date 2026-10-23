-- TWInteractKey.lua
-- Client helper for tw-mod-interact-key (Tortoise-WoW)
--
-- The 1.12 client ignores server-initiated SMSG_LOOT_RESPONSE, so corpse looting
-- cannot be done from a server-side chat command alone. This addon receives the
-- target GUID from the server and sends CMSG_LOOT by calling the client's own
-- LootUnit function.
--
-- Auto-looting requires a custom 1.12 client that exposes C_Loot.LootUnit(guid),
-- such as clients with the ClassicAPI plugin. If C_Loot is not available, the
-- addon falls back to targeting the corpse so the player can press the normal
-- Interact key.

local ADDON_PREFIX = "TW_INTERACT"
local MACRO_NAME = "TWInteract"
local MACRO_BODY = ".interact"
local MACRO_ICON = "INV_Misc_Gear_01"
local DEBUG = false

TWInteractSettings = TWInteractSettings or {}

local frame = CreateFrame("Frame")
frame:RegisterEvent("CHAT_MSG_ADDON")
frame:RegisterEvent("VARIABLES_LOADED")

if RegisterAddonMessagePrefix then
    RegisterAddonMessagePrefix(ADDON_PREFIX)
end

local function LootUnitByGuid(guidHex)
    -- Custom clients (e.g. ClassicAPI-based) expose C_Loot.LootUnit(guidHex).
    if C_Loot and type(C_Loot) == "table" and C_Loot.LootUnit then
        local ok, err = pcall(function() C_Loot.LootUnit(guidHex) end)
        if ok then
            return true
        else
            DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r C_Loot.LootUnit failed: " .. tostring(err))
        end
    end
    return false
end

local function ApplySavedVisibility()
    TWInteractSettings = TWInteractSettings or {}
    local hidden = TWInteractSettings.buttonHidden
    if TWInteractActionButton then
        if hidden ~= false then
            TWInteractActionButton:Hide()
        else
            TWInteractActionButton:Show()
        end
    end
end

local function OnEvent()
    if event == "VARIABLES_LOADED" then
        ApplySavedVisibility()
        return
    end

    if event ~= "CHAT_MSG_ADDON" then
        return
    end

    if DEBUG then
        DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r CHAT_MSG_ADDON args: " ..
            tostring(arg1) .. " | " .. tostring(arg2) .. " | " .. tostring(arg3) .. " | " .. tostring(arg4))
    end

    local prefix = arg1
    local message = arg2

    -- Different clients/orderings for CHAT_MSG_ADDON args have been observed.
    -- Try the common layouts and fall back to the full-body-in-arg1 form.
    if prefix ~= ADDON_PREFIX then
        if arg2 == ADDON_PREFIX then
            prefix = arg2
            message = arg1
        elseif arg1 and strfind(arg1, "^" .. ADDON_PREFIX .. "\t") then
            message = strsub(arg1, strlen(ADDON_PREFIX) + 2)
            prefix = ADDON_PREFIX
        elseif arg1 and strfind(arg1, "^" .. ADDON_PREFIX .. ":") then
            message = strsub(arg1, strlen(ADDON_PREFIX) + 2)
            prefix = ADDON_PREFIX
        else
            return
        end
    end

    if not message then
        return
    end

    local cmd, rest = strsplit(":", message, 2)
    if cmd ~= "LOOT" or not rest then
        return
    end

    local guidHex, name = strsplit(":", rest, 2)
    if not guidHex or guidHex == "" then
        return
    end

    if LootUnitByGuid(guidHex) then
        return
    end

    -- Fallback for clients without C_Loot: target the unit and ask the player
    -- to press the normal Interact key.
    if name and name ~= "" then
        TargetByName(name, true)
        DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r targeted " .. name .. ". Press your Interact key to loot.")
    end
end

frame:SetScript("OnEvent", OnEvent)


-- ---------------------------------------------------------------------------
-- Draggable action-bar helper: create a macro and a button that can be
-- dragged onto the default action bar.
-- ---------------------------------------------------------------------------

local macroIndex = 0

local function EnsureMacro()
    local idx = GetMacroIndexByName(MACRO_NAME)
    if idx ~= 0 then
        return idx
    end

    local numGlobal, numPerChar = GetNumMacros()
    if numPerChar >= 18 then
        DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r per-character macro limit reached. Free a slot for '" .. MACRO_NAME .. "'.")
        return 0
    end

    idx = CreateMacro(MACRO_NAME, MACRO_ICON, MACRO_BODY, 1)
    if idx and idx ~= 0 then
        DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r created macro '" .. MACRO_NAME .. "'. Drag the on-screen button (or the macro) to your action bar.")
        return idx
    end
    return 0
end

local function SendServerInteract()
    local editBox = ChatFrameEditBox or ChatFrame1EditBox
    if editBox then
        editBox:SetText(".interact")
        ChatEdit_SendText(editBox)
    else
        SendChatMessage(".interact", "SAY")
    end
end

local function CreateActionButton()
    macroIndex = EnsureMacro()
    if macroIndex == 0 then
        return
    end

    local button = CreateFrame("Button", "TWInteractActionButton", UIParent)
    button:SetWidth(36)
    button:SetHeight(36)
    button:SetPoint("CENTER", 0, 100)
    button:SetMovable(true)
    button:EnableMouse(true)
    button:RegisterForClicks("LeftButtonUp")
    button:RegisterForDrag("LeftButton")

    local tex = button:CreateTexture(nil, "BACKGROUND")
    tex:SetAllPoints()
    tex:SetTexture("Interface/Icons/" .. MACRO_ICON)

    local border = button:CreateTexture(nil, "OVERLAY")
    border:SetTexture("Interface/Buttons/UI-Quickslot2")
    border:SetPoint("CENTER", 0, 0)
    border:SetWidth(56)
    border:SetHeight(56)

    button:SetScript("OnClick", function()
        SendServerInteract()
    end)

    button:SetScript("OnDragStart", function()
        if IsShiftKeyDown() then
            button:StartMoving()
        elseif macroIndex ~= 0 then
            PickupMacro(macroIndex)
        end
    end)

    button:SetScript("OnDragStop", function()
        button:StopMovingOrSizing()
    end)

    button:SetScript("OnEnter", function()
        GameTooltip:SetOwner(button, "ANCHOR_RIGHT")
        GameTooltip:SetText("TW Interact Key")
        GameTooltip:AddLine("Left-click: interact", 1, 1, 1)
        GameTooltip:AddLine("Drag: place on action bar", 1, 1, 1)
        GameTooltip:AddLine("Shift-drag: move button", 1, 1, 1)
        GameTooltip:Show()
    end)

    button:SetScript("OnLeave", function()
        GameTooltip:Hide()
    end)

end

local function SetButtonShown(shown)
    if not TWInteractActionButton then
        return
    end
    if shown then
        TWInteractActionButton:Show()
    else
        TWInteractActionButton:Hide()
    end
    TWInteractSettings.buttonHidden = not shown
end

SLASH_INTERACT1 = "/interact"
SlashCmdList["INTERACT"] = function(msg)
    local cmd = msg and strtrim(msg) or ""

    if cmd == "" then
        SendServerInteract()
    elseif cmd == "hide" then
        SetButtonShown(false)
    elseif cmd == "show" then
        SetButtonShown(true)
    elseif cmd == "toggle" then
        if TWInteractActionButton then
            SetButtonShown(not TWInteractActionButton:IsVisible())
        end
    elseif cmd == "reset" then
        if TWInteractActionButton then
            TWInteractActionButton:ClearAllPoints()
            TWInteractActionButton:SetPoint("CENTER", 0, 100)
        end
    elseif cmd == "debug" then
        DEBUG = not DEBUG
        DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r debug " .. (DEBUG and "enabled" or "disabled"))
    elseif cmd == "status" then
        local state = TWInteractSettings.buttonHidden
        if state == false then
            DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r saved state: shown")
        elseif state == true then
            DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r saved state: hidden")
        else
            DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r saved state: default (hidden)")
        end
    else
        DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact:|r unknown command '/interact " .. cmd .. "'. Use: hide, show, toggle, reset, status, debug")
    end
end

CreateActionButton()

DEFAULT_CHAT_FRAME:AddMessage("|cffffcc00TW Interact Key|r loaded. Type /interact hide to hide the button.")
