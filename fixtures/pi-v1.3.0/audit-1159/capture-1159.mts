#!/usr/bin/env tsx
/**
 * Frozen public Harness observations for #1159: the durable `edit` tool's BOM and
 * CRLF repair, its per-file mutation queue, its structured diff details and its
 * rejection text.
 *
 * Every scenario drives the public Harness over a real `NodeExecutionEnv`: a
 * scripted faux model issues `edit` calls, the durable coding-tools extension
 * runs them, and the recorded tool results plus the file bytes are the
 * observation. The scenarios never inspect C++ and never restate what the
 * runtime should do: a later C++ test compares against this capture.
 *
 * The script refuses to write evidence unless the private checkout is the exact
 * baseline revision and its tracked source is clean, then re-executes itself
 * under that checkout's tsx so the recorded runtime belongs to the install that
 * ran the capture.
 *
 * Run from the Pike repository:
 *   PI_CHECKOUT=/tmp/cch-1080-pi-frozen PI_BASELINE=pi-v1.3.0 \
 *     /tmp/cch-1080-pi-frozen/node_modules/.bin/tsx \
 *     fixtures/pi-v1.3.0/audit-1159/capture-1159.mts
 */
import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const script = fileURLToPath(import.meta.url);
const repository = path.resolve(path.dirname(script), "../../..");
const family = "audit-1159";
const ticket = "1159";
const baseline = process.env.PI_BASELINE ?? "pi-v1.3.0";
const checkout = process.env.PI_CHECKOUT;
if (checkout === undefined || checkout === "") {
	console.error("PI_CHECKOUT must name the private frozen pi v1.3.0 checkout");
	process.exit(1);
}
const tsx = path.join(checkout, "node_modules/.bin/tsx");
if (!existsSync(tsx)) {
	console.error(`the checkout has no tsx: ${tsx}`);
	process.exit(1);
}

function run(command: string, arguments_: string[], cwd = repository): string {
	return execFileSync(command, arguments_, { cwd, encoding: "utf8" }).trim();
}

function sha256(content: Buffer | string): string {
	return createHash("sha256").update(content).digest("hex");
}

if (process.env.AUDIT_1159_TSX === undefined) {
	execFileSync(tsx, [script, ...process.argv.slice(2)], {
		cwd: repository,
		env: {
			...process.env,
			AUDIT_1159_TSX: "1",
			NODE_OPTIONS: [process.env.NODE_OPTIONS, "--conditions=source"].filter(Boolean).join(" "),
			TSX_TSCONFIG_PATH: path.join(checkout, "tsconfig.json"),
		},
		stdio: "inherit",
	});
	process.exit(0);
}

const registryPath = path.join(repository, "fixtures/pi-v1.3.0/baselines.json");
const registry = JSON.parse(readFileSync(registryPath, "utf8")) as {
	baselines: Record<string, { revision?: string; bundles?: Record<string, unknown> }>;
};
const revision = registry.baselines[baseline]?.revision;
if (revision === undefined || !/^[0-9a-f]{40}$/.test(revision)) {
	console.error(`baseline ${baseline} does not resolve to a full revision`);
	process.exit(1);
}
if (registry.baselines[baseline]?.bundles?.[family] !== undefined) {
	console.error(`${family} is already registered; a deliberate recapture needs an explicit registry replacement`);
	process.exit(1);
}
const head = run("git", ["rev-parse", "HEAD"], checkout);
if (head !== revision) {
	console.error(`checkout revision ${head} is not ${revision}`);
	process.exit(1);
}
const dirty = run("git", ["status", "--porcelain"], checkout);
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

const pi = await import("../audit-1056/pi.mts");
const support = await import("../audit-1056/support.mts");
const { openChat, closeChat, entriesOf, answer, toolResults } = support;
const { MemoryStorage, NodeExecutionEnv, createCodingTools, fauxAssistantMessage, fauxToolCall } = pi;

const normalizations = [
	"no captured value carries a temporary path",
	"a round's results are listed in call-id order, because concurrent tool tasks settle in scheduler order",
];

/** The model-visible tool result, with the diff payload the tool attached. */
function view(result: any) {
	const text = (result.content as any[])
		.filter((block) => block.type === "text")
		.map((block) => block.text)
		.join("");
	return {
		callId: result.toolCallId,
		toolName: result.toolName,
		isError: result.isError,
		text,
		details:
			result.details === undefined
				? null
				: {
						diff: result.details.diff,
						patch: result.details.patch,
						firstChangedLine: result.details.firstChangedLine ?? null,
					},
	};
}

/** File bytes as text plus the digest that proves the exact bytes written. */
function fileView(directory: string, name: string) {
	const bytes = readFileSync(path.join(directory, name));
	return { text: bytes.toString("utf8"), bytes: bytes.length, sha256: sha256(bytes) };
}

type Call = { readonly id: string; readonly args: Record<string, unknown> };
type Round = {
	/** `edit` calls the one assistant tool-use message issues. */
	readonly calls: readonly Call[];
	/** Files written before the round starts, as exact bytes. */
	readonly seed?: Readonly<Record<string, string>>;
	/** Files read back after the round settles. */
	readonly observe: readonly string[];
};

async function runRound(round: Round) {
	const directory = mkdtempSync(path.join(tmpdir(), "cch-1159-frozen-"));
	try {
		for (const [name, content] of Object.entries(round.seed ?? {})) writeFileSync(path.join(directory, name), content);
		const environment = new NodeExecutionEnv({ cwd: directory });
		const chat = await openChat(new MemoryStorage(), { env: () => environment });
		chat.registry.install(createCodingTools());
		// One assistant message, so the round's calls are concurrent tool tasks.
		chat.faux.setResponses([
			fauxAssistantMessage(
				round.calls.map((call) => fauxToolCall("edit", call.args, { id: call.id })),
				{ stopReason: "toolUse" },
			),
			answer("edited"),
		]);
		const settled = await (
			await chat.root.submit({ type: "input", content: "edit" }, pi.BACKGROUND_CONTEXT)
		).wait(pi.BACKGROUND_CONTEXT);
		// Concurrent tool tasks settle in scheduler order; the call id is the
		// stable identity, so the observation lists results by call id.
		const results = toolResults(await entriesOf(chat.root))
			.map(view)
			.sort((left: any, right: any) => String(left.callId).localeCompare(String(right.callId)));
		const files: Record<string, unknown> = {};
		for (const name of round.observe) {
			try {
				files[name] = fileView(directory, name);
			} catch {
				files[name] = null;
			}
		}
		await closeChat(chat);
		return { settled: settled.status, results, files };
	} finally {
		rmSync(directory, { recursive: true, force: true });
	}
}

const scenarios: unknown[] = [];
async function scenario(name: string, source: string, round: Round, inputs: Record<string, unknown> = {}) {
	const observed = await runRound(round);
	scenarios.push({ name, source, inputs: { ...inputs, calls: round.calls, seed: round.seed ?? {} }, expected: observed, normalizations });
}

const bomCrlfSeed = "\uFEFFalpha\r\nbeta\r\ngamma\r\n";
await scenario(
	"a BOM and CRLF file edits when the old text is LF and leaves the normalized content",
	"packages/durable/src/tools/edit.ts:createEditTool.execute;packages/durable/src/tools/edit-diff.ts:stripBom,detectLineEnding,normalizeToLF,restoreLineEndings",
	{
		seed: { "editing.txt": bomCrlfSeed },
		calls: [{ id: "e1", args: { path: "editing.txt", edits: [{ oldText: "beta", newText: "omega" }] } }],
		observe: ["editing.txt"],
	},
	{ seedBytes: { "editing.txt": sha256(bomCrlfSeed) }, seededText: { "editing.txt": "U+FEFF alpha CRLF beta CRLF gamma CRLF" } },
);

const plainSeed = "alpha\nbeta\n";
await scenario(
	"a single replacement returns the frozen diff patch and first changed line",
	"packages/durable/src/tools/edit.ts:createEditTool.execute;packages/durable/src/tools/edit-diff.ts:generateDiffString,generateUnifiedPatch",
	{
		seed: { "notes.txt": plainSeed },
		calls: [{ id: "e1", args: { path: "notes.txt", edits: [{ oldText: "beta", newText: "omega" }] } }],
		observe: ["notes.txt"],
	},
);

// Ten lines put the tenth edit behind a two-character gutter, which pins the
// diff's line-number width at the frozen `content.split("\n")` boundary.
const longSeed = `${Array.from({ length: 10 }, (_, index) => `line${index + 1}`).join("\n")}\n`;
await scenario(
	"a replacement on a two-digit line keeps the frozen diff gutter and context window",
	"packages/durable/src/tools/edit.ts:createEditTool.execute;packages/durable/src/tools/edit-diff.ts:generateDiffString,generateUnifiedPatch",
	{
		seed: { "long.txt": longSeed },
		calls: [{ id: "e1", args: { path: "long.txt", edits: [{ oldText: "line10\n", newText: "line\nadded\n" }] } }],
		observe: ["long.txt"],
	},
);

// The two targets sit sixteen lines apart, outside each other's four-line
// diff context, so every captured value is independent of which call the queue
// runs first: only the final file proves both landed.
const queueSeed = `${Array.from({ length: 20 }, (_, index) => `q${index + 1}`).join("\n")}\n`;
await scenario(
	"two concurrent edits of one file both land without a lost update",
	"packages/durable/src/tools/file-mutation-queue.ts:withFileMutationQueue;packages/durable/src/tools/edit.ts:createEditTool.execute",
	{
		seed: { "queued.txt": queueSeed },
		calls: [
			{ id: "e1", args: { path: "queued.txt", edits: [{ oldText: "q2\n", newText: "second\n" }] } },
			{ id: "e2", args: { path: "queued.txt", edits: [{ oldText: "q18\n", newText: "eighteenth\n" }] } },
		],
		observe: ["queued.txt"],
	},
	{
		execution: "one assistant message, so both edit tasks run concurrently and the file queue decides the order",
		disjointEdits: true,
	},
);

const duplicateSeed = "same\nsame\n";
await scenario(
	"an old text that appears twice fails with the frozen uniqueness message",
	"packages/durable/src/tools/edit-diff.ts:applyEditsToNormalizedContent,getDuplicateError",
	{
		seed: { "duplicate.txt": duplicateSeed },
		calls: [{ id: "e1", args: { path: "duplicate.txt", edits: [{ oldText: "same\n", newText: "once\n" }] } }],
		observe: ["duplicate.txt"],
	},
);

await scenario(
	"an old text that is absent from a multi-edit call fails with the frozen indexed message",
	"packages/durable/src/tools/edit-diff.ts:applyEditsToNormalizedContent,getNotFoundError",
	{
		seed: { "absent.txt": plainSeed },
		calls: [
			{
				id: "e1",
				args: {
					path: "absent.txt",
					edits: [
						{ oldText: "alpha", newText: "gamma" },
						{ oldText: "delta", newText: "epsilon" },
					],
				},
			},
		],
		observe: ["absent.txt"],
	},
);

await scenario(
	"an edit that reproduces the original text fails with the frozen no-change message",
	"packages/durable/src/tools/edit-diff.ts:applyEditsToNormalizedContent,getNoChangeError",
	{
		seed: { "unchanged.txt": "alpha\n" },
		calls: [{ id: "e1", args: { path: "unchanged.txt", edits: [{ oldText: "alpha", newText: "alpha" }] } }],
		observe: ["unchanged.txt"],
	},
);

await scenario(
	"an empty old text fails with the frozen empty-target message",
	"packages/durable/src/tools/edit-diff.ts:applyEditsToNormalizedContent,getEmptyOldTextError",
	{
		seed: { "empty.txt": "alpha\n" },
		calls: [{ id: "e1", args: { path: "empty.txt", edits: [{ oldText: "", newText: "beta" }] } }],
		observe: ["empty.txt"],
	},
);

await scenario(
	"two edits that target overlapping text fail with the frozen overlap message",
	"packages/durable/src/tools/edit-diff.ts:applyEditsToNormalizedContent",
	{
		seed: { "overlap.txt": "one\ntwo\nthree\n" },
		calls: [
			{
				id: "e1",
				args: {
					path: "overlap.txt",
					edits: [
						{ oldText: "one\ntwo\n", newText: "ONE\nTWO\n" },
						{ oldText: "two\nthree\n", newText: "TWO\nTHREE\n" },
					],
				},
			},
		],
		observe: ["overlap.txt"],
	},
);

await scenario(
	"an old text that appears twice in a multi-edit call fails with the frozen indexed uniqueness message",
	"packages/durable/src/tools/edit-diff.ts:applyEditsToNormalizedContent,getDuplicateError",
	{
		seed: { "twice.txt": duplicateSeed },
		calls: [
			{
				id: "e1",
				args: {
					path: "twice.txt",
					edits: [
						{ oldText: "same\n", newText: "once\n" },
						{ oldText: "absent", newText: "x" },
					],
				},
			},
		],
		observe: ["twice.txt"],
	},
);

const capturedAt = new Date().toISOString();
const environment = {
	platform: "linux",
	arch: "x64",
	libc: "glibc",
	glibcVersion: report.header.glibcVersionRuntime,
	node: process.version,
	tsx: run(tsx, ["--version"]).split("\n", 1)[0].replace(/^tsx v?/, "").trim(),
};
const sourceEndpoints = [
	"packages/durable/src/tools/edit.ts:createEditTool.execute",
	"packages/durable/src/tools/edit-diff.ts:applyEditsToNormalizedContent,generateDiffString,generateUnifiedPatch",
	"packages/durable/src/tools/file-mutation-queue.ts:withFileMutationQueue",
];
const bundle = path.join(repository, "fixtures/pi-v1.3.0", family);
mkdirSync(bundle, { recursive: true });
const artifact = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline,
	revision,
	ticket,
	family,
	capturedAt,
	environment,
	sourceEndpoints,
	scenarios,
};
const artifactName = "edit-tool.json";
const artifactContent = `${JSON.stringify(artifact, null, 2)}\n`;
writeFileSync(path.join(bundle, artifactName), artifactContent, "utf8");

const generatorPath = "fixtures/pi-v1.3.0/audit-1159/capture-1159.mts";
const sourceFiles = [
	generatorPath,
	"fixtures/pi-v1.3.0/audit-1056/pi.mts",
	"fixtures/pi-v1.3.0/audit-1056/support.mts",
];
const manifest = {
	schema: "pike/pi-v1.3.0-audit-evidence/1",
	baseline,
	revision,
	ticket,
	family,
	capturedAt,
	environment,
	generator: {
		path: generatorPath,
		sha256: sha256(readFileSync(path.join(repository, generatorPath))),
		command:
			`PI_CHECKOUT=${checkout} PI_BASELINE=${baseline} ` +
			`${path.join(checkout, "node_modules/.bin/tsx")} ${generatorPath}`,
		sources: sourceFiles.map((source) => ({ path: source, sha256: sha256(readFileSync(path.join(repository, source))) })),
	},
	artifacts: [
		{
			path: artifactName,
			family,
			bytes: Buffer.byteLength(artifactContent),
			sha256: sha256(artifactContent),
			sourceEndpoints,
			scenarios,
		},
	],
};
const manifestContent = `${JSON.stringify(manifest, null, 2)}\n`;
writeFileSync(path.join(bundle, "manifest.json"), manifestContent, "utf8");
console.log(`${artifactName}: ${scenarios.length} scenario(s), ${Buffer.byteLength(artifactContent)} bytes`);
console.log(`manifest.json: ${Buffer.byteLength(manifestContent)} bytes, sha256 ${sha256(manifestContent)}`);
