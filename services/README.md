# beebo-proxy

Everything the board cannot do itself: speech in, an agent in the middle,
speech out. It runs on your computer, not on the agent's host, so the board
reaches it directly over the LAN and nothing needs forwarding.

```
board ──── one WebSocket ───▶ beebo-proxy ──┬── speech-to-text → DashScope
                                     :18797 ├── the agent      → OpenClaw gateway
                                            └── text-to-speech → DashScope
```

| file | what it is |
|---|---|
| `server.js` | HTTP + WebSocket front door |
| `relay.js` | the board's socket: a turn, and streaming the reply out as it is written |
| `pipeline.js` | transcribe → ask → speak, all called as functions |
| `gateway-client.js` | a persistent operator session on the OpenClaw gateway |

Only `gateway-client.js` knows what OpenClaw is. Pointing this at a different
agent means rewriting that one file.

## Running it

```sh
cp beebo-proxy/.env.example beebo-proxy/.env    # DashScope key, gateway URL
./run.sh
```

`com.beebo.proxy.plist` keeps it running across logins; replace `REPO` in it
with the path to your checkout first.

Secrets live in `.env`, which is gitignored. Nothing real is committed.

## Talking to it

```sh
# make the robot say something, unprompted
curl -s -X POST http://127.0.0.1:18797/v1/notify \
  -H 'Content-Type: application/json' -d '{"text":"the build finished"}'

# a whole turn over HTTP, useful for testing without the board
curl -s -o reply.wav -X POST http://127.0.0.1:18797/v1/voice \
  -H 'Content-Type: audio/wav' --data-binary @question.wav
```

## Board protocol on `/ws`

```
board  -> {"type":"hello","device":"beebo","fw":"...","token":"..."}
       -> {"type":"turn.begin"}  <binary PCM frames>  {"type":"turn.end"}

server -> {"type":"ready"}
       -> {"type":"state","value":"thinking"}
       -> {"type":"heard","text":"..."}          what the ASR made of it
       -> {"type":"reply.partial","text":"..."}  the reply so far, as it is written
       -> {"type":"reply","text":"..."}          the finished reply
       -> {"type":"audio.begin","index":N,"bytes":N}  <binary WAV>
          {"type":"audio.end","index":N}         one pair per clause
       -> {"type":"turn.done"}
       -> {"type":"notify","text":"..."}         unprompted
       -> {"type":"error","message":"..."}
```

PCM travels headerless at 24 kHz mono 16-bit; the server wraps it.

## Why a turn takes three seconds and not twenty

Three measured changes, in order of how much they were worth.

**Turning the agent's thinking off** saved about nine seconds and was invisible
until the same question was timed against the CLI. It needs the `agent` RPC
rather than `chat.send`, because only that one accepts the parameter.

**Shaping the reply for speech.** The agent answers a spoken question the way it
answers a typed one: markdown headings, tables, emoji. One such reply cost 18 s
to generate and 14 s to synthesise — the length of the text, not the speed of
any service, is what makes a voice turn feel slow. `pipeline.js` appends a short
directive asking for one or two spoken sentences and strips anything screen-only
before it reaches the synthesiser. A 31 s turn became 10 s.

**Speaking each clause as it is written.** OpenClaw streams the text roughly
every 200 ms; waiting for the finished reply threw that away. Now the first
completed clause is synthesised and sent immediately, so the board starts
talking while the agent is still writing. Measured on this setup the remaining
wait is almost entirely the agent's time to first token — about five seconds
before any text exists at all — which is why the opening clause is allowed to
break on a comma while later ones wait for a full stop.

`RELAY_TRACE=1` prints when each clause goes to the synthesiser and when its
audio goes out, which is the only honest way to answer "is it actually
streaming". Measured on the wire, driving the relay exactly as the board does:

```
   +   4 ms  question sent
   + 605 ms  heard: Tell me a three-sentence story about a robot.
   +3360 ms  clause 1 complete, sent for synthesis
   +3822 ms  audio playable            <- the speaker starts here
   +4161 ms  reply COMPLETE (253 chars)  <- 339 ms LATER than the first sound
   +4332 ms  audio 2      +5178 ms  audio 3      +5876 ms  audio 4
```

Two things make that work.

**The reply is spoken as it is written.** The agent streams text roughly every
200 ms; waiting for the finished reply threw that away. Each completed clause
is synthesised and sent immediately, so clauses 2 to 4 arrive while clause 1 is
still being spoken and the speaker never starves. The opening clause is allowed
to break on a comma while later ones wait for a full stop, because almost all
of the wait is in front of the first word and nothing after it is heard as a
delay.

**Synthesis is streamed, and the opening clause is cut in two.** The service
will return a finished clip, but it will also stream the same audio over SSE,
and measured against each other:

| clause | finished clip | first streamed bytes | stream complete |
|---|---|---|---|
| "A lighthouse robot was programmed to keep the light on," | 1105 ms | **335 ms** | 800 ms |
| "but nobody came for two hundred years." | 672 ms | **304 ms** | 645 ms |
| "When a ship finally appeared on the horizon, it said welcome home." | 1036 ms | **310 ms** | 927 ms |

The board plays whole segments, so the way to spend that head start is to send
the first second of the opening clause as its own segment and let playback
begin on it while the rest is still being made. It cut the wait for first audio
from ~900 ms after the clause was ready to ~460 ms. Only the opening clause is
split; every later one is already being synthesised while an earlier one plays.

The seam is why the tail carries `hold_gain`. The board normalises each clip it
receives, and two halves of one sentence normalised independently would step in
volume in the middle of a word, so the tail reuses whatever the head settled on.

What is left is mostly not ours. Of the ~3.8 s before the first sound, about
2.7 is the agent thinking before it writes anything, 0.6 is speech-to-text, and
0.5 is the synthesiser. Transfer is negligible on a LAN.
