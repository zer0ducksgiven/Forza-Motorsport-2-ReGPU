"use strict"
// The Photos window (PhotosForm.cs): Photo Mode saves, read straight from the profiles folder.

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
function size(bytes) {
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
    let x = Math.max(8, Math.min(r.left + Math.min(r.width / 2, 120) - 20, innerWidth - tip.offsetWidth - 8))
    let y = r.bottom + 8
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

let photos = []
let photoId = null
async function loadPhotos() {
  await guarded(async () => {
    photos = await call("photos.list")
    if (!photos.some((p) => p.id === photoId)) photoId = photos.length ? photos[0].id : null
    renderPhotos()
  })
}
function renderPhotos() {
  $("photo-count").textContent = photos.length ? String(photos.length) : ""
  const t = $("thumbs")
  t.replaceChildren()
  if (!photos.length) t.append(el("div", "empty", "Nothing saved yet."))
  for (const p of photos) {
    const b = el("button", "thumb" + (p.id === photoId ? " on" : ""))
    const img = el("img")
    img.src = p.url
    img.loading = "lazy"
    img.alt = p.name
    b.append(img, el("span", "", p.name))
    b.onclick = () => { photoId = p.id; renderPhotos() }
    t.append(b)
  }
  const p = photos.find((x) => x.id === photoId)
  const v = $("viewer")
  if (p) {
    let img = v.querySelector("img")
    if (!img) { img = el("img"); v.replaceChildren(img) }
    if (img.src !== p.url) img.src = p.url
    img.alt = p.name
    $("photo-title").textContent = p.name
    $("photo-meta").textContent = `${p.profile} · ${new Date(p.taken).toLocaleString()} · ${size(p.bytes)}`
  } else {
    v.replaceChildren(el("div", "empty", "No photos yet. In a race, pause and pick Photo Mode, take a shot and save it."))
    $("photo-title").textContent = "Photos"
    $("photo-meta").textContent = ""
  }
  for (const k of ["photo-copy", "photo-save", "photo-reveal"]) $(k).disabled = !p
}
function stepPhoto(d) {
  const i = photos.findIndex((p) => p.id === photoId)
  if (i < 0) return
  const n = photos[Math.max(0, Math.min(photos.length - 1, i + d))]
  if (n && n.id !== photoId) { photoId = n.id; renderPhotos(); document.querySelector(".thumb.on")?.scrollIntoView({ block: "nearest" }) }
}
document.addEventListener("keydown", (e) => {
  if (e.key === "ArrowLeft" || e.key === "ArrowUp") { stepPhoto(-1); e.preventDefault() }
  if (e.key === "ArrowRight" || e.key === "ArrowDown") { stepPhoto(1); e.preventDefault() }
  if (e.key === "c" && e.ctrlKey && photoId) $("photo-copy").click()
})
// The photo as PNG, drawn here: the same on Windows and Linux, no image library in the launcher.
async function pngOf(id) {
  const img = new Image()
  img.src = photos.find((p) => p.id === id).url
  await img.decode()
  const c = document.createElement("canvas")
  c.width = img.naturalWidth
  c.height = img.naturalHeight
  c.getContext("2d").drawImage(img, 0, 0)
  return new Promise((resolve, reject) => c.toBlob((b) => b ? resolve(b) : reject(new Error("Couldn't convert the photo.")), "image/png"))
}
async function base64Of(blob) {
  const bytes = new Uint8Array(await blob.arrayBuffer())
  let s = ""
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000))
  return btoa(s)
}
$("photo-copy").onclick = () => guarded(async () => {
  // The page's clipboard where the web view allows it; otherwise the launcher puts it there (wl-copy or xclip on Linux).
  try { await navigator.clipboard.write([new ClipboardItem({ "image/png": pngOf(photoId) })]) }
  catch { await call("photos.copy", await base64Of(await pngOf(photoId))) }
  toast("Photo copied to the clipboard.", true)
})
$("photo-save").onclick = () => guarded(async () => {
  const id = photoId
  const file = await call("photos.saveAs", id)
  if (!file) return
  await call("photos.write", { id, file, png: file.toLowerCase().endsWith(".png") ? await base64Of(await pngOf(id)) : null })
  toast("Photo saved.", true)
})
$("photo-reveal").onclick = () => guarded(() => call("photos.reveal", photoId))
$("photos-folder").onclick = () => guarded(() => call("photos.reveal", null))
// New photos taken while the window is open show when it's focused again.
window.addEventListener("focus", loadPhotos)
loadPhotos()
