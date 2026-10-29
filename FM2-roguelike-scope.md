# Roguelike Career: scope

An optional mod that turns the career into a randomizer run, built on Random Events. Working name **Roguelike
Career**.

## The run, as the player sees it

1. **Fresh profile only.** If the profile already has a career, entering Career shows a warning ("start this on a
   new profile") and goes back. No way in.
2. **No setup screens.** Opening Career on a new profile skips region, starter car, difficulty and the confirmation.
   It plays the game's **car reward screen** (the gift animation) for one random **Class D** car, puts you in it,
   and goes straight to the career main menu. Difficulty starts at the game's default, Intermediate; **Set
   Difficulty** in the career menu changes it, as in any career.
3. **Go Race has one category:** Random Events. Its four events are built **from your garage**: each one is in the
   class of a car you own, with entry rules that car passes (its make, country, drivetrain, engine and so on), on a
   random track. You can always enter at least one, and the newest car always has an event it can enter.
4. **A new car for every win.** Win a race (first place) and the next car is drawn, shown with the **car reward
   screen** straight after the race, and added to your garage. Any other place earns nothing. Its class is weighted
   around the car you raced (60% same, 20% up, 20% down). Winning an Extreme event has a 1 in 20 chance of a race car
   instead. **Every car you own stays usable**: race whichever one fits an event. The garage you build up is the
   run.
5. **No prize money.** Races pay nothing. The only way to get credits is to sell cars, and a car sells for **10% of
   its dealership price**. **Buy Car** is gone from the career menu.

## What the research found

Everything below was checked against the game's own files (`Media/UI.zip` flows, `Media/DB/gamedb.slt`).

### The career start is flow Lua we can change
- `Flows/CareerMenu.xml` decides: `if Lobby.activePlayer.garage.numCars == 0 then` → `FirstTimeCareer.xml`.
  This is where the "fresh profile" check goes.
- `Flows/FirstTimeCareer.xml` is three screens: region (`C_BUY_RegionWarning`), starter car (`C_CAR_CarSelect`,
  the 7 region cars in `Rewards_DriverLevel` with `TypeId=2`), and difficulty. It then sets 11,000 CR, runs
  `garage:BuyCar(car, colour)` and `garage:GetInCar(id)`, and saves.
- The region still has to be set (`garage:SetHomeRegion`): it picks the price list. The mod picks one at random
  without asking.

### Events can be matched to the garage
- Entry rules are `EventRestrictions` rows. `RestrictionFields` lists what a rule can test. Two matter here:
  - **9 / 10 = `Garage.PerformanceIndex` / `Garage.ClassID`**: the car as it sits in the garage, upgrades
    included. So a car upgraded into a higher class enters that class's events.
  - **13 = `Data_Car.Id`**: one specific car model (the game uses it 12 times). It's not needed, since every
    owned car may race, but it's there if an event should ever be for one car.
- So each event takes a class and a theme from a car the player owns, and any owned car that passes can enter.
- Event Select works out qualifying cars live (`garage:GetQualifiedCars(eventId)`), so rules changed just before
  the screen opens take effect. Random Events already changes prizes this way.

### Cars and prices
- 63 Class D road cars (`ClassID 0`, not race cars).
- The **dealership price** is `RarityPrice_<region>.RegionPrice`, one table per home region (`List_Region.TableName`).
  It is not `Data_Car.BaseCost`. In Europe:

  | Class | Cars | Dealer price | 10% sale price |
  |---|---|---|---|
  | D | 63 | 9,000 – 73,000 | 900 – 7,300 |
  | C | 57 | 13,500 – 127,500 | 1,350 – 12,750 |
  | B | 49 | 31,500 – 155,000 | 3,150 – 15,500 |
  | A | 38 | 110,000 – 222,500 | 11,000 – 22,250 |
  | S | 29 | 230,000 – 250,000 | 23,000 – 25,000 |
  | Race | 74 | 275,000 – 350,000 | 27,500 – 35,000 |

### Selling is done in the flow
- `Flows/CareerMyCars.xml`: the price comes from `garage:CalculateSalePrice(id)` and the sale from
  `garage:SellCar(id)`. A car flagged as a reward (`Garage.Flags & 2`) gets a lower price and its own warning.
- So the 10% rule can live in the same flow:
  1. Show `floor(RegionPrice × 0.1)`.
  2. After `SellCar`, set the credits to what they were before the sale plus that amount.

  This gets round the low price the game gives reward cars without touching native code. The game already stops
  you selling the car you're in.

### No prize money
- Race pay is `EventScoring.Credits` and the event bonus is `Events.CashPrize`. Setting both to 0 for the mod's
  events means nothing is paid.
- The winnings screen skips itself when nothing was won (`RaceWinnings.xml`: `GetPlayerWinnings().IsValid`).
- **Side effect to confirm:** driver XP in FM2 appears to come from winnings (`LevelProgress.xml`), so the driver
  level would stay put. That also shuts off a loophole: level-ups give free cars (`Rewards_DriverLevel`
  `TypeId 1/3`). I think that's what we want, but it needs checking.

### Buy Car is one menu entry
- The career menu is a Lua table (`MYC_Menu` in `CareerMenu.xml`); Buy Car is `Event="BuyCar"`. The mod drops that
  entry.
- The post-race quick menu (`CareerQuickStatus.xml`) offers Buy Car too; it goes as well.
- **Risk:** the menu's video wall (`C_MYC_VideoWall.lua`) may count positions rather than events. That needs a
  check once the entry is removed.

## How to build it

It reuses what Random Events already has:
- the port's Lua rewriting (`hooks/lua.cpp`: `luaL_loadstring` and buffer loads go through
  `RandomEventsLuaSource`);
- Lua run before a script (the prize car's `BuyCar`);
- the race-finished hook (`TU 82348078`);
- per-profile state in `profiles\<name>\*.ini`;
- the launcher building the mod from the player's game files.

**Who does what**
- **Port (C++):**
  - Random draws, with a real random source rather than the game's Lua.
  - Run state per profile in `roguelike.ini`: run started, races, the next car.
  - Builds the four events from the garage.
  - Puts values into the flow Lua as it loads.
- **Flow rewrites:** text changes to the Lua chunks above, done the way Random Events changes its queries today.
- **Mod package (launcher-built):** the category, four event slots with their `Races` / `EventRestrictions` /
  `EventParticipants` rows, strings and icon. All prize rows are 0.

**Events built from the garage.** The current generator builds events before launch, but the garage changes after
every race while the game is running. So the port builds them in-game instead:
1. Event Select's Lua lists the garage (car, class as upgraded) and hands it to the port through the same note trick
   ScaledPrizes uses for level and car count.
2. The port reads each car's attributes from a car list the generator writes, as it does for the prize list today.
3. It picks four cars to build on: the newest car always, the rest spread across the classes you own. For each, it
   picks a theme that car passes, a track and laps, and an AI field from cars of that class and theme.
4. It sends that to Event Select as Lua `UPDATE`s on the four events' rows.
5. Event names and descriptions go in as plain text through the query rewrite, which already works.

Tiers become **difficulty of the AI field and length**; the class comes from the car each event is built on.

**Skipping the two start screens.** A `<scene>` in a flow always shows, and Lua can't remove it, so the flow file
itself has to change. Tested (spike, 2026-10-28):
- **A DLC-style zip mounted over `Media\UI` does not work.** The game reads `UI.zip` whole at boot (1.2 s), and
  DLC packs only mount at sign-in (about 30 s), so the flows are already loaded.
- **The port's loose-file mod layer works.** `Documents\fm2\mods\<name>\game\Media\UI.zip` (the layer Forza Plus 2
  uses) is merged at boot: the log showed "1 members changed". With `FirstTimeCareer.xml`'s region screen cut,
  Career went straight to Europe's starter cars, and the confirmation said "specify Europe as your home region".
- So the mod is two parts: a **DLC package** (events, strings, icon) and a **`mods\` folder** holding a patched
  `UI.zip`. The launcher builds the patched copy from the player's own `UI.zip`.
  - To check: the overlay's "(0 mod files)" suggests loose member files (`mods\<name>\game\Media\UI\Flows\…`) may
    also be merged. That would mean shipping only the changed flows.
  - The `mods\` folder is read by every game launch, so the launcher must switch it with the mod.

**The car reward screen.** This is `Flows/CarReveal.xml` and its `C_WIN_CarReveal` scene, which the game runs after
a race when `RewardReveal:CheckForCarUnlocks()` says a car was won.
- The scene's script doesn't choose the car. It asks the native `RewardReveal` object (`GetCurrentCarUnlock`,
  `NextCar`, `UnlockCar`) and looks the car up in `StockGarage`.
- Normally that object only holds cars the race unlocked: an event's prize car for first place, or level-up cars.
  So the game won't show our car on its own, and not after every race.
- Inside, `RewardReveal` keeps the cars to show as a vector of car IDs (`this+12` .. `+16`):
  - `CheckForCarUnlocks` (TU `sub_822A54E0`) is "not empty";
  - `GetCurrentCarUnlock` (TU `sub_822A5510`) returns the first ID and erases it.

  These were found from the Lua bindings' startup registrations (`sub_82831960` / `sub_82831980`) and their glue
  (`sub_8232A808` / `sub_8232A8A0`).
- **How it works (tested, spike 0b):** the port hooks those two methods. While a roguelike car is waiting, the
  first returns true and the second returns that car once. Otherwise both run as normal, so the game's own unlocks
  still work.
- A Lua stand-in for `RewardReveal` does **not** work: the scene's script runs in its own environment and never sees
  the flow's globals, and the screen came up empty.
- The car itself is added with `BuyCar`, as the prize car already is. The reveal only presents it.

**The first car.** The patched `FirstTimeCareer.xml` keeps only the career-blocked checks, the home region and
difficulty settings (set, not asked), and the purchase. It loses the region, starter-car and difficulty screens and
the confirmation popup.
- The port picks the car and the region and passes them into the flow's Lua.
- After `BuyCar` / `GetInCar`, the flow runs `CarReveal.xml`. The reveal scene is added to the flow's package, as
  `CareerCircuitRace.xml`'s `CarReveal-Scene` package does.
- Then the save, and the career main menu.
- Difficulty is set from the game's own Intermediate defaults: the flow's `Difficulty == 1` block (AI, assists,
  damage).

**The new car after a win.**
1. The race-finished hook counts the race in `roguelike.ini`.
2. `CarReveal.xml` runs at the end of every career race (`CareerCircuitRace.xml`). Its first check is plain flow
   Lua, and the patched copy asks the port for the race's car, passing the event, the place and the class raced.
   - For a win, the port draws the car and keeps it as `pending` until it's given, so a reload gets the same car.
   - For any other place, the race is marked done with no car.
3. The flow buys the car (lending credits for the purchase and putting them back) and tells the port which car to
   show. The game's own check then fires the reveal through the hooks.
4. The player stays in the car they were in and switches in My Cars.

If the game is quit between the race and the reveal, the car is still waiting in `roguelike.ini`. The career main
menu gives it then, with a popup, so it's never lost.

**What makes a harder event harder** (built 2026-10-29). Each event is built around one of your cars, and the
tiers differ in three ways:

| Tier | AI skill on Intermediate (`AISkills`: 1 best .. 20) | Aggression (`AITemperaments`: 1 most .. 10) | Opponents (the theme's cars by stock PI) | Engine torque |
|---|---|---|---|---|
| Easy | 16 | 8 | the 7 slowest | stock |
| Medium | 11 | 6 | the 7 nearest your car's PI (as upgraded) | stock |
| Hard | 6 | 4 | the 7 from your car's PI up | +5% |
| Extreme | 1 | 2 | the 7 fastest | +10% |

- **Difficulty settings:** Novice uses skill 4 levels easier and Hardcore 4 levels harder (the game's Hard
  difficulty uses the same AI column as Intermediate).
- **Torque:** the generator adds the game's skill rows again as `AISkills` 101–120 (`NominalTorqueScale` ×1.05)
  and 201–220 (×1.10).
- **Not found:** the game has no system that upgrades AI cars. `EventParticipants` only names a stock car, and the
  PI-range names in the code belong to online tournaments. Fitting parts to AI cars would mean reverse-engineering
  how the game builds them for a race.
- **Checked in game:** around a Lexus IS300 (D 328), Easy fielded D 228–333 and Extreme D 364–399. How skill and
  torque feel in a race isn't tested yet.

**One run mod at a time.** Roguelike Career and Random Events would fight over the same category. Either give
Roguelike its own IDs (category 11, events 910–913), or have the launcher switch one off when the other goes on.
I suggest both.

## Phases

0. **Spike: done 2026-10-28, all passed.** Run on this PC with a temporary hook (`FM2_SPIKE_LUA`: Lua from a file
   run at Event Select, notes logged; kept as a patch, not in the source) and the RogueSpike profile:
   - **Changing the start:** a patched `UI.zip` in `Documents\fm2\mods\<name>` cut the region screen. A DLC zip
     mounted over `Media\UI` does not, because it mounts too late.
   - **Reading the garage:** Event Select's Lua listed it with each car's class, PI and make, plus the current car.
   - **Entry rules:** rewriting `EventRestrictions` made the Golf eligible ("Class D And make VW"), and a Class C
     event stayed closed. Every rule but the last needs `AndOrIndicator = 'And'`, or the game's query has no joiner
     and nothing qualifies.
   - **Track and laps:** rewriting `Races` moved the race to Laguna Seca, 1 lap, and it ran there.
   - **AI field:** rewriting `EventParticipants.Ordinal` gave an all-Class-D grid. Without it, the old event's
     Class C cars raced a Class D Golf.
   - **Zero pay:** the prize box showed 0 CR. The finished race went straight back to the career menu with credits
     unchanged (2,000), no errors, and driver level still 0, which confirms no XP without winnings.
   - Ordering note: the prize scaling's Lua runs at the same point. Roguelike's own Lua must come after it, or
     replace it.
0b. **Spike: the car reward screen: done 2026-10-28, passed.** Port hooks on `RewardReveal` (above), patch kept
   in the scratchpad, not in the source.
   - **Career start (new profile):** after the starter car was bought, the reveal showed the 1992 Golf GTi with its
     D 204 badge, VW logo and stats, then the career menu.
   - **After a race:** the race finished, the flow bought car 247, and the hooks gave it to the reveal
     (`GetCurrentCarUnlock -> 247`). The reveal flow ran to its end.
1. **Mod shell and fresh-profile check.**
   - Launcher-built package; mutual exclusion with Random Events.
   - The `CareerMenu.xml` check: any career not started under the mod (no `roguelike.ini`) gets the popup and goes
     back.
2. **The start.** Opening Career on a new profile plays the car reward screen for a random Class D road car (any
   of the 63), then goes to the career main menu. There are no region, car, difficulty or confirmation screens. The
   region is random, difficulty is Intermediate, and credits start at 0.
3. **Events from the garage.** In-game event builder, Go Race showing only this category, prize rows at 0.
4. **A car per win.** Built and tested 2026-10-28. A 1-lap test race (`FM2_ROGUELIKE_TEST=1`: every event is one lap
   of the shortest layout) drew a Class D car (2006 Astra VXR), gave it once and showed it on the reward screen. The
   events were then rebuilt with it.
   - That test finished 4th, before the rule was "first place only"; the rule is in since.
   - Still to do: the main menu as a fallback if the game was quit before the reveal.
5. **Economy.** 10% sales, Buy Car removed from both menus, no winnings screen.
6. **Polish.** Run counter in the Race Type description (races survived, cars driven). Tidy the text and test the
   whole run.

## Decisions (settled 2026-10-28)

1. **Next car: weighted random.** The class comes from the car you just raced, at its class as upgraded:
   - 60% the same class, 20% one class up, 20% one class down;
   - never below D or above S;
   - only a win earns a car (any other place, none), and winning doesn't change the odds.
2. **Race cars: a rare drop.** Winning an **Extreme** event has a **1 in 20** chance that the gift is a race car
   (one of the 74) instead. There's no other way to get one.
3. **Upgrades: kept.** Buy Upgrades stays, and it's where sale money goes. Upgrades last, because cars stay, and an
   upgrade can lift a car into a higher class's events.
4. **The run is endless.** The Race Type description shows the run so far: races, wins and the best car you've
   had.
5. **Full garage: racing is blocked until you sell.** When the garage is full and a car is due, Go Race shows "sell
   a car first" and won't start an event. No money comes from anywhere but selling. The game's own garage-full path,
   which pays the car's price, is not used.
   - To find: the garage's size limit, and where the game checks it (`IDS_Garage_Full_*`, `CarReveal.xml`'s
     `CarCredits`).
6. **Driver level: no XP, stays at 0.** Level-ups give free cars and part discounts, which would bypass the run. The
   run counter is the progress instead.

## Out of scope

- Arcade, online and the auction house. The mod only changes the career.
- Turning an existing career into a run. The fresh-profile check prevents that by design.
