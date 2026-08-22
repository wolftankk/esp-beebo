/*
 * Persistent operator connection to the OpenClaw gateway.
 *
 * Spawning `openclaw agent` per turn cost about 2.8 s of node startup before
 * the model even saw the question - roughly a third of the round trip. Holding
 * one authenticated socket open removes that entirely.
 *
 * This service pairs under its own device identity rather than borrowing the
 * CLI's: reusing that identity trips a "metadata-upgrade" re-approval, and
 * approving it would rewrite the CLI's own pairing record. A separate identity
 * is also separately revocable.
 *
 * Handshake per docs/gateway-protocol.md:
 *   v2|deviceId|clientId|clientMode|role|scopes|signedAtMs|token|nonce
 * signed with Ed25519, deviceId = sha256(raw public key).
 */
const fs = require("fs"), os = require("os"), path = require("path"), crypto = require("crypto");

const STATE_DIR = process.env.PROXY_STATE_DIR || path.join(__dirname, ".state");
const IDENTITY  = path.join(STATE_DIR, "identity.json");
/* Running off-box now, so there is no OpenClaw config to read the bootstrap
 * token out of - it comes from the environment like every other secret. */
/* Defaults to the same machine. Point GATEWAY_URL at wherever the agent's
 * gateway actually listens; nothing real is committed here. */
/* Accepts "host:port" as well as a full URL. A bare host:port is the natural
 * thing to write in a .env and it produced a WebSocket constructor throw that
 * the retry loop swallowed - the proxy then retried a malformed address every
 * five seconds forever, silently, and every turn failed with "gateway not
 * connected" while the host was perfectly reachable. */
function gatewayUrl(raw) {
  const v = (raw || "").trim();
  if (!v) return "ws://127.0.0.1:18789";
  if (/^wss?:\/\//.test(v)) return v;
  if (/^https?:\/\//.test(v)) return v.replace(/^http/, "ws");
  console.warn(`[GW] GATEWAY_URL "${v}" has no scheme - reading it as ws://${v}`);
  return "ws://" + v;
}

const GW_URL    = gatewayUrl(process.env.GATEWAY_URL);
const SCOPES    = ["operator.read", "operator.write"];

function readGatewayToken() { return process.env.OPENCLAW_GATEWAY_TOKEN || ""; }

function loadIdentity() {
  try { return JSON.parse(fs.readFileSync(IDENTITY, "utf8")); } catch {}
  const { publicKey, privateKey } = crypto.generateKeyPairSync("ed25519");
  const raw = publicKey.export({ type: "spki", format: "der" }).slice(-32);
  const id = {
    version: 1,
    deviceId: crypto.createHash("sha256").update(raw).digest("hex"),
    publicKeyRaw: raw.toString("base64url"),
    privateKeyPem: privateKey.export({ type: "pkcs8", format: "pem" }),
    deviceToken: "",
  };
  fs.mkdirSync(STATE_DIR, { recursive: true });
  fs.writeFileSync(IDENTITY, JSON.stringify(id, null, 2), { mode: 0o600 });
  console.log("[GW] generated a new device identity");
  return id;
}

class GatewayClient {
  constructor() {
    this.id = loadIdentity();
    this.key = crypto.createPrivateKey(this.id.privateKeyPem);
    this.ready = false;
    this.seq = 100;
    this.waiters = new Map();
    this.runs = new Map();
    this.traceEvents = process.env.GW_TRACE === "1";
  }

  get paired() { return !!this.id.deviceToken; }

  start() { this._connect(); }

  /* Repeated identical failures are logged once, then every tenth time. The
   * failure mode here is a loop that runs for hours, and a line every three
   * seconds buries everything else in the log. */
  _complain(what) {
    if (what === this._lastGripe) {
      if (++this._gripes % 10) return;
      console.error(`[GW] ${what} (still, x${this._gripes})`);
      return;
    }
    this._lastGripe = what;
    this._gripes = 1;
    console.error(`[GW] ${what}`);
  }

  /* Why a turn cannot run right now, in words the robot can say. */
  get unavailableReason() {
    return this.ready ? null : `I cannot reach the agent at ${GW_URL} right now.`;
  }

  _persist() {
    fs.writeFileSync(IDENTITY, JSON.stringify(this.id, null, 2), { mode: 0o600 });
  }

  _connect() {
    this.ready = false;
    let ws;
    try {
      ws = new WebSocket(GW_URL);
    } catch (e) {
      /* Said out loud rather than swallowed: this is the branch a bad address
       * lands in, and it used to retry in silence. */
      this._complain(`cannot open ${GW_URL}: ${e.message}`);
      return setTimeout(() => this._connect(), 5000);
    }
    this.ws = ws;

    ws.onmessage = (ev) => {
      let m; try { m = JSON.parse(ev.data); } catch { return; }
      if (m.event === "connect.challenge") return this._answerChallenge(m.payload);
      if (m.type === "res" && m.id === "connect") return this._onConnectResult(m);
      if (m.type === "res" && this.waiters.has(m.id)) {
        const w = this.waiters.get(m.id);
        this.waiters.delete(m.id);
        clearTimeout(w.timer);
        w.resolve(m);
        return;
      }
      if (m.type === "event" && m.event === "chat" && m.payload) {
        const w = this.runs.get(m.payload.runId);
        if (w && m.payload.state === "delta") {
          /* Deltas carry the text so far, not just the increment. Passing them
           * on is the whole point: the agent is already streaming, and waiting
           * for the final event throws that away. */
          const t = (m.payload.message?.content || []).map((c) => c.text || "").join("");
          if (t) { w.partial = t; if (w.onDelta) w.onDelta(t); }
        }
        if (w && m.payload.state === "final") {
          const text = (m.payload.message?.content || [])
            .map((c) => c.text || "").join("").trim();
          this.runs.delete(m.payload.runId);
          clearTimeout(w.timer);
          if (w.ending) clearTimeout(w.ending);
          w.resolve(text || w.partial || "");
        }
        return;
      }
      /* A run that ends without a final chat event would otherwise hang the
       * caller until its timeout; treat the lifecycle end as a backstop. */
      if (m.type === "event" && m.event === "agent" &&
          m.payload?.stream === "lifecycle" && m.payload?.data?.phase === "end") {
        const w = this.runs.get(m.payload.runId);
        if (w && !w.ending) {
          /* The final chat event lands just after this one, so give it a
           * moment to win rather than resolving with an empty string. */
          w.ending = setTimeout(() => {
            if (!this.runs.has(m.payload.runId)) return;
            this.runs.delete(m.payload.runId);
            clearTimeout(w.timer);
            w.resolve(w.partial || "");
          }, 1500);
        }
        return;
      }
      if (m.type === "event" && this.traceEvents) {
        console.log("[GW] event", m.event, JSON.stringify(m.payload || {}).slice(0, 300));
      }
    };
    ws.onclose = () => {
      const wasReady = this.ready;
      this.ready = false;
      for (const [, w] of this.runs) { clearTimeout(w.timer); }
      this.runs.clear();
      if (wasReady) console.warn(`[GW] disconnected from ${GW_URL} - reconnecting`);
      else this._complain(`connection to ${GW_URL} closed before it was ready`);
      setTimeout(() => this._connect(), 3000);
    };
    ws.onerror = (e) => this._complain(`${GW_URL}: ${e && e.message ? e.message : "connection error"}`);
  }

  _answerChallenge({ nonce, ts }) {
    // Sign against the gateway's clock so the proof is inside its skew window.
    const token = this.paired ? this.id.deviceToken : readGatewayToken();
    const canon = ["v2", this.id.deviceId, "cli", "cli", "operator",
                   SCOPES.join(","), String(ts), token, nonce].join("|");
    const sig = crypto.sign(null, Buffer.from(canon), this.key);
    const auth = this.paired ? { deviceToken: token } : { token };
    this.ws.send(JSON.stringify({
      type: "req", id: "connect", method: "connect",
      params: {
        minProtocol: 4, maxProtocol: 4,
        client: { id: "cli", version: "voice-turn", platform: "darwin", mode: "cli" },
        role: "operator", scopes: SCOPES, caps: [],
        auth,
        device: {
          id: this.id.deviceId,
          publicKey: this.id.publicKeyRaw,
          signature: Buffer.from(sig).toString("base64url"),
          signedAt: ts,
          nonce,
        },
      },
    }));
  }

  _onConnectResult(m) {
    if (m.ok) {
      const tok = m.payload?.auth?.deviceToken;
      if (tok && tok !== this.id.deviceToken) { this.id.deviceToken = tok; this._persist(); }
      this.ready = true;
      console.log("[GW] connected (persistent operator session)");
      return;
    }
    const d = m.error?.details || {};
    if (d.code === "PAIRING_REQUIRED") {
      console.log(`[GW] PAIRING REQUIRED - approve this service once:`);
      console.log(`[GW]   openclaw devices approve ${d.requestId}`);
      console.log(`[GW]   (deviceId ${this.id.deviceId})`);
      console.log(`[GW] until then, turns fall back to the openclaw CLI`);
    } else {
      console.log("[GW] connect refused:", JSON.stringify(m.error).slice(0, 300));
      if (this.paired) {           // stale token: drop it and bootstrap again
        this.id.deviceToken = "";
        this._persist();
      }
    }
    try { this.ws.close(); } catch {}
  }

  /* chat.send only acknowledges with a runId - the answer streams back as
   * chat events, so the wait belongs here rather than in every caller. */
  /* onDelta receives the reply text as it grows. Without it this behaves as
   * before and simply resolves with the finished reply. */
  async chat(message, sessionKey, timeoutMs = 180000, onDelta = null) {
    /* The `agent` method rather than `chat.send`, because only this one takes
     * `thinking`. Left at its default the model spent fifteen of a sixteen
     * second turn reasoning before writing a word, which no amount of
     * streaming can hide. The CLI has always passed "off"; switching to
     * chat.send quietly dropped it. */
    const res = await this.request("agent", {
      message, sessionKey,
      thinking: process.env.AGENT_THINKING || "off",
      idempotencyKey: require("crypto").randomUUID(),
    }, 30000);
    if (!res.ok) throw new Error("agent: " + JSON.stringify(res.error).slice(0, 200));

    /* It may answer outright, or acknowledge with a runId and stream the rest
     * back as chat events. */
    const direct = (res.payload?.result?.payloads || res.payload?.payloads || [])
      .map((p) => p.text || "").join("\n").trim();
    const runId = res.payload?.runId || res.payload?.result?.runId;
    if (direct && !runId) return direct;
    if (!runId) throw new Error("agent returned neither text nor a runId");

    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.runs.delete(runId);
        reject(new Error("agent run timed out"));
      }, timeoutMs);
      this.runs.set(runId, { resolve, timer, partial: "", onDelta });
    });
  }

  request(method, params, timeoutMs = 180000) {
    return new Promise((resolve, reject) => {
      if (!this.ready) return reject(new Error("gateway not ready"));
      const id = String(this.seq++);
      const timer = setTimeout(() => {
        this.waiters.delete(id);
        reject(new Error(method + " timed out"));
      }, timeoutMs);
      this.waiters.set(id, { resolve, timer });
      this.ws.send(JSON.stringify({ type: "req", id, method, params }));
    });
  }
}

/* The reply can sit in a few shapes depending on how the turn completed, so
 * pull the deepest text payloads rather than betting on one path. */
function extractReply(res) {
  const out = [];
  (function walk(o, key) {
    if (!o) return;
    if (Array.isArray(o)) return o.forEach((v) => walk(v, key));
    if (typeof o === "object") {
      for (const k of Object.keys(o)) walk(o[k], k);
      return;
    }
    if (typeof o === "string" && key === "text" && o.trim()) out.push(o.trim());
  })(res.payload ?? res, "");
  return out.join("\n").trim();
}

module.exports = { GatewayClient, extractReply };
