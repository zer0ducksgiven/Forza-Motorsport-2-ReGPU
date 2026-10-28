"""Random Events (career mod): builds the mod's folder for make_mod.py.

  python random_events.py <gamedb.slt> <game Media\\StringTables folder> <out folder> [--rotation N] [--seed S]

A "Random Events" career category with four events at rising difficulty (Easy, Medium, Hard, Extreme). Each event
is a random track (and layout), a random theme (an era, region, country, make, drivetrain, aspiration, engine size,
weight limit, or race cars), the tier's class, an AI field drawn from the cars that pass the rules, laps for the
tier's distance and a prize. Rotation N (races completed / 2) picks the seed, so a rotation is always the same set.

Entry rules are EventRestrictions rows; their FieldAttributeID, from the game's own events:
  1 year  3 region (1 Asia, 2 Europe, 3 North America)  4 country  6 make  8 cylinders (List_Cylinders id)
  10 class  11 weight (kg / 100)  12 power  14 drivetrain (1 FWD, 2 RWD, 3 AWD)  15 aspiration (1 natural,
  2 turbo)  18 car type (3 = race car)
The names go into CareerEventTypes.str and Events.str of every language: the mod's overrides zip replaces those two
tables (no official pack touches them).

Out folder: <out>/RandomEvents/media/db/patch/92000_merge.slt and <out>/DLCZips-extra/ (overrides zip,
zipmount.xml, stringtables.xml, RandomEventsPrizes.txt), for make_mod.py <out>/RandomEvents ... --zip RandomEvents.
RandomEventsPrizes.txt lists every car ("<id> <price> <year> <make> <model>"): the port draws the prize car from it.
"""
import argparse, os, random, sqlite3, sys, zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import strtable

CATEGORY_ID = 10           # CareerEventTypes: the game has 1-9
FIRST_EVENT_ID = 900       # Events: the game uses up to 478
FIRST_ROW_ID = 20000       # Races, EventRestrictions, EventParticipants
TEMPLATE_EVENT = 1         # the first Proving Grounds event: the source of the other Events columns
FIELD = dict(year=1, region=3, country=4, make=6, cylinders=8, cls=10, weight=11, drive=14, aspiration=15, cartype=18)
TIERS = [  # name, classes (one is picked), target distance (m), first prize, AI skill (Easy, Medium, Hard)
    ('Easy', [0, 1], 10000, 6000, (20, 15, 3)),
    ('Medium', [2], 15000, 12000, (20, 12, 2)),
    ('Hard', [3], 20000, 25000, (20, 10, 1)),
    ('Extreme', [4, 'race'], 25000, 50000, (20, 6, 1)),
]
SHARE = [1.0, 0.7, 0.5, 0.35, 0.25, 0.15, 0.1, 0.05]  # prize by place
DRIVES = {1: 'FWD', 2: 'RWD', 3: 'AWD'}
REGIONS = {1: 'Asian', 2: 'European', 3: 'North American'}
SKIP_TRACKS = ('DebugTrack', 'TuningTrack')
ICONS = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'icons')  # make_icons.py's .xds files
# The event's picture (Events.EventIcon -> Media/UI/Textures/L_Map/Event_Bkgds/EventBkgd<n>.tga): the game's own
# event closest to the theme.
ICON = dict(classics=101, modern=34, fwd=4, rwd=6, awd=5, turbo=26, free=30, light=11)
REGION_ICON = {1: 2, 2: 1, 3: 3}  # Asian, European, North American Open
COUNTRY_ICON = {'Japan': 82, 'Germany': 78, 'Italy': 48, 'United Kingdom': 76, 'England': 76, 'USA': 42,
                'United States': 42}
MAKE_ICON = {'Acura': 13, 'Volkswagen': 15, 'Toyota': 16, 'Nissan': 18, 'Audi': 19, 'Porsche': 20, 'Chevrolet': 21,
             'Dodge': 22, 'Lamborghini': 23, 'Ferrari': 24, 'Ford': 61, 'Mazda': 62, 'Lotus': 65, 'Shelby': 66,
             'TVR': 69, 'McLaren': 72, 'Honda': 477}
CYLINDER_ICON = {'4': 25, '5': 31, '6': 27, '8': 29}


def ref(prefix, key):
    return '_&%d' % (prefix << 16 | key)


def load_cars(g):
    """Every car with the attributes the rules test."""
    q = '''SELECT c.Id, c.Year, c.MakeID, c.ClassID, c.CarTypeID, c.CurbWeight, m.CountryID, n.RegionID,
                  e.DriveTypeID, e.AspirationID_Stock, e.CylinderID
           FROM Data_Car c JOIN List_CarMake m ON m.ID = c.MakeID JOIN List_Country n ON n.CountryID = m.CountryID
           JOIN List_UpgradeEngine u ON u.Ordinal = c.Id AND u.IsStock = 1 JOIN Data_Engine e ON e.EngineID = u.EngineID'''
    keys = 'id year make cls cartype weight country region drive aspiration cylinders'.split()
    return [dict(zip(keys, r)) for r in g.execute(q)]


def themes(rng, cars, names):
    """Candidate (title, rules, test, picture) for this rotation: rules are (field, func, value)."""
    out = []
    y = rng.choice([1975, 1985, 1990, 1995, 2000])
    out.append(('Pre-%d Classics' % y, [(FIELD['year'], '<', str(y))], lambda c, y=y: c['year'] < y, ICON['classics']))
    y = rng.choice([1995, 2000, 2003])
    out.append(('Post-%d Machines' % y, [(FIELD['year'], '>', str(y))], lambda c, y=y: c['year'] > y, ICON['modern']))
    r = rng.choice(list(REGIONS))
    out.append(('%s Open' % REGIONS[r], [(FIELD['region'], '=', str(r))], lambda c, r=r: c['region'] == r, REGION_ICON[r]))
    region_of = {c['country']: c['region'] for c in cars}
    co = rng.choice(sorted({c['country'] for c in cars}))
    cname = names['country'].get(co) or 'National'
    out.append(('%s Showdown' % cname, [(FIELD['country'], '=', str(co))], lambda c, co=co: c['country'] == co,
                COUNTRY_ICON.get(cname) or REGION_ICON.get(region_of[co], 1)))
    mk = rng.choice(sorted({c['make'] for c in cars}))
    mname = names['make'].get(mk) or 'One-Make'
    mregion = next(c['region'] for c in cars if c['make'] == mk)
    out.append(('%s Cup' % mname, [(FIELD['make'], '=', str(mk))], lambda c, mk=mk: c['make'] == mk,
                MAKE_ICON.get(mname) or REGION_ICON.get(mregion, 1)))
    d = rng.choice(list(DRIVES))
    out.append(('%s Shootout' % DRIVES[d], [(FIELD['drive'], '=', str(d))], lambda c, d=d: c['drive'] == d, ICON[DRIVES[d].lower()]))
    a = rng.choice([1, 2])
    out.append(('Turbo Trophy' if a == 2 else 'Free-Breathing Trophy', [(FIELD['aspiration'], '=', str(a))], lambda c, a=a: c['aspiration'] == a, ICON['turbo' if a == 2 else 'free']))
    cyl = rng.choice(sorted({c['cylinders'] for c in cars if c['cylinders']}))
    cyl_name = names['cylinders'].get(cyl, 'Engine')
    out.append(('%s Challenge' % cyl_name, [(FIELD['cylinders'], '=', str(cyl))], lambda c, cyl=cyl: c['cylinders'] == cyl,
                CYLINDER_ICON.get(cyl_name.split('-')[0], ICON['modern'])))
    w = rng.choice([11.0, 12.5, 14.0])
    out.append(('Lightweight Invitational', [(FIELD['weight'], '<', '%g' % w)], lambda c, w=w: c['weight'] < w, ICON['light']))
    return out


def generate(g, rotation, seed, names):
    rng = random.Random(seed * 1000003 + rotation)
    cars = load_cars(g)
    tracks = [r for r in g.execute('SELECT id, DisplayName, MediaName, Length FROM Tracks') if r[2] not in SKIP_TRACKS]
    ai_players = [r[0] for r in g.execute('SELECT Id FROM AIPlayers')]
    temperaments = [r[0] for r in g.execute('SELECT id FROM AITemperaments')]
    events = []
    for tname, classes, distance, prize, skill in TIERS:
        for _ in range(500):
            cls = rng.choice(classes)
            if cls == 'race':
                base = [(FIELD['cartype'], '=', '3')]; ok = lambda c: c['cartype'] == 3; cls_name = 'Race cars'
            else:
                base = [(FIELD['cls'], '=', str(cls)), (FIELD['cartype'], '<>', '3')]
                ok = lambda c, cls=cls: c['cls'] == cls and c['cartype'] != 3; cls_name = 'Class ' + 'DCBAS'[cls]
            title, rules, test, icon = rng.choice(themes(rng, cars, names))
            eligible = [c for c in cars if ok(c) and test(c)]
            if len(eligible) >= 10: break
        else:
            raise SystemExit('no theme with 10 eligible cars for ' + tname)
        circuit = rng.choice(sorted({t[2] for t in tracks}))  # a circuit, then one of its layouts (the Test Track has 18)
        track = rng.choice([t for t in tracks if t[2] == circuit])
        laps = max(1, round(distance / track[3]))
        field = rng.sample(eligible, 7)
        drivers = rng.sample(ai_players, 7)
        events.append(dict(tier=tname, title='%s: %s' % (tname, title), cls_name=cls_name, rules=base + rules, icon=icon,
                           track=track[0], track_name=names['track'].get(track[0]) or track[2], laps=laps, prize=prize,
                           eligible=len(eligible), skill=skill,
                           field=[(c['id'], d, rng.choice(temperaments)) for c, d in zip(field, drivers)]))
    return events


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gamedb'); ap.add_argument('stringtables'); ap.add_argument('out')
    ap.add_argument('--rotation', type=int, default=0); ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--laps', type=int, help='every event this many laps (for testing)')
    ap.add_argument('--install', help="write the finished files into this installed pack's Media/DLCZips (the launcher)")
    a = ap.parse_args()
    g = sqlite3.connect(a.gamedb)
    mod = os.path.join(a.out, 'RandomEvents'); extra = os.path.join(a.out, 'DLCZips-extra')
    os.makedirs(os.path.join(mod, 'media', 'db', 'patch'), exist_ok=True); os.makedirs(extra, exist_ok=True)

    langs = {}
    for f in sorted(os.listdir(a.stringtables)):
        if f.lower().endswith('.zip'):
            z = zipfile.ZipFile(os.path.join(a.stringtables, f))
            names = {n.lower(): n for n in z.namelist()}
            langs[f[:-4]] = {t: strtable.Table(z.read(names[t.lower() + '.str']))
                             for t in ('CareerEventTypes', 'Events', 'Tracks', 'List_CarMake', 'List_Country', 'Data_Car')
                             if t.lower() + '.str' in names}
    en = langs['EN']
    def look(table, sql):  # id -> English text of a string reference column
        return {i: en[table].strings.get(int(r[2:]) & 0xFFFF, '') for i, r in g.execute(sql) if r and r.startswith('_&')}
    names = dict(track=look('Tracks', 'SELECT id, DisplayName FROM Tracks'),
                 make=look('List_CarMake', 'SELECT ID, DisplayName FROM List_CarMake'),
                 country=look('List_Country', 'SELECT CountryID, Country FROM List_Country'),
                 cylinders={r[0]: '%s-Cylinder' % r[1] for r in g.execute('SELECT * FROM List_Cylinders')})
    events = generate(g, a.rotation, a.seed, names)
    if a.laps:
        for e in events: e['laps'] = a.laps

    cat_prefix = int(g.execute('SELECT Name FROM CareerEventTypes WHERE id=1').fetchone()[0][2:]) >> 16
    ev_prefix = int(g.execute('SELECT Name FROM Events WHERE Id=?', (TEMPLATE_EVENT,)).fetchone()[0][2:]) >> 16
    key = en['CareerEventTypes'].free_key(0x0100)
    cat_keys = (key, key + 1)
    texts = [('CareerEventTypes', key, 'IDS_Name_RandomEvents', 'Random Events'),
             ('CareerEventTypes', key + 1, 'IDS_Description_RandomEvents',
              'Four new events every two races: random tracks, random cars, random rules. Every five races wins you a random car.')]
    k = en['Events'].free_key(0x0100)
    for i, e in enumerate(events):
        while k in en['Events'].strings or k + 1 in en['Events'].strings: k += 1
        e['keys'] = (k, k + 1)
        texts.append(('Events', k, 'IDS_Name_Random%d' % i, e['title']))  # the track goes in the description: the title bar fits ~40 characters
        texts.append(('Events', k + 1, 'IDS_Description_Random%d' % i,
                      '%s. %s, %d lap%s. %d eligible cars.' % (
                          e['track_name'], e['cls_name'], e['laps'], 's' if e['laps'] > 1 else '', e['eligible'])))
        k += 2
    zpath = os.path.join(extra, 'RandomEventsOverrides1_pri_170.zip')
    with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
        for lang, tables in langs.items():
            for table, kk, sym, text in texts:
                tables[table].add(kk, text, sym)
            for t in ('CareerEventTypes', 'Events'):
                z.writestr('%s/%s.str' % (lang, t), tables[t].build())
    # the prize list: every car, for the port to draw from every fifth Random Events race (hooks/random_events.cpp)
    with open(os.path.join(extra, 'RandomEventsPrizes.txt'), 'w', encoding='utf-8', newline='\n') as f:
        model = look('Data_Car', 'SELECT Id, DisplayName FROM Data_Car')
        for cid, year, make, cost in g.execute('SELECT Id, Year, MakeID, BaseCost FROM Data_Car ORDER BY Id'):
            f.write('%d %d %d %s\n' % (cid, cost, year, model.get(cid) or names['make'].get(make, '')))
    # the category's pictures (career/icons, drawn by make_icons.py), added to the game's UI textures
    with zipfile.ZipFile(os.path.join(extra, 'RandomEventsOverrides2_pri_170.zip'), 'w', zipfile.ZIP_DEFLATED) as z:
        for f in sorted(os.listdir(ICONS)):
            if f.endswith('.xds'):
                z.write(os.path.join(ICONS, f), 'C_Map/C_TYP_Map/' + f)
    open(os.path.join(extra, 'zipmount.xml'), 'w', newline='\r\n').write(
        '<?xml version="1.0" ?>\n<zipmount>\n\t<zip Name="RandomEventsOverrides1_pri_170.zip" Mount="game:\\Media\\StringTables\\" />\n'
        '\t<zip Name="RandomEventsOverrides2_pri_170.zip" Mount="game:\\Media\\UI\\Textures\\" />\n</zipmount>\n')
    open(os.path.join(extra, 'stringtables.xml'), 'w', newline='\r\n').write(
        '<?xml version="1.0" ?>\n<stringtables >\n\t<table Name="careereventtypes" />\n\t<table Name="events" />\n</stringtables>\n')

    path = os.path.join(mod, 'media', 'db', 'patch', '92000_merge.slt')
    if os.path.exists(path): os.remove(path)
    p = sqlite3.connect(path)
    def table(name):
        p.execute(g.execute("SELECT sql FROM sqlite_master WHERE type='table' AND name=?", (name,)).fetchone()[0])
        return [r[1] for r in g.execute('PRAGMA table_info([%s])' % name)]
    def insert(name, cols, row):
        p.execute('INSERT INTO [%s] VALUES (%s)' % (name, ','.join('?' * len(cols))), [row.get(c) for c in cols])

    cols = table('CareerEventTypes')
    row = dict(zip(cols, g.execute('SELECT * FROM CareerEventTypes WHERE id=1').fetchone()))
    row.update(id=CATEGORY_ID, Name=ref(cat_prefix, cat_keys[0]), Description=ref(cat_prefix, cat_keys[1]), Level=0,
               IconPath='GAME:\\Media\\UI\\Textures\\C_Map\\C_TYP_Map\\C_TYP_Icon_RandomEvents.tga',
               BadgePath='GAME:\\Media\\UI\\Textures\\C_Map\\C_TYP_Map\\C_TYP_Badge_RandomEvents.tga')
    insert('CareerEventTypes', cols, row)
    ecols = table('Events'); rcols = table('Races'); xcols = table('EventRestrictions'); pcols = table('EventParticipants'); scols = table('EventScoring')
    template = dict(zip(ecols, g.execute('SELECT * FROM Events WHERE Id=?', (TEMPLATE_EVENT,)).fetchone()))
    race_t = dict(zip(rcols, g.execute('SELECT * FROM Races WHERE EventId=? AND RaceNumber=1', (TEMPLATE_EVENT,)).fetchone()))
    rid = FIRST_ROW_ID
    for i, e in enumerate(events):
        eid = FIRST_EVENT_ID + i
        ev = dict(template); ev.update(Id=eid, Name=ref(ev_prefix, e['keys'][0]), Description=ref(ev_prefix, e['keys'][1]),
                                       CareerTypeId=CATEGORY_ID, Level=0, NumberOfRaces=1, EventOrder=i + 1, IsDLC=0,
                                       NumberOfDrivers=8, CashPrize=e['prize'], EventIcon=e['icon'])
        insert('Events', ecols, ev)
        race = dict(race_t); race.update(Id=rid, EventId=eid, RaceNumber=1, TrackId=e['track'], NumLaps=e['laps']); rid += 1
        insert('Races', rcols, race)
        for j, (f, func, v) in enumerate(e['rules']):
            insert('EventRestrictions', xcols, dict(Id=rid, Event_ID=eid, FieldAttributeID=f, Func=func, Value=v,
                                                     AndOrIndicator='And' if j < len(e['rules']) - 1 else '')); rid += 1
        easy, med, hard = e['skill']
        for car, driver, temper in e['field']:
            insert('EventParticipants', pcols, dict(ID=rid, Ordinal=car, ColorSequenceId=1, EventID=eid, AIPlayers_PlayerId=driver,
                                                    Easy_AISkill_Id=easy, Easy_AITemperament_id=temper, Medium_AISkill_Id=med,
                                                    Medium_AITemperament_id=temper, Hard_AISkill_Id=hard, Hard_AITemperament_id=temper)); rid += 1
        for place, share in enumerate(SHARE, 1):
            insert('EventScoring', scols, dict(Event_ID=eid, place=place, Credits=int(e['prize'] * share)))
        print('%d %-8s %-60s %d laps, %d eligible, rules %s' % (eid, e['tier'], ('%s (%d) at %s' % (e['title'], e['icon'], e['track_name']))[:60], e['laps'], e['eligible'], e['rules']))
    p.commit()
    p.close()
    if a.install:
        install(mod, extra, a.install, a.rotation)


def install(mod, extra, dlczips, rotation):
    """An installed pack (the launcher keeps one, rebuilt at launch when the rotation changes): the mod's zip and the
    extra files written into its Media/DLCZips, and RandomEventsRotation.txt saying which rotation it holds. The
    game unpacks the database patch again when the zip changes (imports_xam.cpp UnpackDlcPatches)."""
    os.makedirs(dlczips, exist_ok=True)
    tmp = os.path.join(dlczips, 'RandomEvents_pri_73.zip.tmp')
    with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as z:
        for root, dirs, files in os.walk(mod):
            dirs.sort()
            for f in sorted(files):
                full = os.path.join(root, f)
                z.write(full, os.path.relpath(full, mod).replace(os.sep, '/'))
    os.replace(tmp, os.path.join(dlczips, 'RandomEvents_pri_73.zip'))
    for f in os.listdir(extra):
        with open(os.path.join(extra, f), 'rb') as i, open(os.path.join(dlczips, f), 'wb') as o:
            o.write(i.read())
    with open(os.path.join(dlczips, 'RandomEventsRotation.txt'), 'w') as f:
        f.write('%d\n' % rotation)
    print('installed rotation %d in %s' % (rotation, dlczips))


if __name__ == '__main__':
    main()
