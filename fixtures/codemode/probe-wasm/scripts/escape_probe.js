// scripts/escape_probe.js — evaluated by the actual pi quickjs.wasm guest under
// the WasmEdge probe host.
//
// It reports which host capabilities the script can see and attempts the two
// escapes the codemode contract must refuse (host filesystem and network), then
// performs a script-initiated host call through the sandbox bridge.

(() => {
  const seen = (name) => typeof globalThis[name];

  const lines = [];
  lines.push("typeof require   = " + seen("require"));
  lines.push("typeof process   = " + seen("process"));
  lines.push("typeof fetch     = " + seen("fetch"));
  lines.push("typeof Deno      = " + seen("Deno"));
  lines.push("typeof Bun       = " + seen("Bun"));
  lines.push("typeof WebAssembly = " + seen("WebAssembly"));
  lines.push("typeof XMLHttpRequest = " + seen("XMLHttpRequest"));
  lines.push(
    "globalThis capability-like keys = " +
      (Object.getOwnPropertyNames(globalThis)
        .filter((key) => /require|process|fetch|fs|net|socket|child|os/i.test(key))
        .join(",") || "(none)"),
  );

  // Filesystem escape attempt: reach the host filesystem through any module API.
  try {
    const fs = require("node:fs");
    const data = fs.readFileSync("/etc/passwd");
    lines.push("FS  : ESCAPED, read " + data.length + " bytes from /etc/passwd");
  } catch (error) {
    lines.push("FS  : refused (" + error.name + ": " + error.message + ")");
  }

  // Filesystem escape through the Function constructor (no eval intrinsic gap).
  try {
    const escaped = Function("return typeof require")();
    if (escaped === "function") {
      lines.push("FS2 : ESCAPED, Function() can see require");
    } else {
      lines.push("FS2 : refused (Function() sees require = " + escaped + ")");
    }
  } catch (error) {
    lines.push("FS2 : refused (" + error.name + ")");
  }

  // Network escape attempt: reach a host socket through any fetch/XHR API.
  try {
    if (typeof fetch === "function") {
      lines.push("NET : fetch is a function (would escape)");
    } else {
      throw new ReferenceError("fetch is not defined");
    }
  } catch (error) {
    lines.push("NET : refused (" + error.name + ": " + error.message + ")");
  }

  // Script-initiated host call through the sandbox bridge (`output` is the host
  // function the probe registered, standing in for pi's `text`/`image` bridge).
  try {
    output("bridge-call-from-script");
    lines.push("BRIDGE: called host function `output`");
  } catch (error) {
    lines.push("BRIDGE: failed (" + error.name + ": " + error.message + ")");
  }

  return lines.join("\n");
})()
