# OpenClaw gateway node handshake

Recovered against OpenClaw 2026.7.1-2 by probing the live gateway and reading
its own bundle. Verified end to end: the gateway accepts the frame below and
fails only on the missing shared token.

## Transport

Plain WebSocket on the gateway port (18789), path `/`. No subprotocol, no
auth headers — everything happens in the first two frames.

`gateway.bind` must not be `loopback` or nothing off-box can reach it.

## 1. Gateway speaks first

```json
{"type":"event","event":"connect.challenge",
 "payload":{"nonce":"<uuid>","ts":1787325864273}}
```

`ts` is the gateway's own clock in ms. Signing against it rather than the
device clock keeps the proof inside the server's skew window by construction —
which matters here because the board's clock is only correct after SNTP.

## 2. Identity

- Ed25519 keypair.
- `publicKey` on the wire: **unpadded base64url of the raw 32 bytes**.
- `signature` on the wire: same encoding.
- `deviceId`: **`sha256(raw 32-byte public key)` as lowercase hex** (verified
  against an existing entry in `~/.openclaw/devices/paired.json`).

## 3. The signed string

From the gateway's own bundle:

```
v2|deviceId|clientId|clientMode|role|scopes.join(",")|signedAtMs|token|nonce
```

Joined with `|`, signed raw (no hashing step of its own — Ed25519 handles it).
Empty scopes produce an empty field; the separators still stand.

## 4. Connect frame

```json
{"type":"req","id":"1","method":"connect",
 "params":{
   "minProtocol":4,"maxProtocol":4,
   "client":{"id":"node-host","version":"0.1.0",
             "platform":"esp32s3","mode":"node"},
   "role":"node","scopes":[],"caps":[],
   "commands":["device.info","device.status"],
   "permissions":{},
   "auth":{"token":"<gateway.auth.token>"},
   "device":{"id":"<hex>","publicKey":"<b64url>","signature":"<b64url>",
             "signedAt":1787325864273,"nonce":"<uuid from challenge>"}}}
```

`client.id` and `client.mode` are closed enums; anything else is rejected
outright with `INVALID_REQUEST`.

- `client.id` ∈ webchat-ui, openclaw-control-ui, openclaw-tui, webchat, cli,
  gateway-client, openclaw-macos, openclaw-ios, openclaw-android, **node-host**,
  test, fingerprint, openclaw-probe
- `client.mode` ∈ webchat, cli, ui, backend, **node**, probe, test

## 5. Responses

Success carries the durable credential:

```json
{"type":"res","id":"1","ok":true,
 "payload":{"type":"hello-ok","protocol":4,
   "auth":{"role":"node","scopes":[...],"deviceToken":"..."}, ...}}
```

Persist `auth.deviceToken` and use it on later connections. **Confirmed on
hardware:** send it as `auth.deviceToken` (not `auth.token`) and put the same
value in the token slot of the signed string. The gateway accepts that and
returns hello-ok without any further approval.

That is the whole security argument for this flow: the shared gateway token is
needed exactly once, and the per-device token that replaces it can be revoked on
its own if the board is lost, without touching any other client. Rotating the
shared token afterwards does not disturb an already-paired device.

The full `auth` object accepts `token`, `bootstrapToken`, `deviceToken`,
`password`, `approvalRuntimeToken`, `agentRuntimeIdentityToken` - all optional,
`additionalProperties: false`.

First connection of an unknown device returns `PAIRING_REQUIRED`; approve with:

```
openclaw devices list
openclaw devices approve <requestId>
```

Pending requests expire after 5 minutes, and a reconnecting device reuses the
same requestId rather than piling up new prompts.

Errors are `{"type":"res","ok":false,"error":{"code","message","details"}}`.
Read `details.code`, not the message — `AUTH_TOKEN_MISSING`, `PAIRING_REQUIRED`,
`MISSING_SCOPE` are stable, the prose is not.

## Reference implementation

`scratchpad/wsconnect.py` — stdlib WebSocket plus `cryptography` for Ed25519.
Iterating there instead of reflashing turned a two-minute cycle into seconds.

## Notes for the firmware port

- mbedTLS on ESP-IDF does not expose Ed25519 signing; use libsodium
  (`espressif/libsodium`) `crypto_sign_detached`, which the S3 handles easily.
- Keep the keypair in NVS, ideally with flash encryption on — it is the board's
  identity, and the board leaves the house.
- SNTP must complete before connecting, otherwise `wss://` certificate
  validation fails outright. Plain `ws://` on the LAN does not care, but the
  Tailscale/remote path will.
