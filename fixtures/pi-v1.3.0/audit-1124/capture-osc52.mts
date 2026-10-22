#!/usr/bin/env tsx
/**
 * Capture the `audit-1124` clipboard OSC 52 opt-in family against frozen `pi-v1.3.0` (#1124).
 *
 * HONEST SCOPE: this is a SOURCE PROBE of the frozen `copyToClipboard` policy, not a product-level
 * capture. It calls the frozen public function with a controlled `process.env` and records the bytes
 * written to `process.stdout`. The clipboard *effect* cannot be verified — frozen `clipboard.ts`
 * says so itself ("OSC 52 cannot be verified") — so the observation is the emitted sequence, its
 * count and the reported failure, never a claim that a terminal forwarded it.
 *
 * The helper commands are made unavailable by pointing `PATH` at an empty directory, which is the
 * frozen `runClipboardCommand` failure path (`spawn` error resolves `undefined`). That is the
 * `#9618` desktop-with-a-display shape the opt-in exists to extend to remote clients that leave no
 * SSH markers (#10114, #10826, #10835).
 *
 * Usage (from the repository root):
 *   NODE_OPTIONS=--conditions=source PI_CHECKOUT=<private frozen checkout> PI_BASELINE=pi-v1.3.0 \
 *     <checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1124/capture-osc52.mts
 */

import { execFileSync, spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { createRequire } from "node:module";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const scriptPath = fileURLToPath(import.meta.url);
const familyDir = path.dirname(scriptPath);
const auditRoot = path.resolve(familyDir, "..");
const repositoryRoot = path.resolve(auditRoot, "../..");
const family = "audit-1124";
const registryPath = path.join(auditRoot, "baselines.json");
const piCheckout = path.resolve(process.env.PI_CHECKOUT ?? path.resolve(repositoryRoot, "../pi"));

if (process.env.TSX_TSCONFIG_PATH === undefined) {
    const tsx = path.join(piCheckout, "node_modules", ".bin", "tsx");
    const result = spawnSync(tsx, [scriptPath, ...process.argv.slice(2)], {
        stdio: "inherit",
        env: { ...process.env, TSX_TSCONFIG_PATH: path.join(piCheckout, "tsconfig.json") },
    });
    if (result.error) {
        throw new Error(`cannot re-exec ${tsx} to resolve the pi checkout's workspace paths: ${result.error}`);
    }
    process.exit(result.status ?? 1);
}

const registry = JSON.parse(readFileSync(registryPath, "utf8"));
if (registry.schema !== "pike/pi-v1.3.0-audit-baselines/1") {
    throw new Error(`unexpected audit baseline registry schema: ${registryPath}`);
}
const baselineName = process.env.PI_BASELINE ?? registry.default;
const baseline = registry.baselines[baselineName];
if (!baseline) {
    throw new Error(
        `unknown baseline ${JSON.stringify(baselineName)}; registered baselines: ` +
            Object.keys(registry.baselines).sort().join(", "),
    );
}
const head = execFileSync("git", ["-C", piCheckout, "rev-parse", "HEAD"], { encoding: "utf8" }).trim();
if (head !== baseline.revision) {
    throw new Error(`pi checkout must be at ${baseline.revision} (baseline ${baselineName}), found ${head}`);
}
if (execFileSync("git", ["-C", piCheckout, "status", "--porcelain"], { encoding: "utf8" }).trim()) {
    throw new Error("pi capture requires an unmodified source checkout");
}
const glibc = (process.report.getReport() as any).header.glibcVersionRuntime;
if (process.platform !== "linux" || process.arch !== "x64" || !glibc) {
    throw new Error("audit capture requires Linux x86-64/glibc");
}
for (const [name, entry] of Object.entries<any>(registry.baselines)) {
    if (name !== baselineName && entry.bundles && family in entry.bundles) {
        throw new Error(`refusing to capture ${family}: it already belongs to baseline ${JSON.stringify(name)}`);
    }
}

const requirePi = createRequire(path.join(piCheckout, "package.json"));
let tsxVersion = "unknown";
try {
    tsxVersion = requirePi("tsx/package.json").version;
} catch {
    // The tsx version is provenance, not a capture precondition; keep the sentinel explicit.
}
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: glibc,
    node: process.version,
    tsx: tsxVersion,
};
const capturedAt = new Date().toISOString();

const clipboard = await import(
    pathToFileURL(path.join(piCheckout, "packages/coding-agent/src/utils/clipboard.ts")).href
);
const sourceEndpoints = [
    "packages/coding-agent/src/utils/clipboard.ts:copyToClipboard,isRemoteSession,emitOsc52",
    "packages/coding-agent/test/clipboard.test.ts:PI_CLIPBOARD_OSC52=1 emits OSC 52 after a successful %s copy",
];
const upstreamTest = (testCase: string) => ({ path: "packages/coding-agent/test/clipboard.test.ts", case: testCase });

// An empty `PATH` directory makes every frozen helper launch fail the way a host without
// `xclip`/`xsel`/`wl-copy` fails: `spawn` errors and `runClipboardCommand` resolves `undefined`.
const emptyPath = mkdtempSync(path.join(tmpdir(), "pi-1124-empty-path-"));
const stubbed = [
    "SSH_CONNECTION",
    "SSH_CLIENT",
    "MOSH_CONNECTION",
    "WAYLAND_DISPLAY",
    "TERMUX_VERSION",
    "WT_SESSION",
    "WSL_DISTRO_NAME",
    "WSLENV",
    "PI_CLIPBOARD_OSC52",
];
const originalWrite = process.stdout.write.bind(process.stdout);
const originalEnv = { ...process.env };

/**
 * Run one frozen copy with a controlled environment and record the OSC 52 writes it produces.
 * The stdout interceptor is what a real terminal would receive; nothing here claims it copied.
 */
async function observe(env: Record<string, string | undefined>, text: string) {
    for (const name of stubbed) delete process.env[name];
    process.env.PATH = emptyPath;
    for (const [name, value] of Object.entries(env)) {
        if (value !== undefined) process.env[name] = value;
    }
    const writes: string[] = [];
    process.stdout.write = ((chunk: any, ...rest: any[]) => {
        if (typeof chunk === "string" && chunk.startsWith("\x1b]52;c;")) {
            writes.push(chunk);
            return true;
        }
        return (originalWrite as any)(chunk, ...rest);
    }) as typeof process.stdout.write;
    let outcome: { resolved: boolean; error: string | null };
    try {
        await clipboard.copyToClipboard(text);
        outcome = { resolved: true, error: null };
    } catch (error) {
        outcome = { resolved: false, error: (error as Error).message };
    } finally {
        process.stdout.write = originalWrite;
        for (const name of Object.keys(process.env)) {
            if (!(name in originalEnv)) delete process.env[name];
        }
        Object.assign(process.env, originalEnv);
    }
    // A sequence longer than 96 characters (the 100 000-character bound case) is recorded by
    // its length and its exact head/tail instead of its full payload; the scenario declares
    // that normalization. Every ordinary sequence is recorded in full.
    return {
        outcome,
        osc52: {
            count: writes.length,
            encodedLengths: writes.map((sequence) => sequence.length - 8),
            sequences: writes.map((sequence) => (sequence.length <= 96 ? sequence : null)),
            sequenceHeads: writes.map((sequence) => sequence.slice(0, 12)),
            sequenceTails: writes.map((sequence) => sequence.slice(-1)),
        },
    };
}

const desktop = { DISPLAY: ":0" };
const scenarios = [
    {
        name: "osc52-opt-in-writes-the-sequence-for-a-desktop-session",
        inputs: { env: { ...desktop, PI_CLIPBOARD_OSC52: "1" }, text: "hello", path: "empty" },
        expected: await observe({ ...desktop, PI_CLIPBOARD_OSC52: "1" }, "hello"),
        normalizations: [],
        upstreamTest: upstreamTest("PI_CLIPBOARD_OSC52=1 emits OSC 52 after a successful linux copy"),
    },
    {
        name: "osc52-opt-in-absent-reports-the-x11-failure",
        inputs: { env: desktop, text: "hello", path: "empty" },
        expected: await observe(desktop, "hello"),
        normalizations: [],
        upstreamTest: upstreamTest("local Linux failure does not report an unverified OSC 52 write as success"),
    },
    {
        name: "osc52-opt-in-zero-is-not-an-opt-in",
        inputs: { env: { ...desktop, PI_CLIPBOARD_OSC52: "0" }, text: "hello", path: "empty" },
        expected: await observe({ ...desktop, PI_CLIPBOARD_OSC52: "0" }, "hello"),
        normalizations: [],
        upstreamTest: upstreamTest("local Linux failure does not report an unverified OSC 52 write as success"),
    },
    {
        name: "osc52-opt-in-writes-the-largest-allowed-payload-in-full",
        inputs: { env: { ...desktop, PI_CLIPBOARD_OSC52: "1" }, textBytes: 75000, path: "empty" },
        expected: await observe({ ...desktop, PI_CLIPBOARD_OSC52: "1" }, "a".repeat(75000)),
        normalizations: [
            "the 75000-byte payload's sequence is recorded by encodedLengths plus its exact heads/tails, not in full",
        ],
        upstreamTest: upstreamTest("PI_CLIPBOARD_OSC52=1 emits OSC 52 after a successful linux copy"),
    },
    {
        name: "osc52-opt-in-rejects-one-byte-over-the-size-bound",
        inputs: { env: { ...desktop, PI_CLIPBOARD_OSC52: "1" }, textBytes: 75001, path: "empty" },
        expected: await observe({ ...desktop, PI_CLIPBOARD_OSC52: "1" }, "a".repeat(75001)),
        normalizations: ["the 75001-byte payload is refused, so the observation is the failure message"],
        upstreamTest: upstreamTest("PI_CLIPBOARD_OSC52=1 emits OSC 52 after a successful linux copy"),
    },
    {
        name: "wsl-windows-terminal-prefers-osc52-before-the-remote-branch",
        inputs: { env: { ...desktop, WSL_DISTRO_NAME: "Ubuntu", WT_SESSION: "terminal-session", PI_CLIPBOARD_OSC52: "1" }, text: "hello", path: "empty" },
        expected: await observe(
            { ...desktop, WSL_DISTRO_NAME: "Ubuntu", WT_SESSION: "terminal-session", PI_CLIPBOARD_OSC52: "1" },
            "hello",
        ),
        normalizations: [],
        upstreamTest: upstreamTest("PI_CLIPBOARD_OSC52=1 emits OSC 52 after a successful linux copy"),
    },
];
rmSync(emptyPath, { recursive: true, force: true });

const sha256 = (bytes: Buffer) => createHash("sha256").update(bytes).digest("hex");
const artifactName = "clipboard-osc52.json";
const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: baselineName,
    revision: baseline.revision,
    capturedAt,
    environment,
    sourceEndpoints,
    scenarios,
};
const artifactBytes = Buffer.from(`${JSON.stringify(artifact, null, 2)}\n`);
const generatorPath = path.relative(repositoryRoot, scriptPath).split(path.sep).join("/");
const generatorBytes = readFileSync(scriptPath);
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: baselineName,
    revision: baseline.revision,
    ticket: "1124",
    family,
    capturedAt,
    environment,
    generator: {
        path: generatorPath,
        sha256: sha256(generatorBytes),
        command: `NODE_OPTIONS=--conditions=source PI_CHECKOUT=${piCheckout} PI_BASELINE=${baselineName} ${path.join(piCheckout, "node_modules/.bin/tsx")} ${generatorPath}`,
        sources: [{ path: generatorPath, sha256: sha256(generatorBytes) }],
    },
    artifacts: [
        {
            path: artifactName,
            family,
            bytes: artifactBytes.length,
            sha256: sha256(artifactBytes),
            sourceEndpoints,
            scenarios,
        },
    ],
};

// Write only after every guard and observation succeeded.
mkdirSync(familyDir, { recursive: true });
writeFileSync(path.join(familyDir, artifactName), artifactBytes);
writeFileSync(path.join(familyDir, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`);
console.log(`captured ${family} artifact ${artifactName} for ${baselineName} (${baseline.revision})`);
