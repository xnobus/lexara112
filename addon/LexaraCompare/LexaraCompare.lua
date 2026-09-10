-- LexaraCompare - a fixed text sample for judging the Lexara port (HD MSDF).
--
-- The renderer itself is toggled with CTRL+ALT+F11 (handled in lexara112.dll,
-- inside the EndScene hook). This addon toggles nothing - it provides comparison
-- material, so that differences can be seen on the same text rather than from
-- memory.
--
-- Lua 5.0 / 1.12: table.getn instead of #, `this` in handlers, paths via [[...]].

-- Every row is its own {size, text} pair. Sizes and samples used to be separate
-- tables joined with math.mod, which meant a long text could land on a large size
-- and run outside the frame.
local LINES = {
    {  8, [[The quick brown fox jumps over the lazy dog. !@#$%^&*()]] },
    { 10, [[Sphinx of black quartz, judge my vow 123 +-=_~`|/\]] },
    { 12, [[abcdefghzxc,.-= and !@#$%^&*()_+[]{};:'"<>/?~`|]] },
    { 14, [[Illegal1 O0 rn/m cl/d ijl1 .,:;!? -- dots and dashes]] },
    { 18, [[The cat ate 0.5 of a bowl. It weighed 4-5 kg! (purr...) #1]] },
    { 24, [[abcdefghzxc,.-=+*&%$#@! 0123456789 <>[]{}()]] },
    { 32, [[MEOW ,.-=_ 0123 !@#$%^&*()]] },
    { 48, [[abc ,.-=_ !@#$%&* 123]] },
    { 72, [[Aa ,.-=_ !@#$% 12]] },
}

-- Typefaces worth comparing: each looks different once it goes through MSDF.
local FONTS = {
    { "FRIZQT",  [[Fonts\FRIZQT__.ttf]] },
    { "ARIALN",  [[Fonts\ARIALN.ttf]] },
    { "MORPHEUS",[[Fonts\MORPHEUS.ttf]] },
}

local frame
local activeFontIdx = 1

local function ApplyTexts()
    local path = FONTS[activeFontIdx][2]
    for i = 1, table.getn(frame.lines) do
        local l = frame.lines[i]
        local ok = l:SetFont(path, l.fontSize)
        if not ok then
            -- SetFont returns a status, so it works as a check: when the typeface
            -- does not exist we fall back to the default instead of leaving a blank.
            l:SetFont([[Fonts\FRIZQT__.ttf]], l.fontSize)
        end
    end
    frame.header:SetText("Lexara - " .. FONTS[activeFontIdx][1] ..
        "  (CTRL+ALT+F11 toggles the renderer)")
end

local function Build()
    local f = CreateFrame("Frame", "LexaraCompareFrame", UIParent)
    f:SetWidth(900)
    f:SetHeight(60)  -- the real height is set once the rows have been built
    f:SetPoint("CENTER", UIParent, "CENTER", 0, 0)
    f:SetBackdrop({
        bgFile = [[Interface\Tooltips\UI-Tooltip-Background]],
        edgeFile = [[Interface\Tooltips\UI-Tooltip-Border]],
        tile = true, tileSize = 16, edgeSize = 16,
        insets = { left = 4, right = 4, top = 4, bottom = 4 },
    })
    f:SetBackdropColor(0, 0, 0, 0.85)
    f:SetMovable(true)
    f:EnableMouse(true)
    f:RegisterForDrag("LeftButton")
    f:SetScript("OnDragStart", function() this:StartMoving() end)
    f:SetScript("OnDragStop", function() this:StopMovingOrSizing() end)

    local header = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    header:SetPoint("TOPLEFT", f, "TOPLEFT", 12, -10)
    f.header = header

    -- Right mouse button cycles the typeface - no extra controls needed.
    f:SetScript("OnMouseUp", function()
        if arg1 == "RightButton" then
            activeFontIdx = activeFontIdx + 1
            if activeFontIdx > table.getn(FONTS) then activeFontIdx = 1 end
            ApplyTexts()
        end
    end)

    f.lines = {}
    local y = -32
    for i = 1, table.getn(LINES) do
        local size = LINES[i][1]
        local text = LINES[i][2]

        local label = f:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        label:SetPoint("TOPLEFT", f, "TOPLEFT", 12, y)
        label:SetText(size .. "px")
        label:SetWidth(38)
        label:SetJustifyH("RIGHT")

        -- 1.12: calling SetText on a FontString WITHOUT a font set ends in a Lua
        -- error. So we inherit a template at creation time and SetFont only
        -- overrides the typeface and the size.
        local line = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
        line:SetPoint("TOPLEFT", f, "TOPLEFT", 56, y)
        line:SetJustifyH("LEFT")
        -- Without a width limit the longer lines ran outside the border.
        line:SetWidth(830)
        line:SetHeight(size + 6)
        line:SetText(text)
        line.fontSize = size
        table.insert(f.lines, line)

        y = y - (size + 12)
    end

    f:SetHeight(-y + 16)
    f:Hide()
    return f
end

SLASH_LEXARA1 = "/lexara"
SLASH_LEXARA2 = "/lex"
SlashCmdList["LEXARA"] = function()
    if not frame then
        frame = Build()
        ApplyTexts()
    end
    if frame:IsShown() then
        frame:Hide()
    else
        frame:Show()
    end
end

DEFAULT_CHAT_FRAME:AddMessage(
    "|cff66ccffLexaraCompare|r loaded. /lexara opens the panel, " ..
    "right-click on the panel changes the typeface, CTRL+ALT+F11 toggles the renderer.")
