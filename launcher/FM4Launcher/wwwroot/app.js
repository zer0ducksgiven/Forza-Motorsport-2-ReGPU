"use strict"
// The launcher's page: state from the host (Bridge.cs) drawn; every change saved back to settings.ini.

const $ = (id) => document.getElementById(id)
let state = null
let customFps = false // "Custom…" picked in the Max FPS list
let update = null
let nextId = 1
const pending = new Map()

// ---- host calls

if (host) host.addEventListener("message", (e) => {
  const m = e.data
  if (m.event === "progress") return onProgress(m.data)
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

// ---- helpers

function el(tag, cls, text) {
  const e = document.createElement(tag)
  if (cls) e.className = cls
  if (text !== undefined) e.textContent = text
  return e
}
function size(bytes) {
  if (bytes >= 1073741824) return (bytes / 1073741824).toFixed(2) + " GB"
  if (bytes >= 1048576) return (bytes / 1048576).toFixed(1) + " MB"
  return Math.max(1, Math.round(bytes / 1024)) + " KB"
}
function toast(text, good) {
  const t = $("toast")
  t.textContent = text
  t.className = "toast" + (good ? " good" : "")
  t.hidden = false
  clearTimeout(toast.timer)
  toast.timer = setTimeout(() => { t.hidden = true }, 5000)
}
function confirmBox(title, text, yes) {
  return askBox(title, text, yes).then((v) => v !== null)
}
// The same dialog with a text box (opts.input: its starting text) or a list (opts.options: [{value, name}]):
// resolves to what was entered or picked, or null when cancelled.
function askBox(title, text, yes, opts) {
  opts = opts || {}
  return new Promise((resolve) => {
    $("confirm-title").textContent = title
    $("confirm-text").textContent = text
    $("confirm-yes").textContent = yes || "OK"
    $("confirm").querySelector("[data-choice=no]").textContent = opts.no || "Cancel"
    const input = $("confirm-input"), select = $("confirm-select")
    input.hidden = opts.input === undefined
    select.hidden = !opts.options
    if (!input.hidden) input.value = opts.input
    if (opts.options) fill(select, opts.options, opts.options.length ? opts.options[0].value : "")
    const box = $("confirm")
    box.hidden = false
    if (!input.hidden) { input.focus(); input.select() }
    const done = (ok) => {
      box.hidden = true
      input.onkeydown = null
      resolve(!ok ? null : !input.hidden ? input.value.trim() : opts.options ? select.value : true)
    }
    input.onkeydown = (e) => { if (e.key === "Enter") done(true); if (e.key === "Escape") done(false) }
    box.onclick = (e) => {
      const c = e.target.dataset && e.target.dataset.choice
      if (!c && e.target !== box) return
      done(c === "yes")
    }
  })
}
async function guarded(fn) {
  try { return await fn() } catch (e) { toast(e.message) }
}

// ---- tooltips: any element with data-tip, shown after a short hover

let tipTimer = 0
let tipFor = null
document.addEventListener("mouseover", (e) => {
  const t = e.target.closest && e.target.closest("[data-tip]")
  if (t === tipFor) return
  hideTip()
  if (!t) return
  tipFor = t
  tipTimer = setTimeout(() => showTip(t), 450)
})
document.addEventListener("mousedown", hideTip)
document.addEventListener("scroll", hideTip, true)
function showTip(t) {
  const tip = $("tooltip")
  tip.textContent = t.dataset.tip
  tip.hidden = false
  const r = t.getBoundingClientRect()
  const w = tip.offsetWidth, h = tip.offsetHeight
  let x = r.left + Math.min(r.width / 2, 120) - 20
  let y = r.bottom + 8
  if (y + h > innerHeight - 8) y = r.top - h - 8
  x = Math.max(8, Math.min(x, innerWidth - w - 8))
  tip.style.left = x + "px"
  tip.style.top = y + "px"
  requestAnimationFrame(() => tip.classList.add("on"))
}
function hideTip() {
  clearTimeout(tipTimer)
  tipFor = null
  const tip = $("tooltip")
  tip.classList.remove("on")
  tip.hidden = true
}

// ---- tabs

$("tabs").onclick = (e) => {
  const b = e.target.closest("button")
  if (!b) return
  for (const t of $("tabs").children) t.classList.toggle("on", t === b)
  for (const p of ["play", "profiles", "graphics", "settings", "report", "updates", "about"]) $("page-" + p).hidden = p !== b.dataset.tab
  $("tabs").dataset.shown = b.dataset.tab
  if (b.dataset.tab === "profiles") loadProfiles()
  else if (b.dataset.tab === "report") loadReport()
  else if (b.dataset.tab !== "play" && b.dataset.tab !== "graphics") refresh()
}

// The title opens the About page (no tab of its own).
$("brand").onclick = () => {
  for (const t of $("tabs").children) t.classList.remove("on")
  for (const p of ["play", "profiles", "graphics", "settings", "report", "updates"]) $("page-" + p).hidden = true
  $("page-about").hidden = false
  $("tabs").dataset.shown = "about"
}
$("brand").onkeydown = (e) => { if (e.key === "Enter" || e.key === " ") $("brand").onclick() }

// The selected profile's actions, in a pop-up opened from under the list.
$("prof-manage").onclick = () => { $("prof-modal").hidden = false }
$("prof-modal-close").onclick = () => { $("prof-modal").hidden = true }
$("prof-modal").onclick = (e) => { if (e.target === $("prof-modal")) $("prof-modal").hidden = true }

// The DLC packs' switches, in a pop-up.
$("dlc-manage").onclick = () => { $("dlc-modal").hidden = false }
$("dlc-modal-close").onclick = () => { $("dlc-modal").hidden = true }
$("dlc-modal").onclick = (e) => { if (e.target === $("dlc-modal")) $("dlc-modal").hidden = true }

// ---- drawing

function fill(select, items, value) {
  select.replaceChildren()
  for (const it of items) {
    const o = el("option", "", it.name)
    o.value = String(it.value)
    select.append(o)
  }
  select.value = String(value)
  if (select.selectedIndex < 0 && select.options.length) select.selectedIndex = select.options.length - 1
}

function render() {
  const s = state, set = s.settings, o = s.options
  $("version").textContent = "Version " + s.version + " · native PC port"

  const chips = $("chips")
  chips.replaceChildren()
  const chip = (cls, text, tip) => { const c = el("span", "chip " + cls); c.append(el("span", "dot"), text); if (tip) c.dataset.tip = tip; chips.append(c) }
  chip(s.checks.game ? "ok" : "bad", s.checks.game ? "Game files" : "Game files missing", "The game built from your disc, in the install's game folder.")
  chip(s.checks.running ? "warn" : "ok", s.checks.running ? "Game running" : "Not running")
  // play and graphics pages
  fill($("resolution"), o.resolutions.map((r) => ({ value: r, name: r.replace("x", " × ") })), set.resolution)
  fill($("aspect"), o.aspects, set.aspect)
  const custom = customFps || !o.fps.includes(set.maxFps)
  fill($("maxFps"), [...o.fps.map((f) => ({ value: f, name: f + " fps" })), { value: "custom", name: "Custom…" }], custom ? "custom" : set.maxFps)
  $("customFpsField").hidden = !custom
  if (document.activeElement !== $("customFps")) $("customFps").value = set.maxFps
  fill($("msaa"), o.msaa, set.msaa)
  fill($("language"), o.languages.map((l) => ({ value: l.id, name: l.name })), set.language)
  $("lang-note").textContent = o.missingLanguages.length
    ? `Your disc also has ${o.missingLanguages.join(", ")} text. Those can't be picked yet.` : ""
  for (const b of $("display").children) b.classList.toggle("on", (b.dataset.full === "1") === set.fullscreen)
  for (const k of ["vsync", "enhance"]) $(k).classList.toggle("on", !!set[k])

  const lang = o.languages.find((l) => l.id === set.language)
  const aspect = o.aspects.find((a) => a.value === set.aspect)
  $("summary").innerHTML = ""
  const line = (label, value) => { const d = el("div"); d.append(label + ": "); d.append(el("b", "", value)); $("summary").append(d) }
  line("Profile", set.profile || "Player")
  line("Display", `${set.resolution.replace("x", " × ")}, ${set.fullscreen ? "fullscreen" : "windowed"}${aspect && set.aspect !== "auto" ? ", " + aspect.name : ""}`)
  line("Frame rate", `${set.maxFps} fps cap, VSync ${set.vsync ? "on" : "off"}`)
  line("Anti-aliasing", (o.msaa.find((m) => m.value === set.msaa) || { name: "Off" }).name)
  line("Language", lang ? lang.name : "English")

  const cl = $("checklist")
  cl.replaceChildren()
  const item = (ok, text) => { const d = el("div", "check-item " + (ok ? "ok" : "bad")); d.append(el("span", "dot"), text); cl.append(d) }
  item(s.checks.exe, s.checks.exe ? "fm4.exe found" : "fm4.exe is missing from this install")
  item(s.checks.game, s.checks.game ? "Game files ready" : "Game files missing: run the installer again")
  item(s.shaders.exists, s.shaders.exists ? `Shaders compiled (${s.shaders.shaders.toLocaleString()})` : "First start compiles shaders (about 3½ minutes)")
  if (s.dlc.length) item(true, `${s.dlc.filter((d) => d.enabled).length} of ${s.dlc.length} DLC packs on`)
  $("launch").disabled = !s.checks.exe || !s.checks.game || s.checks.running
  $("launch").textContent = s.checks.running ? "Running" : "Play"

  // settings page
  const st = $("shader-stats")
  st.replaceChildren()
  const stat = (n, l) => { const d = el("div", "stat"); d.append(el("div", "n", n), el("div", "l", l)); st.append(d) }
  stat(s.shaders.exists ? s.shaders.shaders.toLocaleString() : "0", "compiled shaders")
  stat(s.shaders.exists ? size(s.shaders.bytes) : "-", "on disk")
  stat(s.shaders.built ? new Date(s.shaders.built).toLocaleDateString() : "-", "pipelines saved")
  stat(s.shaders.exists ? "Ready" : "Empty", "state")
  $("shaders-delete").disabled = !s.shaders.exists || s.checks.running
  $("shaders-rebuild").disabled = s.checks.running || !s.checks.exe

  $("dlc-summary").textContent = s.dlc.length
    ? `${s.dlc.filter((d) => d.enabled).length} of ${s.dlc.length} car packs on.`
    : "No car packs installed."
  const dl = $("dlc")
  dl.replaceChildren()
  if (!s.dlc.length) dl.append(el("div", "empty", "No DLC installed. Add DLC to bring in the car packs."))
  for (const p of s.dlc) {
    const it = el("div", "item" + (p.enabled ? "" : " off"))
    const main = el("div", "main")
    main.append(el("div", "title", p.name), el("div", "meta", size(p.bytes)))
    const sw = el("button", "switch" + (p.enabled ? " on" : ""))
    sw.dataset.tip = p.enabled ? "On: the game loads this pack. Click to switch it off (it's kept in dlc-disabled)." : "Off: kept, not loaded. Click to switch it on."
    sw.onclick = () => guarded(async () => { state = await call("dlc.toggle", { id: p.id, enabled: !p.enabled }); render() })
    it.append(main, sw)
    dl.append(it)
  }
  $("dlc-add").disabled = s.checks.running

  const f = $("folders")
  f.replaceChildren()
  for (const [k, v] of [["Install", s.paths.root], ["User data", s.paths.user]]) f.append(el("div", "k", k), el("div", "v", v))
}

function renderUpdate() {
  const u = update
  $("v-installed").textContent = (u && u.installed) || (state && state.version) || "-"
  $("v-latest").textContent = (u && u.latest) || "-"
  $("update-badge").hidden = !(u && u.available)
  const us = $("update-state")
  if (!u || !u.latest) { us.textContent = ""; us.className = "update-state" }
  else if (u.available) { us.textContent = `Version ${u.latest} is available.`; us.className = "update-state new" }
  else { us.textContent = "You're on the latest version."; us.className = "update-state ok" }
  $("update-apply").disabled = !(u && u.available && u.hasPatch) || (state && state.checks.running)
  $("update-apply").textContent = u && u.patchBytes ? `Download & install (${size(u.patchBytes)})` : "Download & install"
  $("notes-title").textContent = u && u.name ? u.name : "Release notes"
  const n = $("notes")
  n.replaceChildren()
  if (u && u.notes) n.append(...markdown(u.notes))
  else if (u) n.append(el("div", "empty", "No notes for this release."))
}

// A small Markdown subset (what the release notes use): headings, bullet lists, **bold**, `code`, paragraphs.
function markdown(text) {
  const out = []
  let list = null
  const inline = (s, into) => {
    for (const part of s.split(/(\*\*[^*]+\*\*|`[^`]+`)/)) {
      if (part.startsWith("**") && part.endsWith("**")) into.append(el("b", "", part.slice(2, -2)))
      else if (part.startsWith("`") && part.endsWith("`")) into.append(el("code", "", part.slice(1, -1)))
      else if (part) into.append(part.replace(/\[([^\]]+)\]\([^)]+\)/g, "$1"))
    }
    return into
  }
  for (const raw of text.replace(/\r/g, "").split("\n")) {
    const line = raw.trim()
    if (/^[-*] /.test(line)) {
      if (!list) { list = el("ul"); out.push(list) }
      list.append(inline(line.slice(2), el("li")))
      continue
    }
    list = null
    if (!line || line.startsWith("🤖")) continue
    const h = line.match(/^#+\s*(.*)$/)
    out.push(h ? inline(h[1], el("h3")) : inline(line, el("p")))
  }
  return out
}

// ---- settings changes

function current() {
  return { ...state.settings }
}
let saveTimer = 0
function change(patch) {
  Object.assign(state.settings, patch)
  render()
  clearTimeout(saveTimer)
  saveTimer = setTimeout(() => guarded(async () => {
    try { state = await call("save", state.settings) } finally { saveTimer = 0 }
    render()
  }), 250)
}
$("resolution").onchange = (e) => change({ resolution: e.target.value })
$("aspect").onchange = (e) => change({ aspect: e.target.value })
$("maxFps").onchange = (e) => {
  customFps = e.target.value === "custom"
  if (customFps) { render(); $("customFps").focus() } else change({ maxFps: +e.target.value })
}
$("customFps").onchange = (e) => {
  const f = Math.min(360, Math.max(30, Math.round(+e.target.value) || 60))
  e.target.value = f
  change({ maxFps: f })
}
$("msaa").onchange = (e) => change({ msaa: +e.target.value })
$("language").onchange = (e) => change({ language: +e.target.value })
$("display").onclick = (e) => { const b = e.target.closest("button"); if (b) change({ fullscreen: b.dataset.full === "1" }) }
for (const k of ["vsync", "enhance"]) $(k).onclick = () => change({ [k]: !state.settings[k] })

// ---- actions

$("launch").onclick = () => guarded(async () => {
  const status = $("status")
  $("launch").disabled = true
  status.className = "status"
  status.textContent = state.shaders.exists ? "Starting…" : "Starting: the first start compiles shaders behind the loading screen (about 3½ minutes)."
  try {
    await call("launch", { settings: current() })
    status.className = "status done"
    status.textContent = "Game started."
    setTimeout(refresh, 2500)
  } catch (e) {
    status.className = "status error"
    status.textContent = e.message
    $("launch").disabled = false
  }
})

$("shaders-delete").onclick = () => guarded(async () => {
  if (!await confirmBox("Delete the shader cache?", "The next start compiles every shader again (a few minutes behind the loading screen). Saves and settings are kept.", "Delete")) return
  state = await call("shaders.delete")
  render()
  toast("Shader cache deleted.", true)
})
$("shaders-rebuild").onclick = () => guarded(async () => {
  if (!await confirmBox("Rebuild the shader cache?", "The cache is deleted and the game starts to compile it again. Let it run: after the loading screen there can be up to a minute of black screen while pipelines build.", "Rebuild")) return
  state = await call("shaders.rebuild")
  render()
  toast("Game started: compiling shaders.", true)
})
async function addPackages(cmd, what) {
  const r = await call(cmd)
  state = r.state
  render()
  if (r.errors && r.errors.length) toast(r.errors.join(" "))
  else if (r.added) toast(`${r.added} ${what}${r.added > 1 ? "s" : ""} installed. ${r.added > 1 ? "They appear" : "It appears"} in the game next time it starts.`, true)
}
$("dlc-add").onclick = () => guarded(() => addPackages("dlc.add", "package"))
for (const b of document.querySelectorAll("[data-open]")) b.onclick = () => guarded(() => call("open", b.dataset.open))
// Settings changed elsewhere (the game's own window, an edited settings.ini): picked up when coming back here.
// Not while a dropdown is open or a change is still being saved: on Linux closing a dropdown's pop-up focuses the
// window, and the settings fetched then would put the old choice back.
window.addEventListener("focus", () => {
  if (saveTimer || ["SELECT", "INPUT"].includes(document.activeElement.tagName)) return
  guarded(async () => { state = await call("state"); render() })
})

async function checkUpdates(quiet) {
  try {
    update = await call("update.check")
    renderUpdate()
  } catch (e) {
    if (!quiet) toast("Couldn't reach GitHub: " + e.message)
    $("notes").replaceChildren(el("div", "empty", "Couldn't reach GitHub. Check your connection and try again."))
  }
}
$("update-check").onclick = () => checkUpdates(false)
$("update-page").onclick = () => guarded(() => call("url", (update && update.page) || "https://github.com/Aiden-Piercee/Forza-Motorsport-4-ReGPU1/releases"))
$("update-apply").onclick = () => guarded(async () => {
  if (!await confirmBox(`Install ${update.latest}?`, "The patch is downloaded from GitHub and installed over this install. Saves and settings are kept. The first start afterwards may rebuild the shader cache.", "Install")) return
  $("update-apply").disabled = true
  $("update-check").disabled = true
  $("progress").classList.add("on")
  $("update-status").className = "status"
  try {
    const r = await call("update.apply")
    update = r.update
    state = r.state
    render()
    renderUpdate()
    $("update-status").className = "status done"
    $("update-status").textContent = `Installed ${r.files} file${r.files === 1 ? "" : "s"}.`
    if (r.launcherChanged && await confirmBox("Restart the launcher?", "The update includes a new launcher. Restart it now to use the new version.", "Restart")) await call("restart")
  } catch (e) {
    $("update-status").className = "status error"
    $("update-status").textContent = e.message
    renderUpdate()
  } finally {
    $("update-check").disabled = false
    $("progress").classList.remove("on")
  }
})
function onProgress(d) {
  $("progress").querySelector(".bar").style.width = Math.round(d.p * 100) + "%"
  $("update-status").textContent = d.text
}

// ---- profiles: each one's saves and backups (Profiles.cs)

let profiles = null
let profileSel = null
async function loadProfiles() {
  await guarded(async () => { profiles = await call("profiles.list"); renderProfiles() })
}
function selectedProfile() { return profiles && profiles.profiles.find((p) => p.name === profileSel) }
function when(d) { return d ? new Date(d).toLocaleString() : "never" }
function renderProfiles() {
  const ps = profiles.profiles
  if (!ps.some((p) => p.name === profileSel)) profileSel = (ps.find((p) => p.active) || ps[0] || {}).name || null
  const list = $("prof-list")
  list.replaceChildren()
  if (!ps.length) list.append(el("div", "empty", "No profiles yet. Play once, or make one with New."))
  for (const p of ps) {
    const it = el("button", "item pick" + (p.name === profileSel ? " on" : ""))
    const main = el("div", "main")
    const title = el("div", "title", p.name)
    if (p.active) title.append(el("span", "badge-on", "Playing"))
    main.append(title, el("div", "meta", `${p.photos} photo${p.photos === 1 ? "" : "s"} · ${size(p.bytes)} · ${p.lastPlayed ? new Date(p.lastPlayed).toLocaleDateString() : "never played"}`))
    it.append(main)
    it.onclick = () => {
      profileSel = p.name
      renderProfiles()
      if (!p.active) profileCall("profiles.use", { name: p.name }, `Playing as ${p.name}.`).then(refresh)
    }
    list.append(it)
  }

  const p = selectedProfile()
  const running = profiles.running
  $("prof-title").textContent = p ? p.name : "Profile"
  $("prof-active").hidden = !(p && p.active)
  const info = $("prof-info")
  info.replaceChildren()
  if (p) for (const [k, v] of [["Last played", when(p.lastPlayed)], ["Photos", String(p.photos)], ["Size", size(p.bytes)], ["XUID", p.xuid]])
    info.append(el("div", "k", k), el("div", "v", v))
  for (const id of ["prof-backup", "prof-duplicate", "prof-rename", "prof-export", "prof-merge", "prof-delete"]) $(id).disabled = !p
  for (const id of ["prof-duplicate", "prof-rename", "prof-merge", "prof-delete", "prof-import"]) if (running) $(id).disabled = true
  $("prof-merge").disabled = !p || running || ps.length < 2
  $("prof-status").textContent = running ? "The game is running: close it to change profiles." : ""

  const bl = $("prof-backups")
  bl.replaceChildren()
  if (!profiles.backups.length) bl.append(el("div", "empty", "No backups yet."))
  for (const b of profiles.backups) {
    const it = el("div", "item")
    const main = el("div", "main")
    main.append(el("div", "title", b.profile), el("div", "meta", `${when(b.made)} · ${size(b.bytes)}`))
    const restore = el("button", "ghost small", "Restore")
    restore.dataset.tip = "Puts this backup back. The profile's current save is backed up first."
    restore.disabled = running
    restore.onclick = () => guarded(async () => {
      if (!await confirmBox(`Restore ${b.profile}?`, `The ${b.profile} profile goes back to this backup from ${when(b.made)}. Its current save is backed up first.`, "Restore")) return
      const r = await call("profiles.restore", { file: b.file })
      profiles = r.state; profileSel = r.name; renderProfiles()
      toast(`${r.name} restored.`, true)
    })
    it.append(main, restore)
    bl.append(it)
  }
}
async function profileCall(cmd, args, done) {
  await guarded(async () => {
    const r = await call(cmd, args)
    if (r && r.profiles) profiles = r
    else if (r && r.state) profiles = r.state
    renderProfiles()
    if (done) toast(typeof done === "function" ? done(r) : done, true)
  })
}
$("prof-new").onclick = async () => {
  const name = await askBox("New profile", "A name of up to 15 characters.", "Create", { input: "" })
  if (name) { await profileCall("profiles.create", { name }, `${name} created. Pick it in the list to play it.`); profileSel = name; renderProfiles() }
}
$("prof-backup").onclick = () => profileCall("profiles.backup", { name: profileSel }, `${profileSel} backed up.`)
$("prof-duplicate").onclick = async () => {
  const newName = await askBox(`Duplicate ${profileSel}`, "The copy's name. It gets the whole career, garage, liveries and photos.", "Duplicate", { input: profileSel.slice(0, 12) + " 2" })
  if (newName) { await profileCall("profiles.duplicate", { name: profileSel, newName }, `${newName} created.`); profileSel = newName; renderProfiles() }
}
$("prof-rename").onclick = async () => {
  const newName = await askBox(`Rename ${profileSel}`, "The new name. Its saves keep working.", "Rename", { input: profileSel })
  if (newName && newName !== profileSel) { const old = profileSel; profileSel = newName; await profileCall("profiles.rename", { name: old, newName }, `Renamed to ${newName}.`); refresh() }
}
$("prof-export").onclick = () => guarded(async () => { if (await call("profiles.export", { name: profileSel })) toast("Profile exported.", true) })
$("prof-import").onclick = () => guarded(async () => {
  const r = await call("profiles.import", {})
  if (!r) return
  profiles = r.state; profileSel = r.name; renderProfiles()
  toast(`${r.name} imported.`, true)
})
$("prof-merge").onclick = async () => {
  const others = profiles.profiles.filter((p) => p.name !== profileSel).map((p) => ({ value: p.name, name: `${p.name} (${p.photos} photo${p.photos === 1 ? "" : "s"})` }))
  const from = await askBox(`Merge photos into ${profileSel}`, "Copies another profile's saved photos into this one (the other profile keeps them too). Careers and garages can't be merged: each is one encrypted save.", "Copy photos", { options: others })
  if (from) await profileCall("profiles.mergePhotos", { from, into: profileSel }, (r) => `${r.copied} photo${r.copied === 1 ? "" : "s"} copied.`)
}
$("prof-delete").onclick = async () => {
  if (!await confirmBox(`Delete ${profileSel}?`, "Its career, garage, liveries and photos are deleted. A backup is made first, so you can restore it from the Backups list.", "Delete")) return
  await profileCall("profiles.delete", { name: profileSel }, "Deleted (a backup was kept).")
  $("prof-modal").hidden = true
  refresh()
}
$("prof-backups-open").onclick = () => guarded(() => call("profiles.openBackups"))
// ---- photos: their own window (PhotosForm.cs, photos.html)

$("photos-open").onclick = () => guarded(() => call("photos.open"))

// ---- report a bug (Reports.cs, as PGR4's launcher)

async function loadReport() {
  const r = await call("report.state")
  $("report-offline").hidden = r.configured
  $("bug-send").disabled = !r.configured
  $("share-logs").classList.toggle("on", r.shareLogs)
  $("share-status").textContent = !r.configured || !r.shareLogs ? "" : r.lastError ? "Last try didn't send: " + r.lastError
    : r.lastSent && !r.lastSent.startsWith("0001") ? "Last log sent " + new Date(r.lastSent).toLocaleString() : ""
  const log = await call("report.preview")
  // the end matters most (where it went wrong); the whole log is sent
  $("log-preview").textContent = log ? (log.length > 60000 ? "…\n" + log.slice(-60000) : log) : "No play.log yet: it's written when the game runs."
  if (log) $("log-preview").scrollTop = $("log-preview").scrollHeight
}
$("bug-log").onclick = () => $("bug-log").classList.toggle("on")
$("share-logs").onclick = () => guarded(async () => {
  const r = await call("report.shareLogs", !$("share-logs").classList.contains("on"))
  $("share-logs").classList.toggle("on", r.shareLogs)
  toast(r.shareLogs ? "Thanks! Your play.log will be sent after each session." : "Log sharing is off.", true)
})
$("bug-send").onclick = () => guarded(async () => {
  $("bug-send").disabled = true
  $("bug-status").textContent = "Sending…"
  try {
    const r = await call("report.send", { title: $("bug-title").value, description: $("bug-text").value, contact: $("bug-contact").value, withLog: $("bug-log").classList.contains("on") })
    $("bug-title").value = $("bug-text").value = ""
    $("bug-status").textContent = `Sent: report ${r.id}. Thanks!`
    toast("Report sent. Thanks!", true)
  } catch (e) {
    $("bug-status").textContent = ""
    throw e
  } finally {
    $("bug-send").disabled = false
  }
})

// First start: log sharing is on unless turned off, so say so before anything is sent (once there's a server to send to).
async function sharingNotice() {
  const r = await call("report.state")
  if (r.noticeShown || !r.configured) return
  // Shown is what counts: closing the launcher with the box up mustn't keep sharing off for good
  const asked = askBox("Sharing play.log",
    "To help find bugs on PCs like yours, the launcher sends each session's play.log to the developers: your hardware, drivers, settings, frame rates and errors, and the list of graphics pipelines the game built (hashes only, so later versions can build them on the first start). Your Windows user name and PC name are removed first. " +
    "This is on by default. To opt out, open the Report tab and switch off \"Share play.log after each session\".",
    "Got it", { no: "Open Report tab" })
  await call("report.noticeShown")
  if ((await asked) === null) document.querySelector("[data-tab=report]").click()
}

async function refresh() {
  await guarded(async () => { state = await call("state"); render(); renderUpdate() })
}

refresh().then(() => guarded(sharingNotice)).then(() => checkUpdates(true))
setInterval(() => { if (!document.hidden && $("confirm").hidden && !["SELECT", "INPUT"].includes(document.activeElement.tagName)) refresh() }, 5000)
