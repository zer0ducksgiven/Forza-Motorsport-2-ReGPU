"use strict"
// The wheel Bindings window (BindingsForm.cs): live input from SDL, and wheel_bindings.ini for the game.

const $ = (id) => document.getElementById(id)
let nextId = 1
const pending = new Map()

// Pad inputs the game reads, in the order shown. role: what it does in the game, where that is certain.
const TARGETS = [
  { group: "Driving" },
  { key: "steer", name: "Steering", role: "Left stick, left and right", axisOnly: true },
  { key: "steerleft", name: "Steer left", role: "A key or button (keyboard default A)" },
  { key: "steerright", name: "Steer right", role: "A key or button (keyboard default D)" },
  { key: "throttle", name: "Throttle", role: "Right trigger" },
  { key: "brake", name: "Brake", role: "Left trigger" },
  { group: "Buttons" },
  { key: "a", name: "A", role: "Select in menus" },
  { key: "b", name: "B", role: "Back in menus" },
  { key: "x", name: "X" },
  { key: "y", name: "Y" },
  { key: "lb", name: "LB", role: "Shift down (left paddle)" },
  { key: "rb", name: "RB", role: "Shift up (right paddle)" },
  { key: "back", name: "Back" },
  { key: "start", name: "Start", role: "Pause" },
  { key: "ls", name: "Left stick click" },
  { key: "rs", name: "Right stick click" },
  { group: "D-pad (menus)" },
  { key: "up", name: "D-pad up" },
  { key: "down", name: "D-pad down" },
  { key: "left", name: "D-pad left" },
  { key: "right", name: "D-pad right" },
]

// Besides SDL's devices (wheels, pedals, shifters, button boxes): the keyboard and mouse ("keyboard/k<virtual key>",
// mouse buttons included) and the controller ("pad/b<n>" buttons, "pad/a<n>" sticks, in the standard gamepad layout
// the page's Gamepad API reports). The game reads those two itself (src/kernel/imports_xam.cpp). GitHub #15.
const KEYBOARD = { guid: "keyboard", name: "Keyboard and mouse", kind: "keys and mouse buttons", axes: 0, buttons: 0, hats: 0, rest: [] }
const PAD = { guid: "pad", name: "Controller", kind: "Xbox layout", axes: 0, buttons: 0, hats: 0, rest: [] }
const PAD_BUTTONS = ["A", "B", "X", "Y", "LB", "RB", "LT", "RT", "Back", "Start", "Left stick click", "Right stick click",
  "D-pad up", "D-pad down", "D-pad left", "D-pad right"]
const PAD_AXES = ["Left stick X", "Left stick Y", "Right stick X", "Right stick Y"]
const MOUSE = { 0: 1, 2: 2, 1: 4, 3: 5, 4: 6 } // event.button to its virtual key
const isPseudo = (guid) => guid === "keyboard" || guid === "pad"
// The same device, the name's CRC in its SDL GUID (hex digits 4-7) aside: launchers before 0.9.8 named some
// wheels differently from the game, so bindings saved then carry another CRC (report #21).
const sameDevice = (a, b) => a === b || (a.length === 32 && b.length === 32 && a.slice(0, 4) === b.slice(0, 4) && a.slice(8) === b.slice(8))
const isKeyboard = (src) => src.startsWith("keyboard/")

// The game's own keys (src/kernel/imports_xam.cpp). A row without keys bound keeps its key, shown as a chip, unless
// another row has that key bound: a key does one thing, so binding it takes it from wherever it was.
const KEY_DEFAULTS = { steerleft: 65, steerright: 68, throttle: 87, brake: 83, a: 13, b: 8, x: 32, y: 89, lb: 81, rb: 69,
  back: 9, start: 27, up: 38, down: 40, left: 37, right: 39 }
const boundKeys = () => new Set(Object.values(bindings).flat().filter(isKeyboard))

// A Windows virtual key's name, for the chips.
function keyName(vk) {
  if ((vk >= 48 && vk <= 57) || (vk >= 65 && vk <= 90)) return String.fromCharCode(vk)
  if (vk >= 112 && vk <= 135) return "F" + (vk - 111)
  if (vk >= 96 && vk <= 105) return "Num " + (vk - 96)
  const names = { 1: "Left mouse button", 2: "Right mouse button", 4: "Middle mouse button", 5: "Mouse button 4", 6: "Mouse button 5",
    8: "Backspace", 9: "Tab", 13: "Enter", 16: "Shift", 17: "Ctrl", 18: "Alt", 20: "Caps Lock", 27: "Esc", 32: "Space",
    33: "Page Up", 34: "Page Down", 35: "End", 36: "Home", 37: "Left arrow", 38: "Up arrow", 39: "Right arrow", 40: "Down arrow",
    45: "Insert", 46: "Delete", 106: "Num *", 107: "Num +", 109: "Num -", 110: "Num .", 111: "Num /", 186: ";", 187: "=", 188: ",",
    189: "-", 190: ".", 191: "/", 192: "`", 219: "[", 220: "\\", 221: "]", 222: "'" }
  return names[vk] || "Key " + vk
}

let devices = []        // from SDL: guid, name, kind, axes, buttons, hats, rest
let bindings = {}       // target -> ["<guid>/<source>", ...]
let names = {}          // guid -> device name (remembered for devices not plugged in)
let live = new Map()    // guid -> latest state
let shown = null        // guid shown on the left
let listen = null       // { key, add, baseline, until }

if (host) host.addEventListener("message", (e) => {
  const m = e.data
  if (m.event === "state") return onState(m.data)
  if (m.event === "devices") return onDevices(m.data)
  const p = pending.get(m.id)
  if (!p) return
  pending.delete(m.id)
  m.ok ? p.resolve(m.result) : p.reject(new Error(m.error))
})

function call(cmd, args) {
  if (!host) return Promise.reject(new Error("Not running inside the launcher."))
  const id = nextId++
  return new Promise((resolve, reject) => {
    pending.set(id, { resolve, reject })
    host.postMessage({ id, cmd, args })
  })
}

function el(tag, cls, text) {
  const e = document.createElement(tag)
  if (cls) e.className = cls
  if (text !== undefined) e.textContent = text
  return e
}

function toast(text, good) {
  const t = $("toast")
  t.textContent = text
  t.className = "toast" + (good ? " good" : "")
  t.hidden = false
  clearTimeout(toast.timer)
  toast.timer = setTimeout(() => { t.hidden = true }, 4000)
}

// ---- tooltips (as the main window)
let tipTimer = null, tipFor = null
document.addEventListener("mouseover", (e) => {
  const t = e.target.closest("[data-tip]")
  if (t === tipFor) return
  hideTip()
  if (!t) return
  tipFor = t
  tipTimer = setTimeout(() => {
    const tip = $("tooltip")
    tip.textContent = t.dataset.tip
    tip.hidden = false
    const r = t.getBoundingClientRect()
    let x = Math.max(8, Math.min(r.left, innerWidth - tip.offsetWidth - 8)), y = r.bottom + 8
    if (y + tip.offsetHeight > innerHeight - 8) y = r.top - tip.offsetHeight - 8
    tip.style.left = x + "px"
    tip.style.top = y + "px"
    requestAnimationFrame(() => tip.classList.add("on"))
  }, 450)
})
document.addEventListener("mousedown", hideTip)
function hideTip() {
  clearTimeout(tipTimer)
  tipFor = null
  $("tooltip").classList.remove("on")
  $("tooltip").hidden = true
}

// ---- devices

function deviceName(guid) {
  if (guid === "keyboard") return KEYBOARD.name
  if (guid === "pad") return PAD.name
  const d = devices.find((x) => sameDevice(x.guid, guid))
  return d ? d.name : names[guid] || "Unplugged device"
}

function onDevices(list) {
  const before = devices.map((d) => d.guid).join()
  // Xbox (XInput) pads are the Controller entry (the game reads them itself); SDL's view of them would double up.
  // By name: wheels such as Thrustmaster's report themselves to SDL as gamepads.
  devices = [...list.filter((d) => !/xbox|x-box|xinput/i.test(d.name)), PAD, KEYBOARD]
  for (const d of devices) if (!isPseudo(d.guid)) names[d.guid] = d.name
  $("nodev").hidden = true
  if (before === devices.map((d) => d.guid).join()) return
  if (!devices.some((d) => d.guid === shown)) {
    const wheel = devices.find((d) => d.kind === "wheel") || devices[0]
    shown = wheel ? wheel.guid : null
  }
  const sel = $("device")
  sel.innerHTML = ""
  for (const d of devices) {
    const o = el("option", "", `${d.name} (${d.kind})`)
    o.value = d.guid
    o.selected = d.guid === shown
    sel.append(o)
  }
  drawDevice()
  drawRows()
}
$("device").onchange = () => { shown = $("device").value; drawDevice() }

function drawDevice() {
  const d = devices.find((x) => x.guid === shown)
  $("axes").innerHTML = $("buttons").innerHTML = $("hats").innerHTML = ""
  if (!d) { $("dev-title").textContent = "Inputs"; $("dev-sub").textContent = ""; return }
  $("dev-title").textContent = d.name
  if (d.guid === "keyboard") { $("dev-sub").textContent = "Keys and mouse buttons. Bind a row, then press the key or click the mouse button you want."; return }
  if (d.guid === "pad") { $("dev-sub").textContent = "An Xbox or other controller, by its buttons and sticks. Press a button on it once if Bind doesn't see it."; return }
  $("dev-sub").textContent = `${d.axes} axes, ${d.buttons} buttons, ${d.hats} hats. Move or press anything on it to see which number it is.`
  for (let i = 0; i < d.axes; i++) {
    const row = el("div", "axis-row")
    row.id = "ax" + i
    const bar = el("div", "axis-bar")
    bar.append(el("i"), el("span", "rest"))
    row.append(el("span", "", "Axis " + i), bar, el("span", "val", "0"))
    $("axes").append(row)
  }
  for (let i = 0; i < d.buttons; i++) {
    const b = el("span", "", String(i))
    b.id = "bt" + i
    $("buttons").append(b)
  }
  for (let i = 0; i < d.hats; i++) {
    const h = el("div", "")
    h.id = "ht" + i
    $("hats").append(h)
  }
}

const HAT = { 1: "up", 2: "right", 4: "down", 8: "left", 3: "up-right", 6: "down-right", 12: "down-left", 9: "up-left" }

function onState(states) {
  live = new Map(states.map((s) => [s.guid, s]))
  const d = devices.find((x) => x.guid === shown), s = live.get(shown)
  if (d && s) {
    s.axes.forEach((v, i) => {
      const row = $("ax" + i)
      if (!row) return
      const rest = d.rest[i] || 0
      // filled from the rest position (pedals) or the centre (steering and other centred axes)
      const from = Math.abs(rest) > 30000 ? rest : 0
      const a = (Math.min(from, v) + 32768) / 65535 * 100, b = (Math.max(from, v) + 32768) / 65535 * 100
      const fill = row.querySelector("i")
      fill.style.left = a + "%"
      fill.style.width = Math.max(b - a, 0.6) + "%"
      row.querySelector(".rest").style.left = (from + 32768) / 65535 * 100 + "%"
      row.querySelector(".val").textContent = v
      row.classList.toggle("hot", Math.abs(v - rest) > 3000)
    })
    s.buttons.forEach((down, i) => { const b = $("bt" + i); if (b) b.classList.toggle("down", down) })
    s.hats.forEach((h, i) => { const e = $("ht" + i); if (e) e.innerHTML = `Hat ${i}: <b>${h ? HAT[h] || h : "centred"}</b>` })
  }
  if (listen) checkListen()
}

// ---- binding

function sourceText(src) {
  const [guid, s] = src.split("/")
  const kind = s[0], rest = s.slice(1)
  let what
  if (kind === "k") what = keyName(parseInt(rest))
  else if (guid === "pad") what = kind === "a" ? PAD_AXES[parseInt(rest)] || "Axis " + parseInt(rest) : PAD_BUTTONS[parseInt(rest)] || "Button " + rest
  else if (kind === "a") what = "Axis " + parseInt(rest)
  else if (kind === "b") what = "Button " + rest
  else what = `Hat ${parseInt(rest)} ${{ u: "up", d: "down", l: "left", r: "right" }[s[s.length - 1]]}`
  return { guid, what, reversed: kind === "a" && s.endsWith("-") }
}

function drawRows() {
  const root = $("rows")
  root.innerHTML = ""
  const taken = boundKeys()
  for (const t of TARGETS) {
    if (t.group) { root.append(el("div", "group", t.group)); continue }
    const row = el("div", "bind-row" + (listen && listen.key === t.key ? " listening" : ""))
    const label = el("div", "")
    label.append(el("div", "what", t.name))
    if (t.role) label.append(el("div", "role", t.role))
    const chips = el("div", "chips-b")
    const all = bindings[t.key] || []
    const list = all
    if (listen && listen.key === t.key)
      chips.append(el("span", "listen-text", t.axisOnly ? "Turn the wheel or move a stick... Esc cancels" :
        "Press a key, button or pedal, or click a mouse button... Esc cancels"))
    const defaultKey = KEY_DEFAULTS[t.key]
    const showDefault = defaultKey && !list.some(isKeyboard) && !taken.has(`keyboard/k${defaultKey}`)
    if (!(listen && listen.key === t.key) && showDefault) {
      const chip = el("span", "chip-b")
      chip.dataset.tip = "The game's key for this. Binding another key here, or this key elsewhere, replaces it."
      chip.append(el("span", "", keyName(defaultKey)), el("span", "dev", "Keyboard default"))
      chips.append(chip)
    }
    else if (!list.length && !(listen && listen.key === t.key)) chips.append(el("span", "default", "game default"))
    list.forEach((src, i) => {
      const info = sourceText(src)
      const chip = el("span", "chip-b" + (isPseudo(info.guid) || devices.some((d) => sameDevice(d.guid, info.guid)) ? "" : " missing"))
      chip.append(el("span", "", info.what), el("span", "dev", deviceName(info.guid)))
      if (t.key === "steer") {
        const rev = el("button", "rev" + (info.reversed ? " on" : ""), "reverse")
        rev.dataset.tip = "Turns the steering the other way, if left is right."
        rev.onclick = () => { all[all.indexOf(src)] = info.reversed ? src.slice(0, -1) : src + "-"; save() }
        chip.append(rev)
      }
      const x = el("button", "", "×")
      x.dataset.tip = "Removes this one."
      x.onclick = () => { all.splice(all.indexOf(src), 1); save() }
      chip.append(x)
      chips.append(chip)
    })
    const acts = el("div", "acts")
    const bind = el("button", "primary small", "Bind")
    bind.dataset.tip = "Replaces this row's binding: press the button, move the axis or push the hat you want."
    bind.onclick = () => startListen(t, false)
    const add = el("button", "ghost small", "+ Add")
    add.dataset.tip = "Adds another input for this row (both work), for example a paddle and a button for shifting."
    add.onclick = () => startListen(t, true)
    const clear = el("button", "ghost small", "Default")
    clear.dataset.tip = "Back to what the game picks for your wheel."
    clear.disabled = !list.length
    clear.onclick = () => { bindings[t.key] = []; save() }
    acts.append(bind, add, clear)
    row.append(label, chips, acts)
    root.append(row)
  }
}

function startListen(t, add) {
  const baseline = new Map([...live].map(([g, s]) => [g, JSON.parse(JSON.stringify(s))]))
  listen = { key: t.key, axisOnly: !!t.axisOnly, add, baseline, pad: padState(), until: Date.now() + 10000 }
  drawRows()
}

function stopListen() { listen = null; drawRows() }
document.addEventListener("keydown", (e) => {
  if (!listen) return
  if (e.key === "Escape") return stopListen()
  if (!e.keyCode) return
  e.preventDefault()
  if (listen.axisOnly) return toast("Steering takes a wheel or a stick. For keys, bind Steer left and Steer right.")
  bind(`keyboard/k${e.keyCode}`)
})
document.addEventListener("mousedown", (e) => {
  if (!listen || listen.axisOnly || e.target.closest("button, select") || !MOUSE[e.button]) return
  e.preventDefault()
  bind(`keyboard/k${MOUSE[e.button]}`)
})
document.addEventListener("contextmenu", (e) => { if (listen) e.preventDefault() })

// The first connected controller through the Gamepad API (standard layout): buttons pressed, sticks. Only
// a standard-layout one: wheels are in that list too, and a wheel's button bound as a controller's
// ("pad/b1") is never read from the wheel (report #21).
function padState() {
  let pads = []
  try { pads = navigator.getGamepads ? [...navigator.getGamepads()] : [] } catch { } // not allowed: no controller, keys still bind
  const p = pads.find((g) => g && g.connected && g.mapping === "standard")
  return p ? { buttons: p.buttons.map((b) => b.pressed || b.value > 0.5), axes: [...p.axes] } : null
}

// The source caught for the listening row: replaces that row's bindings of this kind (Bind) or joins them (+ Add).
function bind(src) {
  // A key does one thing: take it off any other row first.
  if (isKeyboard(src))
    for (const k of Object.keys(bindings)) if (k !== listen.key) bindings[k] = bindings[k].filter((x) => x !== src)
  const list = bindings[listen.key] = listen.add ? bindings[listen.key] || [] : []
  if (!list.includes(src)) list.push(src)
  listen = null
  save()
}

function checkListen() {
  if (Date.now() > listen.until) { stopListen(); return toast("Nothing was pressed: try Bind again.") }
  const pad = padState()
  if (pad) {
    const was = listen.pad || { buttons: [], axes: [] }
    if (listen.axisOnly) {
      const i = pad.axes.findIndex((v, n) => n % 2 === 0 && Math.abs(v - (was.axes[n] || 0)) > 0.5) // a stick's X
      if (i >= 0) return bind(`pad/a${i}`)
    } else {
      const i = pad.buttons.findIndex((down, n) => down && !was.buttons[n])
      if (i >= 0) return bind(`pad/b${i}`)
    }
    listen.pad = pad
  }
  for (const [guid, s] of live) {
    const base = listen.baseline.get(guid)
    if (!base || !devices.some((d) => d.guid === guid)) continue // SDL's view of a controller: the Controller entry has it
    let found = null
    // The axis that moved most, once it's past a tenth of its range: a direct drive base set to 900 degrees
    // or more moves its steering axis little for a normal turn, and some pedals don't reach their full range
    // (a Simagic base's pedals moved 8000 of 65535: the old 22000 never caught either).
    let most = 6500
    s.axes.forEach((v, i) => { const d = Math.abs(v - base.axes[i]); if (d > most) { most = d; found = "a" + i } })
    if (!listen.axisOnly) {
      s.buttons.forEach((down, i) => { if (!found && down && !base.buttons[i]) found = "b" + i })
      s.hats.forEach((h, i) => {
        if (found || !h || base.hats[i]) return
        const dir = h & 1 ? "u" : h & 4 ? "d" : h & 8 ? "l" : "r"
        found = `h${i}${dir}`
      })
    }
    if (found) return bind(`${guid}/${found}`)
  }
}

async function save() {
  drawRows()
  try {
    const r = await call("save", { bindings, names })
    $("running").hidden = !r.running
  } catch (e) { toast(e.message) }
}

$("reset-all").onclick = () => {
  for (const k of Object.keys(bindings)) bindings[k] = []
  save()
  toast("Every row is back to the game default.", true)
}

// ---- force feedback (settings.ini, as the main window had them)

function option(value, name, selected) {
  const o = el("option", "", name)
  o.value = value
  o.selected = value === selected
  return o
}

function drawForce(f) {
  const percents = Array.from({ length: 21 }, (_, i) => i * 5)
  $("wheelStrength").replaceChildren(...percents.map((p) => option(p, p + "%", f.wheelStrength)))
  $("wheelSpring").replaceChildren(...percents.map((p) => option(p, p + "%", f.wheelSpring)))
  $("wheelRotation").replaceChildren(option(0, "As set on the wheel", f.wheelRotation),
    ...[270, 360, 540, 720, 900, 1080, 1440, 2520].map((d) => option(d, d + "°", f.wheelRotation)))
  $("wheelReverse").classList.toggle("on", !!f.wheelReverse)
  $("wheelReverse").setAttribute("aria-checked", !!f.wheelReverse)
}

async function setForce(patch) {
  try {
    const r = await call("ffb", patch)
    drawForce(r.ffb)
    $("running").hidden = !r.running
  } catch (e) { toast(e.message) }
}
for (const k of ["wheelStrength", "wheelSpring", "wheelRotation"]) $(k).onchange = (e) => setForce({ [k]: +e.target.value })
$("wheelReverse").onclick = () => setForce({ wheelReverse: !$("wheelReverse").classList.contains("on") })

;(async () => {
  try {
    const r = await call("init")
    drawForce(r.ffb)
    bindings = r.bindings || {}
    names = r.names || {}
    $("running").hidden = !r.running
    onDevices(r.devices || [])
    drawRows()
  } catch (e) { toast(e.message) }
})()
