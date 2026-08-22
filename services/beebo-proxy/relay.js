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
const { transcribe, ask, speakable, synthesize, synthesizeStream, wrapWav, unavailableReason, audioLinks, fetchBuffer } = require("./pipeline");
const { toBoardWav } = require("./transcode");

const TOKEN       = process.env.RELAY_TOKEN || "";     /* empty = LAN trust */
const MAX_PCM     = 12 * 24000 * 2;                    /* 12 s at 24 kHz mono */
const AUDIO_CHUNK = 4096;
const MIN_SENTENCE = 6;                                /* characters */
/* Audio the agent links to is played. Set PLAY_LINKS=0 to have it described
 * instead, which is what you want if the agent hands out links you would
 * rather not have come out of a speaker unannounced. */
const PLAY_LINKS  = process.env.PLAY_LINKS !== "0";
/* RELAY_TRACE=1 prints when each clause is handed to the synthesiser and when
 * its audio goes out. Worth having: "is it actually streaming" is otherwise a
 * question you can only answer by reading the source and hoping. */
const TRACE = process.env.RELAY_TRACE === "1";
const trace = (ms, what) => { if (TRACE) console.log(`[relay]   +${ms}ms ${what}`); };

const clients = new Set();

/* The machine running the proxy already knows what time it is and what zone it
 * is in. Making the board ask a human to configure that - or worse, compiling
 * it in - is asking for a setting that is wrong the first time somebody moves.
 *
 * POSIX TZ inverts the sign of the UTC offset, so UTC+8 is written "-8". The
 * angle-bracket form avoids inventing an abbreviation the board would have to
 * recognise. This carries no daylight-saving rule, only the offset in force
 * right now, which is why it is re-sent rather than sent once. */
function posixTz() {
  const off = -new Date().getTimezoneOffset();        /* minutes east of UTC */
  const a = Math.abs(off), h = Math.floor(a / 60), m = a % 60;
  const label = `<${off >= 0 ? "+" : "-"}${String(h).padStart(2, "0")}${m ? String(m).padStart(2, "0") : ""}>`;
  return `${label}${off >= 0 ? "-" : "+"}${h}${m ? ":" + String(m).padStart(2, "0") : ""}`;
}

function timeMessage() {
  return {
    type: "time",
    epoch: Math.floor(Date.now() / 1000),
    tz: posixTz(),
    zone: Intl.DateTimeFormat().resolvedOptions().timeZone,
  };
}

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
  const nextIndex = () => index++;
  let opener = true;
  for (const clause of clauses(text)) {
    const spoken = speakable(clause, false);
    if (!spoken) continue;
    await speakSegment(ws, spoken, nextIndex, opener);
    opener = false;
  }
  send(ws, { type: "turn.done" });
}

/* Plays a whole audio file - an MP3 someone sent, a jingle, anything the
 * transcoder reads.
 *
 * Paced rather than pushed. The board holds finished segments in a queue eight
 * deep and blocks when it is full, so firing a four-minute file at it as fast
 * as the socket allows would jam its receive task and start dropping the end.
 * A segment is handed over roughly as fast as it is consumed, with a couple in
 * hand so the speaker never starves.
 *
 * hold_gain on everything after the first: the board normalises each clip it
 * receives, and a piece of music re-levelled every eight seconds pumps
 * audibly. The opening segment sets the level and the rest keep it. */
const PLAY_SECONDS = 8;
const PLAY_LEAD    = 2;                       /* segments to keep in hand */

async function playWav(ws, wav) {
  let at = 12;
  while (at + 8 < wav.length && wav.slice(at, at + 4).toString() !== "data")
    at += 8 + wav.readUInt32LE(at + 4);
  const pcm = wav.subarray(at + 8);

  const step = PLAY_SECONDS * 24000 * 2;
  const total = Math.ceil(pcm.length / step);
  let index = 0;

  for (let off = 0; off < pcm.length; off += step) {
    if (ws.readyState !== ws.OPEN || playCancelled) return index;
    sendAudio(ws, index, wrapWav(pcm.subarray(off, off + step)), index > 0);
    index++;
    if (index > PLAY_LEAD && off + step < pcm.length)
      await new Promise((r) => setTimeout(r, PLAY_SECONDS * 1000));
  }
  send(ws, { type: "turn.done" });
  return total;
}

/* Serialised. Two files playing at once interleave their segments on the wire
 * and the board, which has no idea they came from different places, plays the
 * result as one stream of nonsense. */
let playChain = Promise.resolve();

function stopAll() {
  let n = 0;
  for (const ws of clients) {
    if (ws.readyState !== ws.OPEN) continue;
    send(ws, { type: "audio.stop" });
    n++;
  }
  playCancelled = true;
  return n;
}
let playCancelled = false;

async function playAll(input) {
  const wav = await toBoardWav(input);
  const seconds = (wav.length - 44) / 2 / 24000;
  const boards = [...clients].filter((ws) => ws.readyState === ws.OPEN);

  playChain = playChain.then(async () => {
    playCancelled = false;
    await Promise.all(boards.map((ws) =>
      playWav(ws, wav).catch((e) => console.error("[play] failed:", e.message))));
  });
  return { boards: boards.length, seconds, queued: true };
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
    if (enders.includes(pending[i])) { cut = i; continue; }
    /* A full stop ends an English sentence, but only with a space or the end
     * of the text after it - otherwise every decimal and abbreviation splits a
     * clause down the middle. Without this the whole of an English reply after
     * the opening clause arrived as one segment. */
    if (pending[i] === "." && (i + 1 === pending.length || pending[i + 1] === " ")) cut = i;
  }
  return cut + 1 >= min ? cut + 1 : 0;
}

function sendAudio(ws, index, audio, holdGain) {
  if (ws.readyState !== ws.OPEN) return;
  send(ws, { type: "audio.begin", index, bytes: audio.length, hold_gain: !!holdGain });
  for (let at = 0; at < audio.length; at += AUDIO_CHUNK) {
    if (ws.readyState !== ws.OPEN) return;
    ws.send(audio.subarray(at, Math.min(at + AUDIO_CHUNK, audio.length)));
  }
  send(ws, { type: "audio.end", index });
}

/* The opening second of the first clause is sent as soon as it exists, ahead
 * of the rest of that same clause.
 *
 * The synthesiser streams: measured, its first bytes arrive in about 250 ms
 * where the finished clip takes 500-1000. The board plays whole segments, so
 * the way to spend that is to cut the first clause in two and let it start on
 * the head while the tail is still being made. Only the first clause needs it -
 * every later one is already being synthesised while an earlier one plays.
 *
 * The seam is why the tail carries hold_gain: the board normalises each clip it
 * receives, and two halves of one sentence normalised independently would step
 * in volume in the middle of a word. */
const HEAD_MS = parseInt(process.env.TTS_HEAD_MS || "1000", 10);
const HEAD_BYTES = 44 + Math.round((HEAD_MS / 1000) * 24000 * 2);

/* nextIndex() hands out segment numbers, because splitting a clause produces
 * two of them and the caller cannot know in advance whether it will. */
async function speakSegment(ws, text, nextIndex, splitHead) {
  if (ws.readyState !== ws.OPEN) return;

  if (!splitHead) {
    const parts = [];
    await synthesizeStream(text, (b) => parts.push(b));
    return sendAudio(ws, nextIndex(), Buffer.concat(parts), false);
  }

  const parts = [];
  let held = 0, headSent = false;
  await synthesizeStream(text, (b) => {
    parts.push(b);
    held += b.length;
    if (headSent || held < HEAD_BYTES) return;
    headSent = true;
    const all = Buffer.concat(parts);
    sendAudio(ws, nextIndex(), all.subarray(0, HEAD_BYTES), false);
    parts.length = 0;
    parts.push(all.subarray(HEAD_BYTES));       /* the tail starts here */
    held = 0;
  });

  const rest = Buffer.concat(parts);
  if (!headSent) return sendAudio(ws, nextIndex(), rest, false);   /* too short to split */
  if (rest.length) sendAudio(ws, nextIndex(), wrapWav(rest), true);
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

    const nextIndex = () => index++;
    /* Not "index === 0": indices are handed out when a segment is actually
     * sent, which is later and inside the chain, so two clauses queued before
     * the first synthesis finished would both believe they were the opener. */
    let openerPending = true;
    const flush = (raw) => {
      const text = speakable(raw, false);
      if (!text) return;
      const first = openerPending;
      openerPending = false;
      trace(Date.now() - t0, `flush: ${JSON.stringify(text).slice(0, 40)}`);
      chain = chain.then(async () => {
        if (!firstAudioAt) firstAudioAt = Date.now();
        const s0 = Date.now();
        await speakSegment(ws, text, nextIndex, first);
        trace(Date.now() - t0, `spoke (tts ${Date.now() - s0}ms)`);
      }).catch((e) => console.error("[relay] segment failed:", e.message));
    };

    /* Lines the agent chose to say out loud through its own tts tool. They
     * never appear in the reply text, so without this the board stays silent
     * through them and then plays whatever the agent said afterwards - which
     * is written assuming you already heard them. */
    const spokenAside = [];

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
      /* openerPending, not "index === 0": indices are handed out when a
       * segment is sent, which is later and inside the chain. */
      const n = completeSentenceLength(pending, openerPending);
      if (n > 0) {
        flush(pending.slice(0, n));
        spokenUpTo += n;
      }
    }, (aside) => {
      spokenAside.push(aside.trim());
      send(ws, { type: "reply.partial", text: speakable(aside, false) });
      flush(aside);
    });
    const tAgent = Date.now();
    send(ws, { type: "reply", text: reply });

    /* The agent sometimes repeats its spoken line in the reply as well; saying
     * it twice is worse than not saying it. */
    const tail = reply.slice(spokenUpTo).trim();
    if (tail && !spokenAside.some((a) => a && (a === tail || tail.includes(a)))) {
      flush(reply.slice(spokenUpTo));
    }
    await chain;

    /* If the agent answered with a link to audio - a song it found, a clip -
     * play the thing rather than describing it. speakable() has already kept
     * the URL out of the spoken text, since reading one aloud is a minute of
     * punctuation. */
    const links = PLAY_LINKS ? audioLinks(reply) : [];
    for (const url of links.slice(0, 1)) {
      try {
        console.log(`[play] fetching ${url}`);
        const file = await fetchBuffer(url);
        const r = await playAll(file);
        console.log(`[play] ${url} -> ${r.seconds.toFixed(1)}s`);
      } catch (e) {
        console.error(`[play] ${url} failed:`, e.message);
      }
    }

    if (!links.length) send(ws, { type: "turn.done" });

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
        send(ws, timeMessage());        /* before it can possibly need it */
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

  /* Hourly, so a board that stays connected across a daylight-saving change or
   * a slow crystal does not quietly drift. It is forty bytes. */
  const clock = setInterval(() => {
    const msg = timeMessage();
    for (const ws of wss.clients) send(ws, msg);
  }, 3600000);

  const beat = setInterval(() => {
    for (const ws of wss.clients) {
      if (!ws.isAlive) { ws.terminate(); continue; }
      ws.isAlive = false;
      ws.ping();
    }
  }, 20000);
  wss.on("close", () => { clearInterval(beat); clearInterval(clock); });

  console.log("[relay] websocket ready on /ws");
  return wss;
}

module.exports = { attach, notifyAll, playAll, stopAll, clientCount: () => clients.size };
