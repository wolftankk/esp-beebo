/*
 * Anything with audio in it -> the one format the board plays.
 *
 * The board decodes nothing: it takes 24 kHz mono 16-bit PCM in a RIFF wrapper
 * and pushes it at the codec. Putting an MP3 decoder on it would cost flash,
 * CPU and internal RAM - of which there is about 25 KB free - to duplicate
 * something the machine running this proxy already does in hardware.
 *
 * afconvert ships with macOS and hands off to CoreAudio, so MP3, AAC, m4a,
 * FLAC, AIFF and the rest all decode without adding a dependency. ffmpeg is
 * used instead when it is present, since it reads a wider set and exists on
 * Linux.
 */
const { spawn } = require("child_process");
const { execFileSync } = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const RATE = 24000;

function have(cmd) {
  try { execFileSync("/usr/bin/which", [cmd], { stdio: "ignore" }); return true; }
  catch { return false; }
}

const FFMPEG = have("ffmpeg");
const AFCONVERT = have("afconvert");

function run(cmd, args) {
  return new Promise((resolve, reject) => {
    const p = spawn(cmd, args, { stdio: ["ignore", "ignore", "pipe"] });
    let err = "";
    p.stderr.on("data", (d) => { err += d.toString(); });
    p.on("error", reject);
    p.on("close", (code) => (code === 0 ? resolve() : reject(new Error(`${cmd}: ${err.trim().slice(-200)}`))));
  });
}

/* Returns a RIFF/WAVE buffer at 24 kHz mono 16-bit, whatever went in. */
async function toBoardWav(input) {
  if (!FFMPEG && !AFCONVERT)
    throw new Error("no transcoder: install ffmpeg, or run this on macOS");

  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "beebo-"));
  const src = path.join(dir, "in");
  const dst = path.join(dir, "out.wav");
  try {
    fs.writeFileSync(src, input);
    if (FFMPEG) {
      await run("ffmpeg", ["-hide_banner", "-loglevel", "error", "-y",
                           "-i", src, "-ac", "1", "-ar", String(RATE),
                           "-c:a", "pcm_s16le", "-f", "wav", dst]);
    } else {
      await run("afconvert", ["-f", "WAVE", "-d", `LEI16@${RATE}`, "-c", "1", src, dst]);
    }
    return fs.readFileSync(dst);
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

module.exports = { toBoardWav, RATE, transcoder: FFMPEG ? "ffmpeg" : AFCONVERT ? "afconvert" : null };
