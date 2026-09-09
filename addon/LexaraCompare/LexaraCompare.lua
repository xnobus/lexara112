-- LexaraCompare - stały wzorzec tekstu do oceny portu Lexary (HD MSDF).
--
-- Sam renderer przelacza sie skrotem CTRL+ALT+F (obsluga jest w lexara112.dll,
-- w haku EndScene). Ten addon niczego nie przelacza - daje material porownawczy,
-- zeby roznice bylo widac na tym samym tekscie, a nie z pamieci.
--
-- Lua 5.0 / 1.12: table.getn zamiast #, this w handlerach, sciezki przez [[...]].

-- Kazdy wiersz to wlasna para {rozmiar, tekst}. Wczesniej rozmiary i probki
-- byly osobnymi tablicami laczonymi przez math.mod, przez co przy duzych
-- rozmiarach trafial sie dlugi tekst i wyjezdzal poza ramke.
local LINIE = {
    {  8, [[Miau miau - kot chodzi wlasnymi drogami. !@#$%^&*()]] },
    { 10, [[Mruczek, Filemon, Bonifacy 123 kocury +-=_~`|/\]] },
    { 12, [[abcdefghzxc,.-= oraz !@#$%^&*()_+[]{};:'"<>/?~`|]] },
    { 14, [[Illegal1 O0 rn/m cl/d ijl1 .,:;!? -- kropki i myslniki]] },
    { 18, [[Kotek zjadl 0.5 miski. Wazyl 4-5 kg! (mruuu...) #1]] },
    { 24, [[abcdefghzxc,.-=+*&%$#@! 0123456789 <>[]{}()]] },
    { 32, [[MIAU ,.-=_ 0123 !@#$%^&*()]] },
    { 48, [[abc ,.-=_ !@#$%&* 123]] },
    { 72, [[Aa ,.-=_ !@#$% 12]] },
}

-- Kroje warte porownania: kazdy inaczej wyglada po przejsciu na MSDF.
local KROJE = {
    { "FRIZQT",  [[Fonts\FRIZQT__.ttf]] },
    { "ARIALN",  [[Fonts\ARIALN.ttf]] },
    { "MORPHEUS",[[Fonts\MORPHEUS.ttf]] },
}

local ramka
local aktywnyKrojIdx = 1

local function UstawTeksty()
    local sciezka = KROJE[aktywnyKrojIdx][2]
    for i = 1, table.getn(ramka.linie) do
        local l = ramka.linie[i]
        local ok = l:SetFont(sciezka, l.rozmiar)
        if not ok then
            -- SetFont zwraca status, wiec nadaje sie na kontrole:
            -- gdy kroj nie istnieje, wracamy na domyslny zamiast zostawic pustke.
            l:SetFont([[Fonts\FRIZQT__.ttf]], l.rozmiar)
        end
    end
    ramka.naglowek:SetText("Lexara - " .. KROJE[aktywnyKrojIdx][1] ..
        "  (CTRL+ALT+F11 przelacza renderer)")
end

local function Buduj()
    local f = CreateFrame("Frame", "LexaraCompareFrame", UIParent)
    f:SetWidth(900)
    f:SetHeight(60)  -- realna wysokosc ustawiana po zbudowaniu wierszy
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

    local naglowek = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
    naglowek:SetPoint("TOPLEFT", f, "TOPLEFT", 12, -10)
    f.naglowek = naglowek

    -- Prawy przycisk myszy przelacza kroj - bez dodatkowych kontrolek.
    f:SetScript("OnMouseUp", function()
        if arg1 == "RightButton" then
            aktywnyKrojIdx = aktywnyKrojIdx + 1
            if aktywnyKrojIdx > table.getn(KROJE) then aktywnyKrojIdx = 1 end
            UstawTeksty()
        end
    end)

    f.linie = {}
    local y = -32
    for i = 1, table.getn(LINIE) do
        local rozmiar = LINIE[i][1]
        local tresc = LINIE[i][2]

        local etykieta = f:CreateFontString(nil, "OVERLAY", "GameFontDisableSmall")
        etykieta:SetPoint("TOPLEFT", f, "TOPLEFT", 12, y)
        etykieta:SetText(rozmiar .. "px")
        etykieta:SetWidth(38)
        etykieta:SetJustifyH("RIGHT")

        -- 1.12: SetText na FontString BEZ ustawionej czcionki konczy sie bledem
        -- Lua. Dziedziczymy wiec szablon przy tworzeniu, a SetFont tylko
        -- nadpisuje kroj i rozmiar.
        local linia = f:CreateFontString(nil, "OVERLAY", "GameFontNormal")
        linia:SetPoint("TOPLEFT", f, "TOPLEFT", 56, y)
        linia:SetJustifyH("LEFT")
        -- Bez ograniczenia szerokosci dluzsze linie wychodzily poza obramowanie.
        linia:SetWidth(830)
        linia:SetHeight(rozmiar + 6)
        linia:SetText(tresc)
        linia.rozmiar = rozmiar
        table.insert(f.linie, linia)

        y = y - (rozmiar + 12)
    end

    f:SetHeight(-y + 16)
    f:Hide()
    return f
end

SLASH_LEXARA1 = "/lexara"
SLASH_LEXARA2 = "/lex"
SlashCmdList["LEXARA"] = function()
    if not ramka then
        ramka = Buduj()
        UstawTeksty()
    end
    if ramka:IsShown() then
        ramka:Hide()
    else
        ramka:Show()
    end
end

DEFAULT_CHAT_FRAME:AddMessage(
    "|cff66ccffLexaraCompare|r wczytany. /lexara otwiera panel, " ..
    "prawy przycisk na panelu zmienia kroj, CTRL+ALT+F11 przelacza renderer.")
