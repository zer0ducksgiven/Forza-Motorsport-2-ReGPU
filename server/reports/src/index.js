// FM2 reports server: the launcher's bug reports and opted-in play.logs, filed in a private GitHub repo.
//   POST /v1/report  { kind: "bug" | "log", title?, description?, contact?, version, installId, system, log? }
//   log: gzip, base64. Bug reports become issues; logs are stored as files (logs/<date>/...).
// Secrets: GITHUB_TOKEN (fine-grained: Contents and Issues read/write on REPORTS_REPO only).
// Vars: REPORTS_REPO ("owner/name"). Binding: LIMITER (rate limit per client address).

const MAX_BODY = 6 << 20; // the launcher sends at most a 4 MB log, compressed
const MAX_LOG = 8 << 20; // decompressed

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "GET" && url.pathname === "/") return json({ ok: true });
    if (request.method !== "POST" || url.pathname !== "/v1/report") return json({ error: "Not found." }, 404);
    if (!(request.headers.get("user-agent") || "").startsWith("FM2Launcher/")) return json({ error: "Reports come from the FM2 launcher." }, 403);

    const ip = request.headers.get("cf-connecting-ip") || "local";
    if (env.LIMITER) {
      const { success } = await env.LIMITER.limit({ key: ip });
      if (!success) return json({ error: "Too many reports from here: try again in a few minutes." }, 429);
    }
    const length = Number(request.headers.get("content-length") || 0);
    if (length > MAX_BODY) return json({ error: "That report is too big." }, 413);

    let r;
    try {
      const text = await request.text();
      if (text.length > MAX_BODY) return json({ error: "That report is too big." }, 413);
      r = JSON.parse(text);
    } catch {
      return json({ error: "That report couldn't be read." }, 400);
    }
    if (r.kind !== "bug" && r.kind !== "log") return json({ error: "Unknown report kind." }, 400);

    let log = null;
    if (typeof r.log === "string" && r.log.length) {
      try {
        log = redact(await gunzip(r.log));
      } catch (e) {
        return json({ error: e.message === "too big" ? "The log is too big." : "The log couldn't be read." }, 400);
      }
    }
    if (r.kind === "log" && !log) return json({ error: "No log." }, 400);

    const id = `${new Date().toISOString().slice(0, 10).replaceAll("-", "")}-${crypto.randomUUID().slice(0, 8)}`;
    const installId = clean(r.installId, 16).replace(/[^a-z0-9]/gi, "") || "unknown";
    const version = clean(r.version, 24).replace(/[^0-9a-z.\-]/gi, "") || "unknown";
    const gh = github(env);

    try {
      let logPath = null;
      if (log) {
        logPath = `logs/${id.slice(0, 8)}/${installId}-${id.slice(9)}.log`;
        await gh(`/contents/${logPath}`, "PUT", {
          message: `${r.kind === "bug" ? "Bug report" : "Log"} ${id} (${version})`,
          content: base64(log),
        });
      }
      if (r.kind === "bug") {
        const title = clean(redact(r.title || ""), 120) || "(no title)";
        const issue = await gh("/issues", "POST", {
          title,
          body: issueBody(r, id, version, installId, logPath, env),
          labels: ["bug report", `v${version}`],
        });
        return json({ id: `#${issue.number}` });
      }
      return json({ id });
    } catch (e) {
      console.log("github", e.message);
      // GitHub's status only (401: token, 403/404: its repo or permissions), never its text.
      const status = /: (\d{3}) /.exec(e.message)?.[1] || "unreachable";
      return json({ error: `The reports server couldn't save it (GitHub ${status}): try again later.` }, 502);
    }
  },
};

function json(o, status = 200) {
  return new Response(JSON.stringify(o), { status, headers: { "content-type": "application/json" } });
}

function clean(s, max) {
  return typeof s === "string" ? s.replace(/[\u0000-\u0008\u000b-\u001f]/g, "").trim().slice(0, max) : "";
}

// The launcher redacts before sending; this catches older launchers and anything it missed.
function redact(text) {
  return text
    .replace(/([A-Za-z]:[\\/](?:Users|Documents and Settings)[\\/])[^\\/\r\n"]+/gi, "$1<user>")
    .replace(/((?:[A-Za-z]:)?[\\/]home[\\/])[^\\/\r\n"]+/gi, "$1<user>");
}

async function gunzip(b64) {
  const bytes = Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  const stream = new Blob([bytes]).stream().pipeThrough(new DecompressionStream("gzip"));
  const reader = stream.getReader();
  const parts = [];
  let total = 0;
  for (;;) {
    const { done, value } = await reader.read();
    if (done) break;
    total += value.length;
    if (total > MAX_LOG) throw new Error("too big");
    parts.push(value);
  }
  return new TextDecoder().decode(await new Blob(parts).arrayBuffer());
}

function base64(text) {
  const bytes = new TextEncoder().encode(text);
  let s = "";
  for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode(...bytes.subarray(i, i + 0x8000));
  return btoa(s);
}

function github(env) {
  return async (path, method, body) => {
    // GITHUB_API: a local stand-in for testing (.dev.vars)
    const res = await fetch(`${env.GITHUB_API || "https://api.github.com"}/repos/${env.REPORTS_REPO}${path}`, {
      method,
      headers: {
        authorization: `Bearer ${env.GITHUB_TOKEN}`,
        accept: "application/vnd.github+json",
        "x-github-api-version": "2022-11-28",
        "user-agent": "fm2-reports",
        "content-type": "application/json",
      },
      body: JSON.stringify(body),
    });
    if (!res.ok) throw new Error(`${method} ${path}: ${res.status} ${(await res.text()).slice(0, 300)}`);
    return res.json();
  };
}

function issueBody(r, id, version, installId, logPath, env) {
  const s = r.system && typeof r.system === "object" ? r.system : {};
  const row = (k, v) => (v ? `| ${k} | ${clean(String(v), 200).replace(/\|/g, "/")} |\n` : "");
  const quote = (t) => clean(redact(t || ""), 8000).replace(/^/gm, "> ");
  return (
    `${quote(r.description)}\n\n` +
    `| | |\n|---|---|\n` +
    row("Version", version) +
    row("OS", s.os) +
    row("GPU", s.gpu) +
    row("GPU driver", s.driver) +
    row("Zero-copy", s.zeroCopy === undefined ? "" : s.zeroCopy ? "yes" : "no") +
    row("Install", installId) +
    row("Contact", clean(r.contact, 64)) +
    row("Report", id) +
    `\n` +
    (logPath ? `play.log: [${logPath}](https://github.com/${env.REPORTS_REPO}/blob/main/${logPath})\n` : "No play.log attached.\n")
  );
}
