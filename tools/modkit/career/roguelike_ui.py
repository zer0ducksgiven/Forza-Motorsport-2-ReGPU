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
  return 0 --[[roguelike: next car event=<id> place=<n> class=<c>]]   the race's car (0: none due)
"""
import os, re, zipfile

RUN = 'false --[[roguelike: run]]'
REGION = '2 --[[roguelike: region]]'
CAR = '0 --[[roguelike: car]]'
FULL = 'false --[[roguelike: full]]'
BLOCKED_TITLE = 'Roguelike Career'
BLOCKED_TEXT = ("This profile already has a career. Roguelike Career needs a new profile: make one in the "
                "launcher's Profiles tab, or switch Roguelike Career off in the launcher's Mods panel.")


def once(text, pattern, repl, what):
    out, n = re.subn(pattern, repl, text, count=1, flags=re.S)
    if n != 1:
        raise SystemExit('roguelike_ui: %s not found in this UI.zip' % what)
    return out


def no_buy_car(d, pattern, what):
    """Buy Car's entries out of a menu (the run's cars only come from wins)."""
    out, n = re.subn(pattern, '', d)
    if not n:
        raise SystemExit('roguelike_ui: %s not found in this UI.zip' % what)
    return out


def career_menu(d):
    """A career not started as a Roguelike run is turned away (the run needs a fresh profile); no Buy Car."""
    d = no_buy_car(d, r'[ \t]*\{Text="\[UI\.Strings\.IDS_BUY_CAR\]"[^\n]*\n', 'the career menu\'s Buy Car')
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
			-- after a race: the run's next car (the port draws it, one per race), bought with credits lent for it
			local cls = 0
			pcall(function()
				cls = DB:execute("SELECT ClassID FROM " .. Lobby.activePlayer.DB.Garage .. " WHERE Id = " ..
				                 ProfileManager.Main.CareerCar):toTable()[1].ClassID
			end)
			local car = loadstring("return 0 --[[roguelike: next car event=" .. tostring(RewardReveal.EventId) ..
			                       " place=" .. tostring(RewardReveal.OverallFinish) .. " class=" .. tostring(cls) .. "]]")()
			if car and car > 0 then
				local p = ProfileManager.Main
				local c = p.Credits
				p.Credits = c + 10000000
				pcall(function() Lobby.activePlayer.garage:BuyCar(car, 1) end)
				p.Credits = c
				-- saved now: the race's own save came before these screens
				pcall(function() ProfileManager.activePlayerProfile:Save() end)
				loadstring("--roguelike: got " .. car)
				loadstring("--roguelike: reveal " .. car)
			end
		end
'''


def car_reveal(d):
    """After every race of a run, the next car is bought and shown with the car reward screen (the port's
    RewardReveal hooks make the screen's own check and car come from the reveal note)."""
    return once(d, r'(<logic>)(\s*if RewardReveal:CheckForCarUnlocks\(\) then)', lambda m: m.group(1) +
                NEXT_CAR.replace('\n', '\r\n') % RUN + m.group(2), 'the car reward check')


def quick_status(d):
    """No Buy New Car in the post-race menu."""
    return no_buy_car(d, r'[ \t]*\{ Label="\[UI\.Strings\.IDS_Choice_BuyCar\]",\s*Event="buycar"\s*\},\r?\n', 'the quick status\'s Buy Car')


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
    """A car sells for 10% of its dealership price, the only money in a run (the game's own price is replaced)."""
    d = once(d, re.escape('UI.Flow.SalePrice = Lobby.activePlayer.garage:CalculateSalePrice( UI.Flow.GarageId )'),
             lambda m: SALE.replace('\n', '\r\n'), 'the sale price')
    return once(d, re.escape('Lobby.activePlayer.garage:SellCar( UI.Flow.GarageId )'),
                lambda m: 'local before = ProfileManager.Main.Credits\r\n\t\t\t\t\t\t' + m.group(0) +
                '\r\n\t\t\t\t\t\tProfileManager.Main.Credits = before + UI.Flow.SalePrice', 'the sale')


FLOWS = {'Flows/CareerMenu.xml': career_menu, 'Flows/FirstTimeCareer.xml': first_time_career,
         'Flows/CarReveal.xml': car_reveal, 'Flows/CareerQuickStatus.xml': quick_status,
         'Flows/CareerEventSelect.xml': event_select, 'Flows/CareerMyCars.xml': my_cars}


def build(ui_zip, out):
    """Writes the changed flows under `out` (<overlay>/game/media/ui.zip)."""
    z = zipfile.ZipFile(ui_zip)
    for member, change in FLOWS.items():
        text = change(z.read(member).decode('latin1'))
        path = os.path.join(out, *member.split('/'))
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(text.encode('latin1'))
    print('roguelike: %d career flows changed' % len(FLOWS))
