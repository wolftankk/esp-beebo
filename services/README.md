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
   +   1 ms  question sent
   + 577 ms  heard: 给我讲一个三句话的小故事。
   +3089 ms  flush #0  "宇航员在火星上种出了第一颗土豆，"   <- first clause, split on a comma
   +3986 ms  audio #0 playable                              <- the speaker starts here
   +4387 ms  reply COMPLETE (89 chars)                      <- 400 ms LATER than the first sound
   +4564 ms  audio #1        +5610 ms  audio #2        +7234 ms  audio #3
```

So the streaming is real, but the grain is a clause, not a frame: each one is
synthesised whole before it is sent. What that buys is the overlap - clauses 1
to 3 arrive while clause 0 is still being spoken, so the speaker never starves
and the total wait is the first clause rather than the whole answer.

What is left is mostly not ours. Of the four seconds before the first sound,
about 2.5 is the agent thinking before it writes anything and 0.9 is the
synthesiser's round trip; ASR is 0.6 and the transfer is negligible on a LAN.
Streaming synthesis would take a chunk out of that 0.9, and nothing here can
do much about the 2.5.
