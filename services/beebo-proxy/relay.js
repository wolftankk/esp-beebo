/*
 * The board's single connection.
 *
 * Audio up, audio and text down, plus whatever the server decides to say
 * unprompted - which is the reason this is a socket and not another HTTP
 * endpoint. A device that can only ask can never be told anything.
 *
 * The reply is spoken as it is written. OpenClaw streams the agent's text a
 * token at a time; waiting for the finished reply before starting synthesis
 * threw that away and made every turn wait for the longest sentence in it.
 * Each completed sentence is synthesised and sent immediately, so the board
 * starts talking while the agent is still thinking about the rest.
 *
 * board  -> {hello} {turn.begin} <binary pcm...> {turn.end}
 * server -> {ready} {state} {heard} {reply}
 *           ({audio.begin} <binary...> {audio.end})*   one per sentence
 *           {turn.done} {notify} {error}
 */
const { WebSocketServer } = require("ws");
const { transcribe, ask, speakable, synthesize, wrapWav, unavailableReason } = require("./pipeline");

const TOKEN       = process.env.RELAY_TOKEN || "";     /* empty = LAN trust */
const MAX_PCM     = 12 * 24000 * 2;                    /* 12 s at 24 kHz mono */
const AUDIO_CHUNK = 4096;
const MIN_SENTENCE = 6;                                /* characters */

const clients = new Set();

function send(ws, obj) {
  if (ws.readyState === ws.OPEN) ws.send(JSON.stringify(obj));
}

/* Splits into the same clause-sized pieces a streamed reply is broken into, so
 * a long notification starts playing after the first sentence rather than
 * after the synthesiser has worked through all of it. */
function clauses(text) {
  const out = [];
  let at = 0;
  for (let i = 0; i < text.length; i++) {
    if (!"。！？!?\n".includes(text[i])) continue;
    if (i + 1 - at < MIN_SENTENCE) continue;
    out.push(text.slice(at, i + 1));
    at = i + 1;
  }
  if (at < text.length) out.push(text.slice(at));
  return out.filter((c) => c.trim());
}

async function speakText(ws, text) {
  let index = 0;
  for (const clause of clauses(text)) {
    const spoken = speakable(clause, false);
    if (spoken) await speakSegment(ws, spoken, index++);
  }
  send(ws, { type: "turn.done" });
}

function notifyAll(text) {
  let n = 0;
  for (const ws of clients) {
    if (ws.readyState !== ws.OPEN) continue;
    send(ws, { type: "notify", text });
    /* Said out loud, not just put on the screen. A notification the robot
     * only displays is one you have to already be looking at. */
    speakText(ws, text).catch((e) => console.error("[notify] speak failed:", e.message));
    n++;
  }
  return n;
}

/* How much of `pending` is safe to synthesise. Splitting mid-clause makes the
 * synthesiser guess at prosody it cannot know yet, so normally this waits for
 * a sentence to end.
 *
 * The first segment is the exception. Measured, the agent needs about five
 * seconds before it emits any text at all and then finishes in under three, so
 * nearly all of the wait is in front of the first word - and every 100 ms
 * saved there is heard, while a slightly flat comma in the middle of a reply
 * is not. So the opening clause is allowed to break on a comma. */
function completeSentenceLength(pending, opening) {
  const enders = opening ? "。！？!?、，,；;：:\n" : "。！？!?\n";
  const min = opening ? 4 : MIN_SENTENCE;
  let cut = -1;
  for (let i = 0; i < pending.length; i++) {
    if (enders.includes(pending[i])) cut = i;
  }
  return cut + 1 >= min ? cut + 1 : 0;
}

async function speakSegment(ws, text, index) {
  if (ws.readyState !== ws.OPEN) return;
  const audio = await synthesize(text);
  if (ws.readyState !== ws.OPEN) return;
  send(ws, { type: "audio.begin", index, bytes: audio.length });
  for (let at = 0; at < audio.length; at += AUDIO_CHUNK) {
    if (ws.readyState !== ws.OPEN) return;
    ws.send(audio.subarray(at, Math.min(at + AUDIO_CHUNK, audio.length)));
  }
  send(ws, { type: "audio.end", index });
}

async function runTurn(ws, pcm) {
  const t0 = Date.now();
  try {
    send(ws, { type: "state", value: "thinking" });

    const heard = await transcribe(wrapWav(pcm));
    const tAsr = Date.now();
    if (!heard) {
      send(ws, { type: "error", message: "nothing recognised" });
      send(ws, { type: "state", value: "idle" });
      return;
    }
    send(ws, { type: "heard", text: heard });

    let spokenUpTo = 0;
    let index = 0;
    let firstAudioAt = 0;
    /* Serialised: sentences must reach the speaker in the order they were
     * written, and synthesis of the next may finish before the previous. */
    let chain = Promise.resolve();

    const flush = (raw) => {
      const text = speakable(raw, false);
      if (!text) return;
      const i = index++;
      chain = chain.then(async () => {
        if (!firstAudioAt) firstAudioAt = Date.now();
        await speakSegment(ws, text, i);
      }).catch((e) => console.error("[relay] segment failed:", e.message));
    };

    let shown = 0;
    const reply = await ask(heard, (full) => {
      /* Show it arriving. The board used to get one finished block of text at
       * the end, so the screen sat blank through the whole wait even though
       * the words existed. */
      if (full.length - shown >= 4) {
        shown = full.length;
        send(ws, { type: "reply.partial", text: speakable(full, false) });
      }
      const pending = full.slice(spokenUpTo);
      const n = completeSentenceLength(pending, index === 0);
      if (n > 0) {
        flush(pending.slice(0, n));
        spokenUpTo += n;
      }
    });
    const tAgent = Date.now();
    send(ws, { type: "reply", text: reply });

    if (reply.length > spokenUpTo) flush(reply.slice(spokenUpTo));
    await chain;
    send(ws, { type: "turn.done" });

    console.log(
      `[proxy] ${new Date().toISOString()} ${pcm.length}B pcm ` +
      `asr=${tAsr - t0}ms agent=${tAgent - tAsr}ms ` +
      `first-audio=${firstAudioAt ? firstAudioAt - tAsr : -1}ms ` +
      `total=${Date.now() - t0}ms segments=${index} ` +
      `heard=${JSON.stringify(heard).slice(0, 50)} reply=${reply.length}ch`
    );
  } catch (err) {
    console.error("[relay] turn failed:", err.message);
    send(ws, { type: "error", message: String(err.message).slice(0, 120) });
    /* A turn that dies leaves the robot staring back having heard you and said
     * nothing, which is indistinguishable from it being broken. If the reason
     * is something a person can act on, it says so out loud. */
    const why = unavailableReason();
    if (why) {
      send(ws, { type: "reply", text: why });
      await speakText(ws, why).catch(() => {});
    }
    send(ws, { type: "state", value: "idle" });
  }
}

function attach(httpServer) {
  const wss = new WebSocketServer({ server: httpServer, path: "/ws" });

  wss.on("connection", (ws, req) => {
    ws.isAlive = true;
    ws.greeted = false;
    ws.chunks = [];
    ws.pcmLen = 0;
    ws.capturing = false;
    const who = req.socket.remoteAddress;

    ws.on("pong", () => { ws.isAlive = true; });

    ws.on("message", async (data, isBinary) => {
      if (isBinary) {
        if (!ws.capturing) return;
        if (ws.pcmLen + data.length > MAX_PCM) {
          ws.capturing = false;
          send(ws, { type: "error", message: "recording too long" });
          return;
        }
        ws.chunks.push(Buffer.from(data));
        ws.pcmLen += data.length;
        return;
      }

      let m;
      try { m = JSON.parse(data.toString()); } catch { return; }

      if (m.type === "hello") {
        if (TOKEN && m.token !== TOKEN) {
          send(ws, { type: "error", message: "unauthorised" });
          return ws.close();
        }
        ws.greeted = true;
        clients.add(ws);
        console.log(`[relay] ${m.device || "device"} connected from ${who} (fw ${m.fw || "?"})`);
        send(ws, { type: "ready" });
        return;
      }
      if (!ws.greeted) return;

      if (m.type === "turn.begin") {
        ws.chunks = [];
        ws.pcmLen = 0;
        ws.capturing = true;
        return;
      }
      if (m.type === "turn.end") {
        ws.capturing = false;
        const pcm = Buffer.concat(ws.chunks);
        ws.chunks = [];
        ws.pcmLen = 0;
        if (pcm.length < 24000) {                 /* under half a second */
          send(ws, { type: "state", value: "idle" });
          return;
        }
        await runTurn(ws, pcm);
        return;
      }
    });

    ws.on("close", () => { clients.delete(ws); console.log(`[relay] ${who} disconnected`); });
    ws.on("error", () => clients.delete(ws));
  });

  const beat = setInterval(() => {
    for (const ws of wss.clients) {
      if (!ws.isAlive) { ws.terminate(); continue; }
      ws.isAlive = false;
      ws.ping();
    }
  }, 20000);
  wss.on("close", () => clearInterval(beat));

  console.log("[relay] websocket ready on /ws");
  return wss;
}

module.exports = { attach, notifyAll, clientCount: () => clients.size };
