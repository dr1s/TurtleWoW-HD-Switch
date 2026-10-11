-- HD Switch on the character select and character create screens.
-- Runs in the glue (login screens) Lua state, where addons are not loaded:
-- HDToggle.dll executes this chunk right after it registers HDPack_List /
-- HDPack_Set / HDPack_Info / HDPack_RefreshGlue there, before GlueXML is loaded,
-- so it waits for the screens to exist and then adds one button to each.
-- The button turns every pack off (remembering the selection) or back on, and
-- the character on screen is rebuilt with the new files.

local COLOR = "|cff66ccff"
local last -- packs that were on before "all off" (this glue session)
local buttons = {}
local pending, delay = nil, 0

local function Packs()
	local list, t, i = {}, { HDPack_List() }, 1
	while t[i] do
		table.insert(list, { letter = t[i], on = t[i + 1] and true or false })
		i = i + 2
	end
	return list
end

local function Allowed()
	if not HDPack_Info then
		return false
	end
	local _, ok, _, locked = HDPack_Info()
	return ok and not locked
end

local restored = false

local function Update()
	local allowed = Allowed()
	if not allowed and HDPack_Info and not restored then
		local _, _, _, locked = HDPack_Info()
		if locked then
			-- unsupported server: the DLL turns every pack back on (no world here, safe)
			restored = true
			HDPack_RefreshGlue("none")
		end
	end
	local on, total = 0, 0
	if allowed then
		for _, p in ipairs(Packs()) do
			total = total + 1
			if p.on then on = on + 1 end
		end
	end
	for _, b in pairs(buttons) do
		if not allowed or total == 0 then
			b:Hide()
		else
			if on == total then
				b:SetText("HD: On")
			elseif on == 0 then
				b:SetText("HD: Off")
			else
				b:SetText("HD: " .. on .. "/" .. total)
			end
			b.on, b.total = on, total
			b:Show()
		end
	end
end

-- rebuild the character on the visible screen with the new files
local function Rebuild()
	local screen = "none"
	if CharacterCreate:IsVisible() then
		screen = "create"
	elseif CharacterSelect:IsVisible() then
		screen = "select"
	end
	HDPack_RefreshGlue(screen)
	if screen == "select" and CharacterSelect.selectedIndex and CharacterSelect.selectedIndex ~= CharacterSelect.createIndex then
		CharacterSelect.currentModel = nil -- the background scene is rebuilt too
		CharacterSelect_SelectCharacter(CharacterSelect.selectedIndex, 1)
	end
	Update()
end

local function Toggle()
	if not Allowed() then
		return Update()
	end
	local packs, anyOn = Packs(), false
	for _, p in ipairs(packs) do
		if p.on then anyOn = true end
	end
	if anyOn then
		last = {}
		for _, p in ipairs(packs) do
			if p.on then
				last[p.letter] = true
				HDPack_Set(p.letter, 0)
			end
		end
	else
		local all = not last or next(last) == nil
		for _, p in ipairs(packs) do
			if all or last[p.letter] then
				HDPack_Set(p.letter, 1)
			end
		end
	end
	Update()
	pending, delay = true, 0.3 -- quick clicks are batched into one refresh
end

local function Tooltip()
	GlueTooltip:ClearAllPoints()
	GlueTooltip:SetPoint("TOP", this, "BOTTOM", 0, -4)
	GlueTooltip:Show()
	local newer = HDPack_Update and HDPack_Update()
	GlueTooltip_SetText(COLOR .. "HD Switch|r\n" .. (this.on or 0) .. " / " .. (this.total or 0) ..
		" HD packs on\nClick: all on / off" ..
		(newer and ("\n|cff66ff66Update available: " .. newer .. "|r") or ""), GlueTooltip)
end

-- errors here would be silent (no error frame on these screens): log them
local function Safe(fn)
	return function()
		local ok, err = pcall(fn)
		if not ok and HDPack_Log then
			HDPack_Log("character screens: " .. tostring(err))
		end
	end
end

local function Build()
	-- inside the screens' UI frames, like the game's own buttons: those frames
	-- cover the whole screen and take the mouse, so a sibling button gets no clicks
	for key, screen in pairs({ select = CharacterSelectUI, create = CharacterCreateFrame }) do
		local b = CreateFrame("Button", "HDSwitchGlueButton_" .. key, screen, "GlueButtonSmallTemplate")
		b:SetFrameLevel(screen:GetFrameLevel() + 5)
		b:SetWidth(130)
		b:SetHeight(32)
		b:SetPoint("TOP", screen, "TOP", 0, -12)
		b:RegisterForClicks("LeftButtonUp")
		b:SetScript("OnClick", Safe(Toggle))
		b:SetScript("OnEnter", Tooltip)
		b:SetScript("OnLeave", function() GlueTooltip:Hide() end)
		b:SetScript("OnShow", Update) -- the selection may have changed in the game
		buttons[key] = b
	end
	Update()
end

local driver = CreateFrame("Frame")
driver:SetScript("OnUpdate", function()
	if not buttons.select then
		if CharacterSelectUI and CharacterCreateFrame and GlueTooltip and CharacterSelect_SelectCharacter then
			Build()
		end
		return
	end
	if pending then
		delay = delay - arg1
		if delay <= 0 then
			pending = nil
			Safe(Rebuild)()
		end
	end
end)
