-- Lua 5.0 tests for dll/glue.lua (the HD Switch button on the character select
-- and create screens). The glue API and HDToggle.dll are faked.
-- Usage: lua test_glue.lua dll/glue.lua

local FILE = arg[1] or "dll/glue.lua"
local failures, count = 0, 0
local function check(cond, name)
	count = count + 1
	if cond then
		print("  ok    " .. name)
	else
		failures = failures + 1
		print("  FAIL  " .. name)
	end
end

-- Fake glue API ---------------------------------------------------------------
local frames = {}
local function frame(kind, name, parent, template)
	local f = { kind = kind, name = name, parent = parent, template = template, scripts = {}, shown = true, points = {} }
	function f:SetScript(n, fn) self.scripts[n] = fn end
	function f:GetScript(n) return self.scripts[n] end
	function f:IsVisible()
		local p = self
		while p do
			if not p.shown then return false end
			p = p.parent
		end
		return true
	end
	function f:Show()
		local was = self:IsVisible()
		self.shown = true
		if not was and self:IsVisible() then
			-- as in WoW: OnShow for this frame and every shown descendant that became visible
			local function fireShow(x)
				if x.scripts.OnShow then
					local old = this; this = x; x.scripts.OnShow(); this = old
				end
				for _, c in ipairs(frames) do
					if c.parent == x and c.shown then fireShow(c) end
				end
			end
			fireShow(self)
		end
	end
	function f:Hide() self.shown = false end
	function f:IsShown() return self.shown end
	function f:SetText(t) self.text = t end
	function f:SetWidth(w) self.width = w end
	function f:SetHeight(h) self.height = h end
	function f:SetPoint(...) table.insert(self.points, arg) end
	function f:ClearAllPoints() self.points = {} end
	function f:SetFrameLevel(l) self.level = l end
	function f:GetFrameLevel() return self.level or (self.parent and self.parent:GetFrameLevel() + 1) or 0 end
	function f:RegisterForClicks(b) self.clicks = b end
	table.insert(frames, f)
	if name then getfenv(0)[name] = f end
	return f
end
function CreateFrame(kind, name, parent, template) return frame(kind, name, parent, template) end

local selected = {}
local function setupScreens()
	CharacterSelect = frame("ModelFFX", "CharacterSelect")
	CharacterCreate = frame("ModelFFX", "CharacterCreate")
	CharacterCreate.shown = false
	CharacterSelectUI = frame("Frame", "CharacterSelectUI", CharacterSelect)
	CharacterCreateFrame = frame("Frame", "CharacterCreateFrame", CharacterCreate)
	CharacterSelect.selectedIndex, CharacterSelect.createIndex = 2, 4
	CharacterSelect.currentModel = "Orc"
	GlueTooltip = frame("Frame", "GlueTooltip")
	GlueTooltip.shown = false
	function GlueTooltip_SetText(text) GlueTooltip.text = text end
	function CharacterSelect_SelectCharacter(id, noCreate) table.insert(selected, { id = id, noCreate = noCreate, model = CharacterSelect.currentModel }) end
end

local function tick(sec)
	for _, f in ipairs(frames) do
		if f.scripts.OnUpdate and f:IsVisible() then
			local oldThis, oldArg = this, arg1
			this, arg1 = f, sec
			f.scripts.OnUpdate()
			this, arg1 = oldThis, oldArg
		end
	end
end
local function click(b) local old = this; this = b; b.scripts.OnClick(); this = old end

-- Fake HDToggle.dll -------------------------------------------------------------
local DLL = { off = {}, installed = { "A", "B", "C", "S", "T" }, refreshes = {}, locked = false }
function HDPack_Info()
	if DLL.locked then return "1.1.0", false, "not available on this server", true end
	return "1.1.0", true, "ready", false
end
function HDPack_List()
	if DLL.locked then return end
	local t = {}
	for _, h in ipairs(DLL.installed) do table.insert(t, h); table.insert(t, not DLL.off[h]) end
	return unpack(t)
end
function HDPack_Set(h, on)
	if DLL.locked then return nil end
	DLL.off[h] = (on == 0) or nil
	return true
end
function HDPack_RefreshGlue(screen) table.insert(DLL.refreshes, screen); return 3 end
local function onCount()
	local n = 0
	for _, h in ipairs(DLL.installed) do if not DLL.off[h] then n = n + 1 end end
	return n
end

-- Tests -------------------------------------------------------------------------
print("== loading before GlueXML")
dofile(FILE)
tick(0.1)
check(HDSwitchGlueButton_select == nil, "no button before the screens exist (chunk runs before GlueXML)")
setupScreens()
tick(0.1)
local bs, bc = HDSwitchGlueButton_select, HDSwitchGlueButton_create
check(bs and bc and bs.parent == CharacterSelectUI and bc.parent == CharacterCreateFrame,
	"buttons live inside the screens' full-screen UI frames (which take the mouse)")
check(bs:GetFrameLevel() > CharacterSelectUI:GetFrameLevel() and bs.clicks == "LeftButtonUp", "above them and clickable")
check(bs.template == "GlueButtonSmallTemplate", "uses the glue button template")
check(bs.text == "HD: On" and bc.text == "HD: On", "label shows all packs on")
check(bs:IsVisible() and not bc:IsVisible(), "only the visible screen's button shows")

print("== character select")
click(bs)
check(onCount() == 0 and bs.text == "HD: Off", "click turns every pack off, label updates at once")
tick(0.1)
check(table.getn(DLL.refreshes) == 0, "refresh is batched, not immediate")
tick(0.25)
check(table.getn(DLL.refreshes) == 1 and DLL.refreshes[1] == "select", "one refresh for the select screen")
check(table.getn(selected) == 1 and selected[1].id == 2 and selected[1].noCreate == 1 and selected[1].model == nil,
	"the selected character is re-selected with its background scene rebuilt")
tick(1)
check(table.getn(DLL.refreshes) == 1, "not repeated")
click(bs); click(bs); click(bs)
tick(0.35)
check(table.getn(DLL.refreshes) == 2 and onCount() == 5, "quick clicks: one refresh, final state applied")

print("== remembering the selection")
DLL.off.B = true
click(bs)
check(onCount() == 0, "partial selection: click turns everything off")
click(bs)
check(onCount() == 4 and DLL.off.B == true, "click again restores the previous selection, not all")
tick(0.35)

print("== character create")
CharacterSelect.shown = false
CharacterCreate:Show()
check(bc.text == "HD: 4/5", "shown screen updates its label (selection changed elsewhere)")
local nsel = table.getn(selected)
click(bc)
tick(0.35)
check(DLL.refreshes[table.getn(DLL.refreshes)] == "create", "the create screen asks the DLL to rebuild its character")
check(table.getn(selected) == nsel, "no character select call on the create screen")
-- clicked on create, but went back to select before the refresh ran
click(bc)
CharacterCreate:Hide(); CharacterSelect:Show()
tick(0.35)
check(DLL.refreshes[table.getn(DLL.refreshes)] == "select", "the refresh follows the screen visible when it runs")

print("== no characters yet")
CharacterSelect.selectedIndex = CharacterSelect.createIndex
nsel = table.getn(selected)
click(bs); tick(0.35)
check(table.getn(selected) == nsel, "the 'create new' slot is not selected (would open character create)")

print("== tooltip")
local old = this; this = bs; bs.scripts.OnEnter(); this = old
check(GlueTooltip:IsShown() and string.find(GlueTooltip.text, "HD Switch") and string.find(GlueTooltip.text, "/ 5 HD packs on"), "tooltip shows the count")
old = this; this = bs; bs.scripts.OnLeave(); this = old
check(not GlueTooltip:IsShown(), "tooltip hides")

print("== errors are logged")
local logged = {}
function HDPack_Log(m) table.insert(logged, m) end
local realSet = HDPack_Set
HDPack_Set = function() error("boom") end
click(bs)
check(table.getn(logged) == 1 and string.find(logged[1], "boom"), "a click error goes to HDToggle.log instead of vanishing")
HDPack_Set = realSet

print("== update notice")
function HDPack_Update() return "1.2.0" end
old = this; this = bs; bs.scripts.OnEnter(); this = old
check(string.find(GlueTooltip.text, "Update available: 1.2.0", 1, true) ~= nil, "glue tooltip shows the available version")
HDPack_Update = nil
old = this; this = bs; bs.scripts.OnEnter(); this = old
check(string.find(GlueTooltip.text, "Update", 1, true) == nil, "no line without an update (or with an older DLL)")

print("== not OctoWoW")
DLL.locked = true
local nref = table.getn(DLL.refreshes)
CharacterSelect:Hide(); CharacterSelect:Show()
check(not bs:IsShown() and not bc:IsShown(), "buttons are hidden")
check(table.getn(DLL.refreshes) == nref + 1 and DLL.refreshes[nref + 1] == "none", "one restoring refresh (every pack back on)")
CharacterSelect:Hide(); CharacterSelect:Show()
tick(1)
check(table.getn(DLL.refreshes) == nref + 1, "not repeated")

print(string.format("\n%d/%d tests passed", count - failures, count))
os.exit(failures == 0 and 0 or 1)
