/* Reads .env beside this file. A dependency-free loader is enough for four
 * keys, and it keeps every secret in one gitignored place. */
const fs = require("fs"), path = require("path");

const file = process.env.PROXY_ENV || path.join(__dirname, ".env");
try {
  for (const line of fs.readFileSync(file, "utf8").split("\n")) {
    const m = /^\s*([A-Z0-9_]+)\s*=\s*(.*)$/.exec(line);
    if (!m) continue;
    const v = m[2].trim().replace(/^["']|["']$/g, "");
    if (!(m[1] in process.env)) process.env[m[1]] = v;
  }
} catch { /* no .env is fine when the environment already carries the keys */ }
