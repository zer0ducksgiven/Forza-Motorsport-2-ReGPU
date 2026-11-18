"use strict"
// The Hypermode Options window (Hypermode.cs): ircluzar's low-spec performance cuts (the game's gpu/hyper.h). Every
// change is saved at once to Documents\fm2\hypermode.ini, which the game reads when it starts.
// What each option does and what it measured comes from docs/hyper/HYPER-SETTINGS-GUIDE.md and the prototype's own
// settings window (tools/hyper_menu.cpp): a Radeon Vega laptop (Ryzen 3 5300U) and an Intel HD 630 (Core i3-7100T),
// both at 720p.

const $ = (id) => document.getElementById(id)
let nextId = 1
const pending = new Map()
if (host) host.addEventListener("message", (e) => {
  const m = e.data
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
  toast.timer = setTimeout(() => { t.hidden = true }, 5000)
}
async function guarded(fn) {
  try { return await fn() } catch (e) { toast(e.message) }
}

// Tooltips: any element with data-tip, shown after a short hover.
let tipTimer = 0
let tipFor = null
document.addEventListener("mouseover", (e) => {
  const t = e.target.closest && e.target.closest("[data-tip]")
  if (t === tipFor) return
  hideTip()
  if (!t) return
  tipFor = t
  tipTimer = setTimeout(() => {
    const tip = $("tooltip")
    tip.textContent = t.dataset.tip
    tip.hidden = false
    const r = t.getBoundingClientRect()
    const x = Math.max(8, Math.min(r.left + Math.min(r.width / 2, 120) - 20, innerWidth - tip.offsetWidth - 8))
    let y = r.bottom + 8
    if (y + tip.offsetHeight > innerHeight - 8) y = Math.max(8, r.top - tip.offsetHeight - 8)
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

// ---- the options
// recompile: "all" = the shaders' text or compile level changes, so every shader is built again at the next start;
// "variants" = new per-draw shader variants are compiled in the background as they are first drawn.
const GROUPS = [
  { id: "surf", title: "Surfaces", note: "Each one softens one thing you can name" },
  { id: "geom", title: "Geometry", note: "Fewer draws and triangles" },
  { id: "shad", title: "Shaders", note: "No visible change; faster shader code" },
]
const ITEMS = [
  // Surfaces
  { key: "helpers", group: "surf", label: "Reflection and glow size", sub: "Offscreen helper surfaces", off: "100",
    choices: [["100", "100%"], ["75", "75%"], ["50", "50%"], ["25", "25%"]],
    tip: "Reflections, bloom and blur are drawn at this size per side; the screen itself keeps its resolution.",
    notice: "Reflections in paint and glass get blurrier and the glow around bright areas softer.",
    perf: "50%: +17% (Vega laptop), +13% (Intel HD 630). 25%: +21%, +19%." },
  { key: "aniso", group: "surf", label: "Anisotropic filtering cap", sub: "Lowers the launcher's own setting", off: "16",
    choices: [["16", "No cap"], ["8", "8x"], ["4", "4x"], ["2", "2x"], ["0", "Off"]],
    tip: "Caps texture filtering at shallow angles, whatever Anisotropic filtering says in the launcher.",
    notice: "The road, kerbs and markings blur sooner in the distance.",
    perf: "2x: +8% (Vega laptop), +1% (Intel HD 630). Off: +9%, +3%." },
  { key: "freeze", group: "surf", label: "Refresh reflections less often", sub: "Helper views redrawn every few frames", off: "0",
    tip: "Reflection, environment and lighting views are drawn normally for the first 120 frames, then only every few frames; in between they keep their last picture.",
    notice: "Reflections lag slightly behind the car's movement.",
    perf: "+4% (Vega laptop), +13% (Intel HD 630)." },
  { key: "freeze_refresh", group: "surf", parent: "freeze", label: "Redraw every", sub: "Odd numbers keep paired views in step", off: "3",
    choices: [["2", "2 frames"], ["3", "3 frames"], ["4", "4 frames"], ["6", "6 frames"]],
    tip: "How often a frozen helper view is redrawn.",
    perf: "4 or 6 gained nothing measurable over 3 on the Intel HD 630." },
  { key: "resolve1010", group: "surf", label: "32-bit colour copies", sub: "No visible change", off: "0",
    tip: "Copies into the game's 10:10:10:2 colour format stay 32-bit instead of 64-bit float: half the memory traffic.",
    notice: "Nothing.",
    perf: "+3% (Vega laptop), +1% (Intel HD 630): both within run-to-run noise." },
  { key: "resolve_by_size", group: "surf", label: "Copies kept per size", sub: "No visible change", off: "0",
    tip: "Keeps a copy of a resolved surface for each size it is read at, so a texture read at another size gets a matching copy. It fixed a flicker of frozen shadow maps.",
    notice: "Nothing.",
    perf: "Within noise on both test machines." },
  { key: "cheap_upscale", group: "surf", label: "One-pass upscale", sub: "Only with FSR 1 below 100% render scale", off: "0",
    tip: "With Upscaling set to FSR 1 in the launcher: replaces FSR 1's three full-screen passes with one plain bilinear scale.",
    notice: "A blurrier picture than FSR 1 gives, with no sharpening.",
    perf: "Saves about 1.8 ms of graphics time a frame on the Vega laptop." },

  // Geometry
  { key: "cull_pixels", group: "geom", label: "Skip tiny draws", sub: "Counted in pixels on screen", off: "-1",
    choices: [["-1", "Off"], ["0", "Hidden only"], ["64", "Up to 64 px"], ["128", "Up to 128 px"], ["256", "Up to 256 px"]],
    tip: "Counts the pixels each object covers and skips those at or under this size. Hidden only skips just what is completely covered by something else. Always runs with the flicker fixes (invisible re-measure, hysteresis, blink fix).",
    notice: "Tiny distant things disappear: crowd pieces, small signs. At 256 px a little flicker can remain while parked.",
    perf: "Hidden only: +8% (Vega laptop). 64 px: +12%. 256 px: +17%, and +7% on the Intel HD 630. Counting pixels has a cost of its own on Intel, so the smaller sizes can lose speed there." },
  { key: "cull_all", group: "geom", parent: "cull_pixels", label: "See-through draws too", sub: "Not only solid ones", off: "0",
    tip: "Blended (see-through) draws can be skipped as well, not only opaque ones.",
    perf: "Part of the Turbo and Potato presets; not measured alone." },
  { key: "cull_passes", group: "geom", parent: "cull_pixels", label: "In every pass", sub: "Reflections and shadows too", off: "0",
    tip: "Tiny draws are also skipped in the reflection and shadow passes, not only the main view.",
    perf: "Part of the Turbo and Potato presets; not measured alone." },
  { key: "decimate", group: "geom", label: "Simplify big meshes", sub: "Triangles kept", off: "0",
    choices: [["0", "Off"], ["90", "Keep 90%"], ["75", "Keep 75%"], ["50", "Keep 50%"], ["25", "Keep 25%"], ["10", "Keep 10%"]],
    tip: "Big meshes are drawn with fewer triangles, simplified in the background with their borders locked so pieces still meet.",
    notice: "Stands and buildings look slightly angular, with a one-time pop the first time each is seen.",
    perf: "Keep 10%: +5% (Vega laptop), +3% (Intel HD 630)." },
  { key: "vcache", group: "geom", parent: "decimate", label: "Reorder triangles", sub: "No visible change", off: "0",
    tip: "Reorders the simplified meshes' triangles for the graphics card's vertex cache and for less overdraw. Same triangles. It runs inside the simplifier, so it needs Simplify big meshes on.",
    notice: "Nothing.",
    perf: "+4.5% on the Intel HD 630 (measured within the full set)." },

  // Shaders
  { key: "shader_opt", group: "shad", label: "Optimise shaders", sub: "Slower compiles, faster shaders", off: "0", recompile: "all",
    tip: "Compiles the game's translated shaders with optimisation on.",
    notice: "Nothing in play. Compiling takes longer: the first start after switching is slow, and so is every new shader after it.",
    perf: "+3% (Vega laptop), +1% (Intel HD 630), where it cut 6.4 ms of graphics time a frame." },
  { key: "plain_mul", group: "shad", label: "Plain multiplies", sub: "Skips an Xbox 360 rule where it can't matter", off: "0", recompile: "all",
    tip: "The Xbox 360 treats 0 x anything as 0, which costs a compare on every multiply. This keeps the rule only where a value could be infinite or not a number, and uses plain multiplies everywhere else.",
    notice: "Nothing seen in testing. If the analysis ever misses a case, it would show as black or white specks.",
    perf: "+9% (Vega laptop), +13% (Intel HD 630)." },
  { key: "swizzle_sel", group: "shad", label: "Simpler texture channel picks", sub: "Swizzles as selects", off: "0", recompile: "all",
    tip: "Texture channel reordering becomes simple selects instead of indexed lookups, which Intel's shader compiler handles better.",
    notice: "Nothing.",
    perf: "+1% (Vega laptop), +6% (Intel HD 630)." },
  { key: "lean_lod", group: "shad", label: "Lean texture detail test", sub: "Drops an unused check", off: "0", recompile: "all",
    tip: "Drops a developer-only detail-level test from every texture read.",
    notice: "Nothing.",
    perf: "+1% on both; 3.5 ms less graphics time a frame on the Intel HD 630." },
  { key: "fetchspec", group: "shad", label: "Texture read specialisation", sub: "A shader variant per kind of draw", off: "0", recompile: "variants",
    tip: "Each draw gets a version of its pixel shader with its textures' sampling modes built in, so the checks that don't apply are gone.",
    notice: "Nothing once warm. The first race on each track can stutter briefly while the variants compile.",
    perf: "+7% (Vega laptop), +15% (Intel HD 630): the biggest single gain on Intel." },
  { key: "tailspec", group: "shad", label: "Output specialisation", sub: "A shader variant per kind of draw", off: "0", recompile: "variants",
    tip: "Each draw's alpha test and output settings are built into its pixel shader as constants.",
    notice: "Nothing once warm. More variants compile the first time each is seen.",
    perf: "Within noise on both test machines alone; +6.5% in the full set on the Intel HD 630." },
]
const SHADER_ALL = { shader_opt: "1", plain_mul: "1", swizzle_sel: "1", lean_lod: "1", fetchspec: "1", tailspec: "1" }
const PRESETS = [
  { name: "Lossless", blurb: "Only the options measured with no visible change: the picture matches normal.",
    set: { resolve1010: "1", resolve_by_size: "1", ...SHADER_ALL } },
  { name: "Turbo", blurb: "Potato's options, but only objects that are completely hidden are skipped.",
    set: { aniso: "0", helpers: "25", resolve1010: "1", resolve_by_size: "1", decimate: "10", vcache: "1", cull_pixels: "0", cull_all: "1",
           cull_passes: "1", freeze: "1", freeze_refresh: "3", ...SHADER_ALL } },
  { name: "Potato", blurb: "Everything, for the weakest hardware. Tiny distant objects are left out.",
    set: { aniso: "0", helpers: "25", resolve1010: "1", resolve_by_size: "1", decimate: "10", vcache: "1", cull_pixels: "256", cull_all: "1",
           cull_passes: "1", freeze: "1", freeze_refresh: "4", ...SHADER_ALL } },
  { name: "Custom", blurb: "Your own mix of the options below." },
]
// The one-pass upscale belongs to the launcher's upscaling choice, not to a preset.
const PRESET_FREE = ["cheap_upscale"]

let state = null
let baseline = "" // the shader-rebuilding options in force when the window opened

const isOn = (item, values) => values[item.key] !== item.off
const itemOf = (key) => ITEMS.find((i) => i.key === key)
function presetValues(preset) {
  const v = {}
  for (const i of ITEMS) v[i.key] = PRESET_FREE.includes(i.key) ? state.values[i.key] : (preset.set[i.key] ?? i.off)
  return v
}
function matchingPreset(values) {
  for (const p of PRESETS) {
    if (!p.set) continue
    const want = presetValues(p)
    if (ITEMS.every((i) => values[i.key] === want[i.key])) return p.name
  }
  return "Custom"
}
// What makes the game rebuild every shader: the "all" options, as they will be in force.
const recompileKey = (s) => !s.enabled ? "" : ITEMS.filter((i) => i.recompile === "all" && isOn(i, s.values)).map((i) => i.key).join(",")

function tipOf(item) {
  let t = item.tip
  if (item.notice) t += "\n\nWhat you'll notice: " + item.notice
  if (item.perf) t += "\n\nMeasured at 720p: " + item.perf
  if (item.recompile === "all") t += "\n\nRecompiles shaders: changing this rebuilds every shader the next time the game starts. Expect a loading screen of about five minutes on a fast processor and longer on a slow one, once. Each combination you have used stays stored, so going back to one is quick."
  if (item.recompile === "variants") t += "\n\nNew shaders in play: extra shader variants are compiled in the background the first time each kind of draw is seen, so the first race on a track can stutter briefly. Later races are smooth."
  return t
}

function build() {
  const presets = $("presets")
  for (const p of PRESETS) {
    const b = el("button", "", p.name)
    b.dataset.preset = p.name
    b.dataset.tip = p.blurb
    b.onclick = () => {
      if (!p.set) return change({ preset: "Custom" })
      change({ preset: p.name, values: presetValues(p) })
    }
    presets.append(b)
  }
  const grid = $("grid")
  for (const g of GROUPS) {
    const panel = el("section", "panel")
    const head = el("div", "panel-head")
    head.append(el("h2", "", g.title), el("div", "panel-note", g.note))
    panel.append(head)
    for (const item of ITEMS.filter((i) => i.group === g.id)) {
      const row = el(item.choices ? "label" : "div", (item.choices ? "field" : "toggle-row") + (item.parent ? " child" : ""))
      row.id = "row-" + item.key
      row.dataset.tip = tipOf(item)
      const title = el("div", item.choices ? "name" : "toggle-title", item.label)
      if (item.recompile) title.append(" ", el("span", "badge " + (item.recompile === "all" ? "recompile" : "variants"), item.recompile === "all" ? "Recompiles shaders" : "New shaders in play"))
      const sub = el("div", item.choices ? "sub" : "toggle-sub", item.sub)
      if (item.choices) {
        const select = el("select")
        select.id = "opt-" + item.key
        for (const [value, name] of item.choices) { const o = el("option", "", name); o.value = value; select.append(o) }
        select.onchange = () => setValue(item.key, select.value)
        row.append(title, select, sub)
      } else {
        const text = el("div")
        text.append(title, sub)
        const sw = el("button", "switch")
        sw.id = "opt-" + item.key
        sw.setAttribute("role", "switch")
        sw.onclick = () => setValue(item.key, state.values[item.key] === "1" ? "0" : "1")
        row.append(text, sw)
      }
      panel.append(row)
    }
    grid.append(panel)
  }
}

function setValue(key, value) {
  const values = { ...state.values, [key]: value }
  change({ values, preset: matchingPreset(values) })
  const item = itemOf(key)
  if (item.recompile === "all" && state.enabled) toast("The game will recompile its shaders the next time it starts.", true)
}

// Saved at once (no delay: the window can be closed straight after a click). The reply only brings what the launcher
// knows (is the game running, the renderer): the choices on screen are never replaced by an older save's echo.
let saving = Promise.resolve()
function change(patch) {
  Object.assign(state, patch)
  render()
  saving = guarded(async () => {
    const saved = await call("hyper.save", { enabled: state.enabled, preset: state.preset, values: state.values })
    state.vulkan = saved.vulkan
    state.running = saved.running
    render()
  })
}

function render() {
  const usable = !state.vulkan
  const live = usable && state.enabled
  $("vulkanNote").hidden = usable
  $("master").classList.toggle("on", live)
  $("master").disabled = !usable
  $("masterRow").classList.toggle("on", live)
  $("masterSub").textContent = !usable ? "Needs Direct3D 12" : state.enabled ? "On: " + state.preset : "Off: the game runs as normal"
  for (const b of $("presets").children) {
    b.classList.toggle("on", b.dataset.preset === state.preset)
    b.disabled = !live
  }
  $("presetRow").classList.toggle("off", !live)
  $("presetBlurb").textContent = (PRESETS.find((p) => p.name === state.preset) || PRESETS[3]).blurb
  for (const item of ITEMS) {
    const control = $("opt-" + item.key)
    const parentOn = !item.parent || isOn(itemOf(item.parent), state.values)
    if (item.choices) control.value = state.values[item.key]
    else control.classList.toggle("on", state.values[item.key] === "1" && parentOn)
    control.disabled = !live || !parentOn
    $("row-" + item.key).classList.toggle("off", !live || !parentOn)
  }
  // The shader rebuild: said plainly whenever what is saved now differs from what the window opened with.
  const note = $("recompileNote")
  const now = recompileKey(state)
  if (usable && now !== baseline && now) {
    note.hidden = false
    note.replaceChildren(el("b", "", "The next start will recompile every shader. "),
      "You changed an option marked Recompiles shaders. The first start with a new combination of them rebuilds the game's shaders: "
      + "expect a loading screen of about five minutes on a fast processor and longer on a slow one, once. Each combination you have used "
      + "stays stored, so going back to one is quick.")
  } else if (usable && now !== baseline) {
    note.hidden = false
    note.replaceChildren("The game goes back to its normal shaders at the next start. Those are already stored, so there is no long wait.")
  } else if (live && now) {
    note.hidden = false
    note.replaceChildren("Options marked Recompiles shaders are on. The game builds a separate set of shaders for them, and the shaders that ship with the game don't cover it: after an update, or the first time on a PC, expect one long first start.")
  } else
    note.hidden = true
  $("status").textContent = state.running ? "The game is running: changes apply the next time it starts." : "Changes are saved as you make them and apply the next time the game starts."
}

$("master").onclick = () => {
  const enabled = !state.enabled
  const patch = { enabled }
  // First time on with nothing chosen: start from a preset (Potato on integrated graphics, the lossless set otherwise).
  if (enabled && ITEMS.every((i) => !isOn(i, state.values))) {
    const p = PRESETS.find((x) => x.name === (state.integrated ? "Potato" : "Lossless"))
    patch.preset = p.name
    patch.values = presetValues(p)
  }
  change(patch)
}
$("close").onclick = async () => { await saving; guarded(() => call("hyper.close")) }

build()
guarded(async () => {
  state = await call("hyper.state")
  state.preset = matchingPreset(state.values) === "Custom" ? "Custom" : matchingPreset(state.values)
  baseline = recompileKey(state)
  render()
})
