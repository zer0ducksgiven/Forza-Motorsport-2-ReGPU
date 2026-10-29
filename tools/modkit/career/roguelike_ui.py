"""Roguelike Career's changes to the game's career flows (FM2-roguelike-scope.md), made from the player's own UI.zip.
random_events.py --mode roguelike calls build(); the files go in the pack's Media/DLCZips/overlay/game/media/ui.zip,
which the port merges into UI.zip while the pack is switched on (kernel/zip_overlay.cpp).

The port fills in the Lua markers as the flows load (hooks/roguelike.cpp):
  false --[[roguelike: run]]     true when this profile's career is a Roguelike run (profiles/<name>/roguelike.ini)
  2 --[[roguelike: region]]      a new career's home region (1 Asia, 2 Europe, 3 North America), picked at random
  0 --[[roguelike: car]]         a new career's car: a random Class D road car (RoguelikeCars.txt)
  false --[[roguelike: full]]    true with FM2_ROGUELIKE_FULL=1: the garage counts as full (to test the block)
and reads the notes the flows load as Lua text, unrun:
  --roguelike: run started        the first car is bought: this profile's career is a run from now on
  --roguelike: reveal <car>       the car reward screen shows this car next (the port's RewardReveal hooks)
  --roguelike: got <car>          the race's car was given
and answers as it loads:
  return 0 --[[roguelike: next car event=<id> place=<n> class=<c>]]   the race's car (0: none due, -1: a win without one,
                                                                       -2: a strike, -3: the second, the car raced lost)
  return 0 --[[roguelike: level]]       the run's level (one up a podium, to 30; -1: not a run)
  return 0 --[[roguelike: strikes]]     the run's strikes (races in a row 4th or worse)
  return 0 --[[roguelike: reset car]]   the last car lost: a new run (level and strikes 0) on this random Class D car
"""
import os, re, xml.dom.minidom, zipfile

RUN = 'false --[[roguelike: run]]'
REGION = '2 --[[roguelike: region]]'
CAR = '0 --[[roguelike: car]]'
FULL = 'false --[[roguelike: full]]'
BLOCKED_TITLE = 'Rogue Mode'
BLOCKED_TEXT = ("This profile already has a career. Rogue Mode needs a new profile: make one in the launcher's "
                "Profiles tab, or switch Rogue Mode off in the launcher's Settings tab.")


def once(text, pattern, repl, what):
    out, n = re.subn(pattern, repl, text, count=1, flags=re.S)
    if n != 1:
        raise SystemExit('roguelike_ui: %s not found in this UI.zip' % what)
    return out


def remove(d, pattern, what):
    """Entries out of a menu (Buy Car: the run's cars only come from wins; Restart: a lost race stays lost)."""
    out, n = re.subn(pattern, '', d)
    if not n:
        raise SystemExit('roguelike_ui: %s not found in this UI.zip' % what)
    return out


# Each car's strikes, as its car level (the garage's Lvl: a run earns no XP), which Select Car's badge shows as
# Strikes (random_events.py renames it)
SYNC_STRIKES = r'''pcall(function()
	local strikes, garage = loadstring("return {} --[[roguelike: all strikes]]")(), Lobby.activePlayer.DB.Garage
	DB:execute("UPDATE " .. garage .. " SET Lvl = 0")
	for gid, n in pairs(strikes) do
		DB:execute("UPDATE " .. garage .. " SET Lvl = " .. n .. " WHERE Id = " .. gid)
	end
end)'''

# The career menu's title bar (P_MAI_Background.xgf shows UI.Flow.SceneTitle): the run's level, and the current car's
# strikes as two crosses, dark grey and red once struck ({colour} paints the title's text and its shadow alike, so
# the level is left as it is, its outline kept)
TITLE = ('\t\tpcall(function()\r\n'
         '\t\t\tlocal level = loadstring("return 0 --[[roguelike: level]]")()\r\n'
         '\t\t\tlocal strikes = loadstring("return 0 --[[roguelike: strikes " .. ProfileManager.Main.CareerCar .. "]]")()\r\n'
         '\t\t\tlocal marks = ""\r\n'
         '\t\t\tfor i = 1, 2 do\r\n'
         '\t\t\t\tmarks = marks .. (i > strikes and "{colour=\'70 70 70 255\'}X " or "{colour=\'210 20 20 255\'}X ")\r\n'
         '\t\t\tend\r\n'
         '\t\t\tUI.Flow.SceneTitle = UI.Flow.SceneTitle .. "   Level " .. level .. "   " .. marks\r\n'
         '\t\tend)\r\n\t\t' + SYNC_STRIKES.replace('\n', '\r\n\t\t'))


def career_menu(d):
    """A career not started as a Roguelike run is turned away (the run needs a fresh profile); no Buy Car."""
    d = remove(d, r'[ \t]*\{Text="\[UI\.Strings\.IDS_BUY_CAR\]"[^\n]*\n', 'the career menu\'s Buy Car')
    d = once(d, r'(<string id="IDS_CAREER_MENU_TITLE"\s+text=")[^"]*', lambda m: m.group(1) + 'Rogue Motorsport',
             'the career menu\'s title')
    d = once(d, r'(\n[ \t]*UI\.Flow\.SceneTitle = UI\.Strings\.IDS_CAREER_MENU_TITLE:GetText\(\))(\r?\n)',
             lambda m: m.group(1) + m.group(2) + TITLE + m.group(2), 'the career menu\'s title bar')
    d = once(d, r'(if Lobby\.activePlayer\.garage\.numCars == 0 then\s*UI:setEvent\("FirstTimeCareer"\))(\s*end)',
             lambda m: m.group(1) + '\r\n\t\telseif not (%s) then\r\n\t\t\tUI:setEvent("RoguelikeBlocked")' % RUN + m.group(2),
             'the first-career check')
    popup = ('\r\n\t\t<choice event="RoguelikeBlocked">\r\n'
             '\t\t\t<popup path="GAME:\\Media\\UI\\Scenes\\PopUp_Message.xgf">\r\n'
             '\t\t\t\tPopupTitle = "%s"\r\n'
             '\t\t\t\tPopupMessage = "%s"\r\n'
             '\t\t\t\tPopupButtons =\r\n\t\t\t\t{\r\n\t\t\t\t\tA = { Event = "Continue" },\r\n\t\t\t\t}\r\n'
             '\t\t\t\tHelpButtonTexts =\r\n\t\t\t\t{\r\n'
             '\t\t\t\t\tA = "{colour=\'255 255 255 255\'}[UI.Strings.IDS_ButtonContinue]",\r\n\t\t\t\t}\r\n'
             '\t\t\t</popup>\r\n\t\t\t<back/>\r\n\t\t</choice>') % (BLOCKED_TITLE, BLOCKED_TEXT)
    return once(d, r'(<choice event="FirstTimeCareer">.*?</choice>)', lambda m: m.group(1) + popup,
                'the first-career branch')


def first_time_career(d):
    """A new career asks nothing: the port picks the home region and a Class D car, the difficulty is the game's
    default (Intermediate: the flow's Difficulty == 1 block still runs), the car is bought and shown with the car
    reward screen, and the save and the career main menu follow. The run starts once the car is bought."""
    scene = lambda path: r'\s*<scene path="GAME:\\Media\\UI\\Scenes\\C_Scenes\\%s\.xgf">.*?</scene>' % path
    # the region screen and its explanation pop-up, the starter cars, the can't-afford check, the difficulty screen
    d = once(d, scene('C_BUY_RegionWarning') + r'\s*<branch>\s*<choice event="DisplayRegionExplanation">.*?</branch>', '',
             'the region screen')
    d = once(d, scene('C_CAR_CarSelect'), '', 'the starter car screen')
    d = once(d, r'\s*<!-- Catch cars you can\'t afford\..*?<!-- End -->', '', 'the can\'t-afford check')
    d = once(d, scene('C_MYC_SetDifficulty'), '', 'the difficulty screen')
    d = once(d, r'\s*<popup path="GAME:\\Media\\UI\\Scenes\\PopUp_Message\.xgf">\s*PopupTitle = "\[UI\.Strings\.IDS_CONFIRMATION\]".*?</popup>',
             '', 'the confirmation')
    # the port's choices
    d = once(d, r'(UI\.Flow\.Difficulty = 1)', lambda m: m.group(1) +
             '\r\n\t\tUI.Flow.Region = %s\r\n\t\tUI.Flow.Car = %s\r\n\t\tUI.Flow.SelectedColor = 1' % (REGION, CAR),
             'the default difficulty')
    # bought with credits lent for it (the run starts with none), then the reward screen
    d = once(d, r'(\t*)(local gId = Lobby\.activePlayer\.garage:BuyCar\(UI\.Flow\.Car, UI\.Flow\.SelectedColor\))',
             lambda m: m.group(1) + 'ProfileManager.Main.Credits = 10000000\r\n' + m.group(1) + m.group(2), 'the first car\'s purchase')
    d = once(d, r'(Lobby\.activePlayer\.garage:GetInCar\(gId\))(\s*</logic>)', lambda m: m.group(1) +
             '\r\n\t\tProfileManager.Main.Credits = 0'
             '\r\n\t\tloadstring("--roguelike: run started")'
             '\r\n\t\tloadstring("--roguelike: reveal " .. tostring(UI.Flow.Car))' + m.group(2) +
             '\r\n\t<flow path="GAME:\\Media\\UI\\Flows\\CarReveal.xml"/>', 'the first car\'s purchase')
    # the reward screen's scene in the flow's package (as CareerCircuitRace.xml's CarReveal-Scene)
    return once(d, r'(<package name="firsttimecareerpackage">)', lambda m: m.group(1) +
                '\r\n            <flow path="GAME:\\Media\\UI\\Flows\\CarReveal.xml"/>'
                '\r\n            <scene path="GAME:\\Media\\UI\\Scenes\\C_Scenes\\C_WIN_CarReveal.xgf"/>'
                '\r\n            <package name="GAME:\\Media\\UI\\Scenes\\C_Scenes\\C_WIN_CarReveal.xgf"/>', 'the flow\'s package')


NEXT_CAR = r'''
		if %s then
			-- after a win: a part, and one time in five a car (the port draws it), bought with credits lent for it
			local cls = 0
			pcall(function()
				cls = DB:execute("SELECT ClassID FROM " .. Lobby.activePlayer.DB.Garage .. " WHERE Id = " ..
				                 ProfileManager.Main.CareerCar):toTable()[1].ClassID
			end)
			local raced = ProfileManager.Main.CareerCar
			local car = loadstring("return 0 --[[roguelike: next car event=" .. tostring(RewardReveal.EventId) ..
			                       " place=" .. tostring(RewardReveal.OverallFinish) .. " class=" .. tostring(cls) ..
			                       " gid=" .. tostring(raced) .. "]]")()
			-- 4th or worse is a strike for the car raced (-2); its second (-3) loses it, with no money for it, and losing
			-- the last car ends the run: a new one starts on a random Class D car (STRIKE says so)
			local function name(id)
				local dc = DB:execute("SELECT Year, MakeID FROM Data_Car WHERE Id = " .. id):toTable()[1]
				return dc.Year .. " " .. loadstring("return '' --[[roguelike: name " .. dc.MakeID .. " " .. id .. "]]")()
			end
			local function garageName(gid)
				return name(DB:execute("SELECT CarId FROM " .. Lobby.activePlayer.DB.Garage .. " WHERE Id = " .. gid):toTable()[1].CarId)
			end
			UI.Flow.RogueStrikeText = nil
			if car == -2 then
				local this = "your car"
				pcall(function() this = "your " .. garageName(raced) end)
				UI.Flow.RogueStrikeTitle = "Warning"
				UI.Flow.RogueStrikeText = "You finished 4th or worse: strike 1 of 2 for " .. this ..
				                          ".\n\nOne more loss in this car and you will lose it."
			elseif car == -3 then
				local p = ProfileManager.Main
				local c = p.Credits
				pcall(function()
					local garage, table_ = Lobby.activePlayer.garage, Lobby.activePlayer.DB.Garage
					local gid = raced
					local lost = garageName(gid)
					local others = DB:execute("SELECT Id FROM " .. table_ .. " WHERE Id &lt;&gt; " .. gid .. " ORDER BY Id DESC"):toTable() or {}
					local text = "You have lost your car:\n" .. lost
					if table.getn(others) > 0 then
						garage:GetInCar(others[1].Id) -- the newest car left
					else
						local new = loadstring("return 0 --[[roguelike: reset car]]")()
						p.Credits = c + 10000000
						garage:BuyCar(new, 1) -- and gets in it
						text = text .. "\n\nIt was your last car: the run is over. A new run starts at level 0 with a " .. name(new) .. "."
					end
					garage:SellCar(gid)
					UI.Flow.RogueStrikeTitle = "Pink Slip"
					UI.Flow.RogueStrikeText = text
					loadstring("--roguelike: car lost " .. gid)
				end)
				p.Credits = c -- the sale's money, and the credits lent, taken back
			end
			@SYNC@
			pcall(function() ProfileManager.activePlayerProfile:Save() end)
			if car and (car > 0 or car == -1) then
				if car > 0 then
					local p = ProfileManager.Main
					local c = p.Credits
					p.Credits = c + 10000000
					-- BuyCar gets the player into the car it buys: back into the one they were in
					local current = ProfileManager.Main.CareerCar
					pcall(function() Lobby.activePlayer.garage:BuyCar(car, 1) end)
					pcall(function() Lobby.activePlayer.garage:GetInCar(current) end)
					p.Credits = c
				end
				-- a part for the car raced: a random type, then a random level of it that fits, owned (Buy
				-- Upgrades offers it to install) and at its price (it sells like a bought part)
				UI.Flow.RoguePartText = nil
				UI.Flow.RoguePartGid = nil
				pcall(function()
					local roll = function(n) return loadstring("return 0 --[[roguelike: roll " .. n .. "]]")() end
					local gid = ProfileManager.Main.CareerCar
					local g = DB:execute("SELECT * FROM " .. Lobby.activePlayer.DB.Garage .. " WHERE Id = " .. gid):toTable()[1]
					local dc = DB:execute("SELECT CarTypeID, Year, MakeID FROM Data_Car WHERE Id = " .. g.CarId):toTable()[1]
					if dc.CarTypeID == 3 then
						return -- race cars take no upgrades
					end
					local carName = dc.Year .. " " .. loadstring("return '' --[[roguelike: name " .. dc.MakeID .. " " .. g.CarId .. "]]")()
					local engine = DB:execute("SELECT EngineID FROM List_UpgradeEngine WHERE Id = " .. tostring(g.Engine) ..
					                          " OR (Ordinal = " .. g.CarId .. " AND IsStock = 1) ORDER BY IsStock"):toTable()[1].EngineID
					-- PartEnum, table (List_Upgrade...), the garage's column, the name, brand column or not
					local PARTS = {
						{ 1, "Brakes", "Brakes", "Brakes", true }, { 2, "SpringDamper", "SpringDamper", "Springs and Dampers", true },
						{ 3, "AntiSway", "AntiSway", "Anti-roll Bars", true }, { 4, "Transmission", "Transmission", "Transmission", true },
						{ 5, "Clutch", "Clutch", "Clutch", true }, { 6, "Flywheel", "Flywheel", "Flywheel", false },
						{ 7, "Driveline", "Driveline", "Driveline", true }, { 8, "Differential", "Differential", "Differential", true },
						{ 9, "TireCompound", "TireCompound", "Tire Compound", true }, { 10, "TireWidth", "TireWidth", "Tire Width", true },
						{ 11, "Weight", "WeightReduction", "Weight Reduction", true }, { 12, "FrontBumper", "FrontBumper", "Front Bumper", false },
						{ 13, "RearWing", "RearWing", "Rear Wing", false }, { 14, "RearBumper", "RearBumper", "Rear Bumper", false },
						{ 15, "SideSkirt", "SideSkirt", "Side Skirts", false }, { 16, "Hood", "Hood", "Hood", false },
						{ 17, "RimSize", "RimSize", "Rim Size", true }, { 18, "EngineCamValves", "CamValves", "Cams and Valves", true },
						{ 19, "EngineBlock", "Block", "Engine Block", true }, { 21, "EngineIgnition", "Ignition", "Ignition", true },
						{ 22, "EngineExhaust", "Exhaust", "Exhaust", true }, { 23, "EngineIntake", "Intake", "Intake", true },
						{ 24, "EngineTurbo", "Turbo", "Turbo", true }, { 25, "EngineCSC", "SuperchargerCSC", "Centrifugal Supercharger", true },
						{ 26, "EngineDSC", "SuperchargerDSC", "Positive Displacement Supercharger", true },
						{ 27, "EngineIntercooler", "Intercooler", "Intercooler", true },
					}
					local types = {}
					for _, t in ipairs(PARTS) do
						local key = string.sub(t[2], 1, 6) == "Engine" and ("EngineID = " .. engine) or ("Ordinal = " .. g.CarId)
						local parts = DB:execute("SELECT Id, Level, Price" .. (t[5] and ", ManufacturerID" or "") .. " FROM List_Upgrade" ..
							t[2] .. " WHERE " .. key .. " AND IsStock = 0 AND Id NOT IN (SELECT PartId FROM " ..
							Lobby.activePlayer.DB.PurchasedParts .. " WHERE GarageId = " .. gid .. " AND PartEnum = " .. t[1] ..
							") AND Id NOT IN (" .. tostring(g[t[3]] or 0) .. ") ORDER BY Id"):toTable() or {} -- nil when none
						if table.getn(parts) > 0 then table.insert(types, { t, parts }) end
					end
					if table.getn(types) == 0 then return end
					local pick = types[roll(table.getn(types))]
					local t, part = pick[1], pick[2][roll(table.getn(pick[2]))]
					local man = part.ManufacturerID or 0
					DB:execute("INSERT INTO " .. Lobby.activePlayer.DB.PurchasedParts ..
					           " (GarageId, PartEnum, PartId, ManufacturerId, PricePaid) VALUES (" ..
					           gid .. ", " .. t[1] .. ", " .. part.Id .. ", " .. man .. ", " .. part.Price .. ")")
					local brand = "" -- body and aero parts have none
					local m = DB:execute("SELECT PartManufacturer FROM List_PartManufacturer WHERE Id = " .. man):toTable()
					if m and m[1] and m[1].PartManufacturer ~= "Stock" then brand = m[1].PartManufacturer .. " " end
					UI.Flow.RoguePartText = "You won a part for your " .. carName .. ": " .. brand .. t[4] ..
					                        " (level " .. part.Level .. ")."
					-- its render, as Buy Upgrades shows it (Upgrades: a picture per type and level)
					local img = DB:execute("SELECT u.ImagePath FROM Upgrades u JOIN UpgradeTypes y ON y.id = u.TypeId WHERE y.PartEnum = " ..
					                       t[1] .. " AND y.IsException = 0 AND u.Level = " .. part.Level):toTable()
					UI.Flow.RoguePartImage = img and img[1] and img[1].ImagePath or ""
					-- UI.Flow keeps plain values only (a table is lost): the part, as fields, for Equip
					UI.Flow.RoguePartGid, UI.Flow.RoguePartEnum, UI.Flow.RoguePartId, UI.Flow.RoguePartMan = gid, t[1], part.Id, man
					loadstring("--roguelike: part " .. t[1] .. " " .. part.Id .. " for garage car " .. gid)
				end)
				-- saved now: the race's own save came before these screens
				pcall(function() ProfileManager.activePlayerProfile:Save() end)
				if car > 0 then
					loadstring("--roguelike: got " .. car)
					loadstring("--roguelike: reveal " .. car)
				end
			end
		end
'''


def car_reveal(d):
    """After every race of a run, the next car is bought and shown with the car reward screen (the port's
    RewardReveal hooks make the screen's own check and car come from the reveal note)."""
    d = once(d, r'(<logic>)(\s*if RewardReveal:CheckForCarUnlocks\(\) then)', lambda m: m.group(1) +
             NEXT_CAR.replace('@SYNC@', SYNC_STRIKES.replace('\n', '\n\t\t\t')).replace('\n', '\r\n') % RUN +
             m.group(2), 'the car reward check')
    return once(d, r'(\s*</flow>\s*)$', lambda m: (PART_WON + STRIKE).replace('\n', '\r\n') + m.group(1), 'the flow\'s end')


PART_WON = r'''
	<!-- Roguelike: the part a win earned: the Event Results scene (CareerTrophy.xml's), its pictures the part's render
	     from Buy Upgrades and no trophy (4th place has no trophy video), then the choice to fit it now -->
	<logic>
		if UI.Flow.RoguePartGid and UI.Flow.RoguePartText then
			UI:setEvent("RoguelikePart")
		end
	</logic>
	<branch>
		<choice event="RoguelikePart">
			<scene path="GAME:\Media\UI\Scenes\C_Scenes\C_Win_Career_Trophy.xgf">
				FinishPlace = 4
				Title = "Part Won"
				EventRewardQuery = DB:execute("SELECT Events.Name, 11 AS CareerTypeId, Events.EventIcon FROM Events WHERE Events.Id = 910")
				EventBkgd = UI.Flow.RoguePartImage
				HasReward = false
				RewardMsg = UI.Flow.RoguePartText
				MessageTable = { }
			</scene>
			<!-- the pop-up's B is the engine's Back, to the scene before it: this one, which goes straight on (past the
			     pop-up, already asked) so No carries on to the career -->
			<scene>
				<onEnter>
					UI.Flow.RogueStepped = nil
				</onEnter>
				<onUpdate>
					if not UI.Flow.RogueStepped then
						UI.Flow.RogueStepped = true
						UI:flowProcess()
					end
				</onUpdate>
			</scene>
			<logic>
				if not UI.Flow.RogueInstallAsked then
					UI.Flow.RogueInstallAsked = true
					UI:setEvent("RogueInstall")
				end
			</logic>
			<branch>
				<choice event="RogueInstall">
					<!-- Buy Upgrades' own Install pop-up (Upgrades.xml): the car's class and PI now and with the part,
					     and its weight and power as the shop's list sets them (UpgradeStats.lua) -->
					<popup path="GAME:\Media\UI\Scenes\C_Scenes\C_UPG_BuyPopUp.xgf">
						GarageQuery = "SELECT * FROM " .. Lobby.activePlayer.DB.Garage .. " WHERE Id=" .. UI.Flow.RoguePartGid
						local now = Lobby.activePlayer.garage:GetCurCar()
						CareerMenuCar = now:DuplicateCar()
						CareerMenuCar:SetInstalledPart(UI.Flow.RoguePartEnum, UI.Flow.RoguePartId, UI.Flow.RoguePartMan)
						pcall(function()
							local new = CareerMenuCar
							UI.Flow.UpgradeWeight = UserInterface.NumUnitString.new(new.CurbWeight, UI.Units.Mass):GetText()
							UI.Flow.UpgradePower = UserInterface.NumUnitString.new(new.SimPeakPower, UI.Units.Power):GetText()
							local function colour(better, worse)
								if better then return '44, 94, 36, 255' elseif worse then return '136, 5, 5, 255' end
							end
							UI.Flow.UpgradeWeightColor = colour(now.CurbWeight > new.CurbWeight, new.CurbWeight > now.CurbWeight)
							UI.Flow.UpgradePowerColor = colour(new.SimPeakPower > now.SimPeakPower, now.SimPeakPower > new.SimPeakPower)
							loadstring("--roguelike: part stats " .. now.CurbWeight .. " " .. new.CurbWeight .. " " ..
							           now.SimPeakPower .. " " .. new.SimPeakPower)
						end)
						PopupTitle = "Install Part"
						PopupMessage = "Install part? You can also install it later from Buy Upgrades."
						HelpButtonTexts =
						{
							A = "{colour='255 255 255 255'}[UI.Strings.IDS_Yes]",
							B = "{colour='255 255 255 255'}[UI.Strings.IDS_No]"
						}
					</popup>
					<logic>
						-- fitted as Buy Upgrades' Install does it, and saved
						UI:fireControlEvent("InstallPartSound")
						local f = UI.Flow
						pcall(function()
							Lobby.activePlayer.garage:InstallUpgrade(f.RoguePartGid, f.RoguePartEnum, f.RoguePartId, f.RoguePartMan)
						end)
						pcall(function() ProfileManager.activePlayerProfile:Save() end)
						loadstring("--roguelike: part installed")
					</logic>
				</choice>
			</branch>
		</choice>
	</branch>
	<logic>
		UI.Flow.RoguePartGid = nil
		UI.Flow.RoguePartText = nil
		UI.Flow.RogueInstallAsked = nil
	</logic>'''

STRIKE = r'''
	<!-- Roguelike: a strike's warning, or the second's Pink Slip (NEXT_CAR), confirmed with A. B would go back to the
	     scene before the pop-up: the pass-through one, which goes straight on past it (already shown) -->
	<logic>
		if UI.Flow.RogueStrikeText then
			UI:setEvent("RogueStrike")
		end
	</logic>
	<branch>
		<choice event="RogueStrike">
			<!-- behind the pop-up, the career's background (Forza.xml's: the showroom and the current car, its title bar
			     the pop-up's; a loss has no Event Results to show it over) -->
			<scene path="GAME:\Media\UI\Scenes\P_Scenes\P_MAI\P_MAI_Background.xgf">
				<OnEnter>
					CareerMenuCar = Lobby.activePlayer.garage:GetCurCar()
					if not UI.Flow.RogueStrikeShown then
						UI.Flow.RogueTitleBefore = UI.Flow.SceneTitle
					end
					UI.Flow.SceneTitle = UI.Flow.RogueStrikeTitle
					UI.Flow.RogueStrikeStepped = nil
				</OnEnter>
				<OnUpdate>
					if not UI.Flow.RogueStrikeStepped then
						UI.Flow.RogueStrikeStepped = true
						UI:flowProcess()
					end
				</OnUpdate>
			</scene>
			<logic>
				if not UI.Flow.RogueStrikeShown then
					UI.Flow.RogueStrikeShown = true
					UI:setEvent("RogueStrikeShow")
				end
			</logic>
			<branch>
				<choice event="RogueStrikeShow">
					<popup path="GAME:\Media\UI\Scenes\PopUp_Message.xgf">
						PopupTitle = UI.Flow.RogueStrikeTitle
						PopupMessage = UI.Flow.RogueStrikeText
						PopupButtons =
						{
							A = { Event = "OK" },
						}
						HelpButtonTexts =
						{
							A = "{colour='255 255 255 255'}Confirm",
						}
					</popup>
				</choice>
			</branch>
		</choice>
	</branch>
	<logic>
		if UI.Flow.RogueStrikeText then
			UI.Flow.SceneTitle = UI.Flow.RogueTitleBefore
		end
		UI.Flow.RogueStrikeText = nil
		UI.Flow.RogueStrikeShown = nil
	</logic>'''


def quick_status(d):
    """No Buy New Car in the post-race menu, and its message without the driver level (a run earns no XP): the
    game's own level-cap wording, always."""
    d = once(d, r'if Status\.NextLvl == nil then(\s*stringToUse = stringToUse \.\. "_MaxLvl")',
             lambda m: 'if true then -- Roguelike: no driver level' + m.group(1), 'the quick status\'s message')
    return remove(d, r'[ \t]*\{ Label="\[UI\.Strings\.IDS_Choice_BuyCar\]",\s*Event="buycar"\s*\},\r?\n', 'the quick status\'s Buy Car')


GARAGE_FULL = r'''
	<logic>
		if Lobby.activePlayer.garage.isFull or %s then
			UI:setEvent("RoguelikeGarageFull")
		end
	</logic>
	<branch>
		<choice event="RoguelikeGarageFull">
			<popup path="GAME:\Media\UI\Scenes\PopUp_Message.xgf">
				PopupTitle = "GARAGE IS FULL"
				PopupMessage = "Your garage is full, and a win needs room for its car. Sell a car in My Cars before you race."
				PopupButtons =
				{
					A = { Event = "Continue" },
				}
				HelpButtonTexts =
				{
					A = "{colour='255 255 255 255'}[UI.Strings.IDS_ButtonContinue]",
				}
			</popup>
			<back/>
		</choice>
	</branch>
'''


def event_select(d):
    """A full garage blocks racing (a win needs room for its car): checked on the way into Race Type and into Event
    Select (the post-race menu goes straight there)."""
    block = GARAGE_FULL.replace('\n', '\r\n') % FULL
    d = once(d, r'([ \t]*<!-- Type Select -->)', lambda m: block + m.group(1), 'Race Type')
    return once(d, r'(<flow label="CareerEventSelect">)', lambda m: m.group(1) + block, 'Event Select')


SALE = r'''UI.Flow.SalePrice = 0
				pcall(function()
					-- the run's sale price: 10% of the dealership's (the home region's price list)
					local prices = DB:execute("SELECT TableName FROM List_Region WHERE Id = " ..
					                          Lobby.activePlayer.garage.homeRegion):toTable()[1].TableName
					local car = DB:execute("SELECT CarId FROM " .. Lobby.activePlayer.DB.Garage .. " WHERE Id = " ..
					                       UI.Flow.GarageId):toTable()[1].CarId
					UI.Flow.SalePrice = math.floor(DB:execute("SELECT RegionPrice FROM " .. prices .. " WHERE Ordinal = " ..
					                                          car):toTable()[1].RegionPrice / 10)
				end)'''


def my_cars(d):
    """A car sells for 10% of its dealership price, the only money in a run (the game's own price is replaced), and
    its strikes go with it."""
    d = once(d, re.escape('UI.Flow.SalePrice = Lobby.activePlayer.garage:CalculateSalePrice( UI.Flow.GarageId )'),
             lambda m: SALE.replace('\n', '\r\n'), 'the sale price')
    return once(d, re.escape('Lobby.activePlayer.garage:SellCar( UI.Flow.GarageId )'),
                lambda m: 'local before = ProfileManager.Main.Credits\r\n\t\t\t\t\t\t' + m.group(0) +
                '\r\n\t\t\t\t\t\tProfileManager.Main.Credits = before + UI.Flow.SalePrice'
                '\r\n\t\t\t\t\t\tloadstring("--roguelike: car gone " .. UI.Flow.GarageId)', 'the sale')


RESTART = r'[ \t]*\{\s*DisplayName = UI\.Strings\.IDS_Restart,\s*MenuAction = "Restart"\s*\},\r?\n'


def circuit_race(d):
    """No Restart after a race (a lost race stays lost), and no Hire Driver on the starting grid. The car reward
    screen's package also loads Buy Upgrades' Install pop-up, for a win's part (CarReveal.xml's PART_WON): a <resource>
    of CarReveal.xml's own left the car reward screen empty."""
    d = once(d, r'(<package name="CarReveal-Scene">\s*<scene path="GAME:\\Media\\UI\\Scenes\\C_Scenes\\C_WIN_CarReveal\.xgf"/>'
                r'\s*<package name="GAME:\\Media\\UI\\Scenes\\C_Scenes\\C_WIN_CarReveal\.xgf"/>)',
             lambda m: m.group(1) + '\r\n            <scene path="GAME:\\Media\\UI\\Scenes\\C_Scenes\\C_UPG_BuyPopUp.xgf"/>'
             '\r\n            <package name="GAME:\\Media\\UI\\Scenes\\C_Scenes\\C_UPG_BuyPopUp.xgf"/>', 'the car reward screen\'s package')
    d = once(d, r'if hiredDriver == 0 then(\s*table\.insert\( source, 4, \{\s*DisplayName = UI\.Strings\.IDS_HireDriver)',
             lambda m: 'if false then -- Roguelike: no hired drivers' + m.group(1), 'the starting grid\'s Hire Driver')
    d = once(d, r'(\s*)else(\s*table\.insert\( source, 4, \{\s*DisplayName = UI\.Strings\.IDS_FireDriver)',
             lambda m: m.group(1) + 'elseif false then' + m.group(2), 'the starting grid\'s Fire Driver')
    return remove(d, RESTART, 'the post-race Restart')


def pause(d):
    """No Restart in the pause menu either."""
    return remove(d, RESTART, 'the pause menu\'s Restart')


def main_menu(d):
    """The main menu's Career is Rogue Mode, and its career box's Level the run's (a profile not on a run: the driver
    level still)."""
    d = once(d, r'(<string id="IDS_Career"\s+text=")[^"]*', lambda m: m.group(1) + 'Rogue Mode', 'the main menu\'s Career')
    d = once(d, r'(Career\.Level\s*= "\[ProfileManager\.Main\.Level\]")',
             lambda m: m.group(1) + '\r\n\t\t\t\tlocal runLevel = loadstring("return 0 --[[roguelike: level]]")()'
             '\r\n\t\t\t\tif runLevel >= 0 then Career.Level = runLevel end', 'the main menu\'s career level')
    return once(d, r'(\n[ \t]*Career = \{\})(\r?\n)', lambda m: m.group(1) + m.group(2) +
                SHORT_NAMES.replace('\n', '\r\n') + m.group(2), 'the main menu\'s career box')


# The short model names the career menus' footer shows, with the car's strikes after them (the port's
# RoguelikeLuaBytecode reads them from this table): made once, before any career menu
SHORT_NAMES = r'''				pcall(function()
					DB:execute("CREATE TABLE IF NOT EXISTS RogueShortNames (Id INTEGER PRIMARY KEY, Name TEXT)")
					local have = DB:execute("SELECT COUNT(1) AS n FROM RogueShortNames"):toTable()
					if not (have and have[1] and have[1].n > 0) then
						for id, name in pairs(loadstring("return {} --[[roguelike: short names]]")()) do
							DB:execute("INSERT INTO RogueShortNames (Id, Name) VALUES (" .. id .. ", '" ..
							           (string.gsub(name, "'", "''")) .. "')")
						end
					end
				end)'''


FLOWS = {'Flows/CareerMenu.xml': career_menu, 'Flows/FirstTimeCareer.xml': first_time_career,
         'Flows/CarReveal.xml': car_reveal, 'Flows/CareerQuickStatus.xml': quick_status,
         'Flows/CareerEventSelect.xml': event_select, 'Flows/CareerMyCars.xml': my_cars, 'Flows/Forza.xml': main_menu,
         'Flows/CareerCircuitRace.xml': circuit_race, 'Flows/CareerPause.xml': pause}


def build(ui_zip, out):
    """Writes the changed flows under `out` (<overlay>/game/media/ui.zip)."""
    z = zipfile.ZipFile(ui_zip)
    for member, change in FLOWS.items():
        text = change(z.read(member).decode('latin1'))
        xml.dom.minidom.parseString(text.encode('latin1'))  # a stray '<' in the Lua would break the flow in game
        path = os.path.join(out, *member.split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(text.encode('latin1'))
    print('roguelike: %d career flows changed' % len(FLOWS))
    # Select Car's car level badge, now a car's strikes (make_icons.py's crosses): its icon from the box's left end
    # (-258) to its middle. The scene's values are a pool of NUL-ended strings the nodes index, so one of the same
    # length goes in its place; the icon's position is the only one that's this.
    scene = 'Scenes/C_Scenes/C_CAR_CarSelect.xgf'
    data, old, new = z.read(scene), b'\0-258 0 -15\0', b'\0-129 0 -15\0'
    if data.count(old) != 1:
        raise SystemExit('roguelike_ui: the car level badge\'s position not found in this UI.zip')
    path = os.path.join(out, *scene.split('/'))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as f:
        f.write(data.replace(old, new))
