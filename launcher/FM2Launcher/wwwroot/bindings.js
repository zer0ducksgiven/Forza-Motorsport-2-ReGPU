"use strict"
// The wheel Bindings window (BindingsForm.cs): live input from SDL, and wheel_bindings.ini for the game.

const $ = (id) => document.getElementById(id)
let nextId = 1
const pending = new Map()

// Pad inputs the game reads, in the order shown. role: what it does in the game, where that is certain.
const TARGETS = [
  { group: "Driving" },
  { key: "steer", name: "Steering", role: "Left stick, left and right", axisOnly: true },
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
  const d = devices.find((x) => x.guid === guid)
  return d ? d.name : names[guid] || "Unplugged device"
}

function onDevices(list) {
  const before = devices.map((d) => d.guid).join()
  devices = list
  for (const d of devices) names[d.guid] = d.name
  $("nodev").hidden = devices.length > 0
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
  if (kind === "a") what = "Axis " + parseInt(rest)
  else if (kind === "b") what = "Button " + rest
  else what = `Hat ${parseInt(rest)} ${{ u: "up", d: "down", l: "left", r: "right" }[s[s.length - 1]]}`
  return { guid, what, reversed: kind === "a" && s.endsWith("-") }
}

function drawRows() {
  const root = $("rows")
  root.innerHTML = ""
  for (const t of TARGETS) {
    if (t.group) { root.append(el("div", "group", t.group)); continue }
    const row = el("div", "bind-row" + (listen && listen.key === t.key ? " listening" : ""))
    const label = el("div", "")
    label.append(el("div", "what", t.name))
    if (t.role) label.append(el("div", "role", t.role))
    const chips = el("div", "chips-b")
    const list = bindings[t.key] || []
    if (listen && listen.key === t.key)
      chips.append(el("span", "listen-text", t.axisOnly ? "Turn the wheel (or move the axis)... Esc cancels" : "Press, move or push it now... Esc cancels"))
    else if (!list.length) chips.append(el("span", "default", "game default"))
    list.forEach((src, i) => {
      const info = sourceText(src)
      const chip = el("span", "chip-b" + (devices.some((d) => d.guid === info.guid) ? "" : " missing"))
      chip.append(el("span", "", info.what), el("span", "dev", deviceName(info.guid)))
      if (t.key === "steer") {
        const rev = el("button", "rev" + (info.reversed ? " on" : ""), "reverse")
        rev.dataset.tip = "Turns the steering the other way, if left is right."
        rev.onclick = () => { list[i] = info.reversed ? src.slice(0, -1) : src + "-"; save() }
        chip.append(rev)
      }
      const x = el("button", "", "×")
      x.dataset.tip = "Removes this one."
      x.onclick = () => { list.splice(i, 1); save() }
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
  if (!devices.length) return toast("Plug in your wheel first.")
  const baseline = new Map([...live].map(([g, s]) => [g, JSON.parse(JSON.stringify(s))]))
  listen = { key: t.key, axisOnly: !!t.axisOnly, add, baseline, until: Date.now() + 10000 }
  drawRows()
}

function stopListen() { listen = null; drawRows() }
document.addEventListener("keydown", (e) => { if (e.key === "Escape" && listen) stopListen() })

function checkListen() {
  if (Date.now() > listen.until) { stopListen(); return toast("Nothing was pressed: try Bind again.") }
  for (const [guid, s] of live) {
    const base = listen.baseline.get(guid)
    if (!base) continue
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
    if (found) {
      const list = bindings[listen.key] = listen.add ? bindings[listen.key] || [] : []
      const src = `${guid}/${found}`
      if (!list.includes(src)) list.push(src)
      listen = null
      save()
      return
    }
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
