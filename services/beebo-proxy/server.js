#!/usr/bin/env node
require("./env");
/**
 * Voice turn service — the board's only server.
 *
 * Two front doors onto the same pipeline:
 *   POST /v1/voice   audio in, audio out. One request per turn. Kept because
 *                    it is simple and easy to test with curl.
 *   WS   /ws         the board's live connection: audio up, audio and text
 *                    down, and messages the server sends unprompted.
 *
 * Everything openclaw-specific lives behind one persistent operator socket
 * held here, so the board needs no gateway credential of its own.
 */
const http = require("http");
const { attach, notifyAll, clientCount } = require("./relay");
const { transcribe, ask, speakable, synthesize, gw, SESSION } = require("./pipeline");

const PORT = parseInt(process.env.VOICE_PORT || "18797", 10);
const MAX_BYTES = 9 * 1024 * 1024;

/* Headers must be latin1-safe and the replies are Chinese. */
const enc = (s) => encodeURIComponent(s).slice(0, 900);

async function readBody(req, limit) {
  const chunks = [];
  let size = 0;
  for await (const c of req) {
    size += c.length;
    if (size > limit) { req.destroy(); throw new Error("body too large"); }
    chunks.push(c);
  }
  return Buffer.concat(chunks);
}

const server = http.createServer(async (req, res) => {
  const json = (code, obj) => {
    res.writeHead(code, { "Content-Type": "application/json" });
    res.end(JSON.stringify(obj));
  };

  if (req.method === "GET" && req.url === "/health")
    return json(200, { ok: true, session: SESSION, gateway: gw.ready, boards: clientCount() });

  /* Push: anything that can make an HTTP request can now make the robot
   * speak - a cron job, a hook, a shell one-liner. */
  if (req.method === "POST" && req.url.startsWith("/v1/notify")) {
    try {
      const body = JSON.parse((await readBody(req, 64 * 1024)).toString());
      const text = (body.text || "").trim();
      if (!text) return json(400, { error: "text required" });
      const n = notifyAll(text);
      console.log(`[notify] -> ${n} board(s): ${JSON.stringify(text).slice(0, 80)}`);
      return json(200, { delivered: n });
    } catch (e) {
      return json(400, { error: String(e.message) });
    }
  }

  if (req.method === "POST" && req.url.startsWith("/v1/voice")) {
    const t0 = Date.now();
    try {
      const wav = await readBody(req, MAX_BYTES);
      if (wav.length < 1024) return json(400, { error: "audio too short" });

      const heard = await transcribe(wav);
      const tAsr = Date.now();
      if (!heard) return json(422, { error: "nothing recognised" });

      const reply = await ask(heard);
      const tAgent = Date.now();

      const spoken = speakable(reply);
      const audio = await synthesize(spoken);
      const tTts = Date.now();

      console.log(
        `[voice] ${new Date().toISOString()} ${wav.length}B ` +
        `asr=${tAsr - t0}ms agent=${tAgent - tAsr}ms tts=${tTts - tAgent}ms ` +
        `heard=${JSON.stringify(heard).slice(0, 60)} ` +
        `reply=${reply.length}ch spoken=${spoken.length}ch`
      );

      res.writeHead(200, {
        "Content-Type": "audio/wav",
        "Content-Length": audio.length,
        "X-Heard": enc(heard),
        "X-Reply": enc(reply),
        "X-Timing": `asr=${tAsr - t0},agent=${tAgent - tAsr},tts=${tTts - tAgent}`,
      });
      return res.end(audio);
    } catch (err) {
      console.error("[voice] error:", err);
      return json(500, { error: String(err && err.message) });
    }
  }

  json(404, { error: "Not found" });
});

attach(server);

server.listen(PORT, "0.0.0.0", () => {
  console.log(`[voice] http  http://0.0.0.0:${PORT}/v1/voice   (session ${SESSION})`);
  console.log(`[voice] ws    ws://0.0.0.0:${PORT}/ws`);
  console.log(`[voice] push  POST /v1/notify {"text":"..."}`);
});
