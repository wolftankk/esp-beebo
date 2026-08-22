/*
 * One spoken turn, in three steps, shared by both front doors: the legacy
 * HTTP endpoint and the WebSocket relay.
 */
const http = require("http");
const https = require("https");
const { GatewayClient } = require("./gateway-client");

/* Speech in and speech out are both called directly here rather than through
 * any service. An earlier arrangement borrowed the agent host's own text-to-
 * speech; that belonged to the agent, was wired into its environment, and
 * meant this proxy broke whenever that got reconfigured. Owning the whole
 * audio path costs a few lines and removes a dependency that was never ours. */
const DASHSCOPE_URL = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
const DASHSCOPE_KEY = process.env.DASHSCOPE_API_KEY || "";
const ASR_MODEL     = process.env.ASR_MODEL || "qwen3-asr-flash";
const TTS_MODEL     = process.env.TTS_MODEL || "qwen3-tts-flash";
const TTS_VOICE     = process.env.TTS_VOICE || "Dylan";
const ASR_LANGUAGE  = process.env.ASR_LANGUAGE || "zh";
/* "Auto" lets the synthesiser follow whatever the agent wrote, which is what
 * you want unless you are pinning it to one language. */
const TTS_LANGUAGE  = process.env.TTS_LANGUAGE || "Auto";
const SESSION       = process.env.VOICE_SESSION_KEY || "beebo";

/* The agent answers a spoken question the way it answers a typed one: markdown
 * headings, tables and emoji. Measured here, one such reply cost 18 s to
 * generate and 14 s to synthesise - the length of the text, not the speed of
 * any service, is what makes a voice turn feel slow.
 *
 * Appended rather than prepended so it reads as a directive about the turn
 * rather than as something the user said. */
const VOICE_STYLE = process.env.VOICE_STYLE ||
  "\n\n[Spoken conversation: answer in one or two conversational sentences. " +
  "No markdown, headings, tables, lists or emoji. If the answer needs more, " +
  "give the conclusion first and offer to go into detail.]";

/* Said in place of the tail of an answer that ran past TTS_MAX_CHARS. */
const TTS_MORE = process.env.TTS_MORE || " There is more - want me to go on?";

const TTS_MAX_CHARS = parseInt(process.env.TTS_MAX_CHARS || "260", 10);

const gw = new GatewayClient();
gw.start();

function request(url, { method = "POST", headers = {}, body }) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const req = http.request(
      { hostname: u.hostname, port: u.port, path: u.pathname + u.search, method, headers },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () =>
          resolve({ status: res.statusCode, buf: Buffer.concat(chunks) }));
      }
    );
    req.on("error", reject);
    if (body) req.write(body);
    req.end();
  });
}

function dashscope(body) {
  return new Promise((resolve, reject) => {
    const payload = JSON.stringify(body);
    const req = https.request(
      new URL(DASHSCOPE_URL),
      { method: "POST",
        headers: {
          Authorization: `Bearer ${DASHSCOPE_KEY}`,
          "Content-Type": "application/json",
          "Content-Length": Buffer.byteLength(payload),
        } },
      (res) => {
        let data = "";
        res.on("data", (c) => (data += c));
        res.on("end", () => {
          try { resolve({ status: res.statusCode, data: JSON.parse(data) }); }
          catch { resolve({ status: res.statusCode, data: { raw: data } }); }
        });
      }
    );
    req.on("error", reject);
    req.write(payload);
    req.end();
  });
}

async function transcribe(wav) {
  if (!DASHSCOPE_KEY) throw new Error("DASHSCOPE_API_KEY is not set");
  /* Inline data URL rather than an uploaded file: the clips are seconds long
   * and DashScope accepts up to 10 MB once base64'd. */
  const res = await dashscope({
    model: ASR_MODEL,
    input: { messages: [{ role: "user", content: [
      { audio: `data:audio/wav;base64,${wav.toString("base64")}` } ] }] },
    parameters: { asr_options: { language: ASR_LANGUAGE, enable_itn: true } },
  });
  if (res.status !== 200 || res.data.code)
    throw new Error("asr: " + JSON.stringify(res.data).slice(0, 160));

  const parts = res.data?.output?.choices?.[0]?.message?.content || [];
  return parts.map((p) => p.text || "").join("").trim();
}

/* There used to be a fallback that shelled out to `openclaw agent`. It made
 * sense while this ran on the OpenClaw host; here that binary does not exist,
 * and a fallback that cannot work only disguises connection problems as
 * something else. Failing plainly is easier to diagnose. */
async function ask(text, onDelta = null, onSpeak = null) {
  if (!gw.ready) throw new Error("gateway not connected");
  return gw.chat(text + VOICE_STYLE, SESSION, 180000, onDelta, onSpeak);
}

/* Whatever the model writes still has to be read aloud, so strip what only
 * makes sense on a screen. */
function speakable(text, truncate = true) {
  let t = text
    .replace(/```[\s\S]*?```/g, " ")
    .replace(/^\s*\|.*\|\s*$/gm, " ")
    .replace(/^\s*[-*+]\s+/gm, "")
    .replace(/^#{1,6}\s*/gm, "")
    .replace(/^\s*[-=]{3,}\s*$/gm, " ")
    .replace(/[*_`>|#]/g, "")
    .replace(/\p{Extended_Pictographic}/gu, "")
    /* Newlines become a sentence break so the synthesiser pauses where the
     * text meant to. Which mark depends on the script - a full-width stop in
     * English reads as a glitch, and a full stop in Chinese is a narrower
     * pause than intended. */
    .replace(/\s*\n\s*/g, /[\u4e00-\u9fff]/.test(text) ? "。" : ". ")
    .replace(/。{2,}/g, "。")
    .replace(/\.\s*(\.\s*)+/g, ". ")
    .replace(/\s{2,}/g, " ")
    .trim();
  if (truncate && t.length > TTS_MAX_CHARS) {
    const cut = t.slice(0, TTS_MAX_CHARS);
    const stop = Math.max(cut.lastIndexOf("。"), cut.lastIndexOf("！"), cut.lastIndexOf("？"),
                          cut.lastIndexOf(". "), cut.lastIndexOf("! "), cut.lastIndexOf("? "));
    t = (stop > TTS_MAX_CHARS / 2 ? cut.slice(0, stop + 1) : cut) + TTS_MORE;
  }
  return t;
}

function fetchBuffer(url) {
  return new Promise((resolve, reject) => {
    const u = new URL(url);
    const mod = u.protocol === "https:" ? https : http;
    mod.get(u, (res) => {
      const chunks = [];
      res.on("data", (c) => chunks.push(c));
      res.on("end", () => resolve(Buffer.concat(chunks)));
    }).on("error", reject);
  });
}

/* Also called directly rather than through OpenClaw's own TTS service. That
 * one belongs to OpenClaw - it is wired into the gateway's environment and the
 * agent uses it as a tool - and borrowing it would mean this proxy breaks
 * whenever that gets reconfigured. Owning the whole audio path costs a few
 * lines and removes a dependency that was never ours. */
/* Streaming synthesis. The service will hand back a whole clip and a URL to
 * fetch it from, but it will also stream the same audio over SSE, and measured
 * against each other the first bytes arrive in ~250 ms where the finished file
 * takes 500-1000. For a robot that has already made you wait for an agent to
 * think, that difference is the difference between answering and pausing.
 *
 * The stream is one WAV: the first chunk carries the RIFF header with a
 * placeholder length, everything after it is more of the same data chunk. */
function synthesizeStream(text, onChunk) {
  return new Promise((resolve, reject) => {
    if (!DASHSCOPE_KEY) return reject(new Error("DASHSCOPE_API_KEY is not set"));
    const body = JSON.stringify({
      model: TTS_MODEL,
      input: { text, voice: TTS_VOICE, language_type: TTS_LANGUAGE },
    });
    const u = new URL(DASHSCOPE_URL);
    const req = https.request({
      hostname: u.hostname, path: u.pathname, method: "POST",
      headers: {
        "Content-Type": "application/json",
        Authorization: "Bearer " + DASHSCOPE_KEY,
        "X-DashScope-SSE": "enable",
        "Content-Length": Buffer.byteLength(body),
      },
    }, (res) => {
      if (res.statusCode !== 200) {
        res.resume();
        return reject(new Error(`tts stream: HTTP ${res.statusCode}`));
      }
      let buf = "", total = 0;
      res.on("data", (c) => {
        buf += c.toString();
        let i;
        while ((i = buf.indexOf("\n\n")) >= 0) {
          const ev = buf.slice(0, i);
          buf = buf.slice(i + 2);
          const line = ev.split("\n").find((l) => l.startsWith("data:"));
          if (!line) continue;
          let j; try { j = JSON.parse(line.slice(5).trim()); } catch { continue; }
          const d = j.output && j.output.audio && j.output.audio.data;
          if (!d) continue;
          const b = Buffer.from(d, "base64");
          total += b.length;
          onChunk(b);
        }
      });
      res.on("end", () => (total ? resolve(total) : reject(new Error("tts stream: no audio"))));
      res.on("error", reject);
    });
    req.on("error", reject);
    req.end(body);
  });
}

async function synthesize(text) {
  if (!DASHSCOPE_KEY) throw new Error("DASHSCOPE_API_KEY is not set");
  const res = await dashscope({
    model: TTS_MODEL,
    input: { text, voice: TTS_VOICE, language_type: TTS_LANGUAGE },
  });
  if (res.status !== 200 || res.data.code)
    throw new Error("tts: " + JSON.stringify(res.data).slice(0, 160));

  const url = res.data?.output?.audio?.url;
  if (!url) throw new Error("tts: no audio url in response");
  return fetchBuffer(url);
}

/* PCM from the board arrives headerless; the services want a RIFF file. */
function wrapWav(pcm, rate = 24000) {
  const h = Buffer.alloc(44);
  h.write("RIFF", 0);
  h.writeUInt32LE(36 + pcm.length, 4);
  h.write("WAVEfmt ", 8);
  h.writeUInt32LE(16, 16);
  h.writeUInt16LE(1, 20);
  h.writeUInt16LE(1, 22);
  h.writeUInt32LE(rate, 24);
  h.writeUInt32LE(rate * 2, 28);
  h.writeUInt16LE(2, 32);
  h.writeUInt16LE(16, 34);
  h.write("data", 36);
  h.writeUInt32LE(pcm.length, 40);
  return Buffer.concat([h, pcm]);
}

module.exports = {
  /* Null when a turn can run; otherwise why it cannot, phrased to be spoken. */
  unavailableReason: () => gw.unavailableReason, synthesizeStream, fetchBuffer, gw, transcribe, ask, speakable, synthesize, wrapWav, SESSION };
