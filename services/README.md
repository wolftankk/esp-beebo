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

A typical turn now: `asr=450ms agent=2800ms first-audio=2799ms total=4708ms`.
