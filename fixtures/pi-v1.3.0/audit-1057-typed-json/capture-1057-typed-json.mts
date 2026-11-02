/**
 * `audit-1057-typed-json` capture: the representable strict-JSON observations behind
 * `cch_chord::copy_strict_json` / `is_strict_json_value`.
 *
 * The #1057 bundle already records `copyJson` detachment and rejection. This supplemental bundle
 * records the direct `JsonValue` model scenario, the alias-free copy, the own `__proto__` data
 * property, and a non-finite number below the root, so a C++ comparison has a frozen observation
 * for the representable half of the typed-JSON contract. JavaScript-only classes (`undefined`,
 * functions, class instances, sparse arrays, symbols, accessors, cycles) stay visible in the
 * captured inputs/results but are excluded by the `support::JsonValue` carrier.
 *
 * The script refuses a checkout whose HEAD is not the registered baseline revision, a modified
 * source tree, a non-Linux/glibc host, and a family already registered. It re-executes itself
 * under the checkout's tsx so the recorded `tsx` version belongs to the same install.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1057-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1057-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1057-typed-json/capture-1057-typed-json.mts
 */

import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

const SCRIPT = fileURLToPath(import.meta.url);
const REPO = join(dirname(SCRIPT), "../../..");
const FAMILY = "audit-1057-typed-json";
const TICKET = "1057";
const BASELINE = process.env.PI_BASELINE ?? "pi-v1.3.0";
const CHECKOUT = process.env.PI_CHECKOUT;
const EXPECTED_REVISION = "71023aec1546d6d677a65669c5665c0927ce0dd0";
const TSX = CHECKOUT ? join(CHECKOUT, "node_modules/.bin/tsx") : "";

if (!CHECKOUT) {
    console.error("PI_CHECKOUT must name the private frozen pi v1.3.0 checkout");
    process.exit(1);
}
if (!existsSync(TSX)) {
    console.error(`the checkout has no tsx: ${TSX}`);
    process.exit(1);
}

function run(command: string, arguments_: string[], cwd = REPO): string {
    return execFileSync(command, arguments_, { cwd, encoding: "utf8" }).trim();
}

function sha256(content: Buffer | string): string {
    return createHash("sha256").update(content).digest("hex");
}

if (!process.env.AUDIT_1057_TYPED_JSON_TSX) {
    execFileSync(TSX, [SCRIPT, ...process.argv.slice(2)], {
        cwd: REPO,
        env: {
            ...process.env,
            AUDIT_1057_TYPED_JSON_TSX: "1",
            NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
            TSX_TSCONFIG_PATH: join(CHECKOUT, "tsconfig.json"),
        },
        stdio: "inherit",
    });
    process.exit(0);
}

const revision = run("git", ["rev-parse", "HEAD"], CHECKOUT);
if (revision !== EXPECTED_REVISION) {
    console.error(`checkout revision ${revision} is not ${EXPECTED_REVISION}`);
    process.exit(1);
}
const dirty = run("git", ["status", "--porcelain"], CHECKOUT);
if (dirty !== "") {
    console.error(`checkout is not clean:\n${dirty}`);
    process.exit(1);
}
if (process.platform !== "linux" || process.arch !== "x64") {
    console.error(`unsupported platform ${process.platform}-${process.arch}`);
    process.exit(1);
}
const report = process.report?.getReport() as { header?: { glibcVersionRuntime?: string } } | undefined;
if (!report?.header?.glibcVersionRuntime) {
    console.error("the captured environment is not glibc Linux");
    process.exit(1);
}

const registryPath = join(REPO, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
    baselines: Record<string, { revision?: string; bundles?: Record<string, unknown> }>;
};
const registered = registry.baselines[BASELINE];
if (registered?.revision !== EXPECTED_REVISION) {
    console.error(`baseline ${BASELINE} does not resolve to ${EXPECTED_REVISION}`);
    process.exit(1);
}
if (registered.bundles?.[FAMILY] !== undefined) {
    console.error(`${FAMILY} is already registered; a deliberate recapture needs an explicit registry replacement`);
    process.exit(1);
}
if (!existsSync(join(CHECKOUT, "packages/chord/src/json.ts"))) {
    console.error("the checkout is not pi v1.3.0: packages/chord/src/json.ts is missing");
    process.exit(1);
}

const chordJson = (await import(
    pathToFileURL(join(CHECKOUT, "packages/chord/src/json.ts")).href
)) as {
    copyJson: (value: unknown) => any;
    isJsonValue: (value: unknown) => boolean;
};
const { copyJson, isJsonValue } = chordJson;

type Scenario = {
    name: string;
    source: string;
    inputs: unknown;
    expected: unknown;
    normalizations?: string[];
};

function errorShape(error: unknown): { name: string; message: string } {
    const value = error as { name?: unknown; message?: unknown };
    return { name: String(value?.name ?? "Error"), message: String(value?.message ?? error) };
}

function plain<T>(value: T): T {
    return JSON.parse(JSON.stringify(value)) as T;
}

const scenarios: Scenario[] = [];
const capturedAt = new Date().toISOString();
const tsxVersion = run(TSX, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim();
const environment = {
    platform: "linux",
    arch: "x64",
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: tsxVersion,
};

{
    const shared = { value: 1 };
    const input = { left: shared, right: shared };
    const copied = copyJson(input) as { left: { value: number }; right: { value: number } };
    const copiedBeforeMutation = plain(copied);
    assert.deepEqual(copiedBeforeMutation, { left: { value: 1 }, right: { value: 1 } });
    assert.notEqual(copied, input);
    assert.notEqual(copied.left, shared);
    assert.notEqual(copied.right, shared);
    assert.notEqual(copied.left, copied.right);

    copied.left.value = 2;
    assert.equal(shared.value, 1);
    assert.equal(input.left.value, 1);
    assert.equal(copied.right.value, 1);
    scenarios.push({
        name: "copyJson copies strict JSON without retaining aliases",
        source:
            "packages/chord/test/json.test.ts:copies strict JSON without retaining aliases; " +
            "packages/chord/src/json.ts:copyJson",
        inputs: {
            value: { left: { value: 1 }, right: { value: 1 } },
            sharedInput: "left and right name one object",
        },
        expected: {
            copiedBeforeMutation,
            copiedIsInput: false,
            leftIsShared: false,
            rightIsShared: false,
            leftIsRight: false,
            sharedValueAfterCopyMutation: shared.value,
            sourceAfterCopyMutation: plain(input),
            copiedAfterMutation: plain(copied),
        },
    });
}

{
    const input = Object.create(null) as Record<string, unknown>;
    Object.defineProperty(input, "__proto__", {
        value: { safe: true },
        enumerable: true,
        writable: true,
        configurable: true,
    });
    const copied = copyJson(input) as Record<string, unknown>;
    assert.equal(Object.getPrototypeOf(copied), null);
    assert.equal(Object.hasOwn(copied, "__proto__"), true);
    assert.deepEqual(copied.__proto__, { safe: true });
    assert.equal(JSON.stringify(copied), JSON.stringify(input));
    scenarios.push({
        name: "copyJson preserves null prototypes and own __proto__ data properties",
        source:
            "packages/chord/test/json.test.ts:preserves null prototypes and own __proto__ data properties; " +
            "packages/chord/src/json.ts:copyJson",
        inputs: {
            value: { ["__proto__"]: { safe: true } },
            nullPrototype: true,
            ownProtoDataProperty: true,
        },
        expected: {
            copiedPrototypeIsNull: Object.getPrototypeOf(copied) === null,
            copiedHasOwnProto: Object.hasOwn(copied, "__proto__"),
            copiedProtoValue: copied.__proto__,
            copiedEqualsInputShape: JSON.stringify(copied) === JSON.stringify(input),
        },
    });
}

{
    const value = { outer: { inner: [1, Number.NaN] } };
    let failure: { name: string; message: string } | undefined;
    try {
        copyJson(value);
    } catch (error) {
        failure = errorShape(error);
    }
    assert.equal(failure?.name, "TypeError");
    assert.match(failure?.message ?? "", /non-finite number/);
    scenarios.push({
        name: "copyJson rejects a non-finite number nested below the root",
        source: "packages/chord/src/json.ts:copyJson (non-finite recursion)",
        inputs: { value: { outer: { inner: [1, "NaN"] } } },
        expected: { failure },
    });
}

{
    const cyclic: { self?: unknown } = {};
    cyclic.self = cyclic;
    const results = {
        strict: isJsonValue({ nested: [1, true, null] }),
        undefinedObject: isJsonValue({ omitted: undefined }),
        uint8Array: isJsonValue(new Uint8Array([1])),
        infinity: isJsonValue(Number.POSITIVE_INFINITY),
        cycle: isJsonValue(cyclic),
    };
    assert.deepEqual(results, {
        strict: true,
        undefinedObject: false,
        uint8Array: false,
        infinity: false,
        cycle: false,
    });
    scenarios.push({
        name: "isJsonValue checks strict JSON without normalizing it",
        source:
            "packages/chord/test/json.test.ts:checks strict JSON without normalizing it; " +
            "packages/chord/src/json.ts:isJsonValue",
        inputs: {
            cases: ["strict", "undefinedObject", "uint8Array", "infinity", "cycle"],
        },
        expected: { results },
    });
}

const bundle = join(REPO, "fixtures/pi-v1.3.0", FAMILY);
mkdirSync(bundle, { recursive: true });
const sourceEndpoints = [
    "packages/chord/src/json.ts:copyJson,isJsonValue",
    "packages/chord/test/json.test.ts:JsonValue model; copyJson alias, prototype and rejection observations",
];
const artifact = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision: EXPECTED_REVISION,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    sourceEndpoints,
    scenarios,
};
const artifactContent = `${JSON.stringify(artifact, null, 2)}\n`;
const artifactName = "chord-typed-json.json";
writeFileSync(join(bundle, artifactName), artifactContent, "utf8");

const generatorPath = "fixtures/pi-v1.3.0/audit-1057-typed-json/capture-1057-typed-json.mts";
const manifest = {
    schema: "pike/pi-v1.3.0-audit-evidence/1",
    baseline: BASELINE,
    revision: EXPECTED_REVISION,
    ticket: TICKET,
    family: FAMILY,
    capturedAt,
    environment,
    generator: {
        path: generatorPath,
        sha256: sha256(readFileSync(join(REPO, generatorPath))),
        command:
            `PI_CHECKOUT=${CHECKOUT} PI_BASELINE=${BASELINE} ` +
            `${join(CHECKOUT, "node_modules/.bin/tsx")} ${generatorPath}`,
        sources: [{ path: generatorPath, sha256: sha256(readFileSync(join(REPO, generatorPath))) }],
    },
    artifacts: [
        {
            path: artifactName,
            family: FAMILY,
            bytes: Buffer.byteLength(artifactContent),
            sha256: sha256(artifactContent),
            sourceEndpoints,
            scenarios,
        },
    ],
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(join(bundle, "manifest.json"), manifestContent, "utf8");

console.log(`${artifactName}: ${scenarios.length} scenario(s), ${Buffer.byteLength(artifactContent)} bytes`);
console.log(`manifest.json: ${Buffer.byteLength(manifestContent)} bytes, sha256 ${sha256(manifestContent)}`);
