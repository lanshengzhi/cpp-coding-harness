#!/usr/bin/env node

import { createHash } from "node:crypto";
import { mkdirSync, readFileSync, writeFileSync } from "node:fs";
import { execFileSync } from "node:child_process";
import { dirname, join, relative, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";

// The named-baseline registry is the single authority for pi revisions (ADR 0065).
// Resolved relative to this file, never relative to --fixture-root, so a copied fixture
// tree still verifies against the repository's policy.
const REGISTRY_PATH = join(
	fileURLToPath(new URL(".", import.meta.url)),
	"..",
	"..",
	"fixtures",
	"pi-ai",
	"baselines.json",
);
const REGISTRY_SCHEMA = "cpp-coding-harness/pi-ai-baselines/1";

function loadBaselines() {
	const registry = JSON.parse(readFileSync(REGISTRY_PATH, "utf8"));
	if (registry.schema !== REGISTRY_SCHEMA) {
		throw new Error(`unexpected baseline registry schema: ${REGISTRY_PATH}`);
	}
	if (!registry.baselines || typeof registry.baselines !== "object") {
		throw new Error(`baseline registry has no baselines: ${REGISTRY_PATH}`);
	}
	for (const [name, entry] of Object.entries(registry.baselines)) {
		if (typeof entry?.revision !== "string" || entry.revision.length !== 40) {
			throw new Error(`baseline ${name} must record a full 40-character revision`);
		}
	}
	if (!(registry.default_baseline in registry.baselines)) {
		throw new Error(`default_baseline is not registered: ${REGISTRY_PATH}`);
	}
	return registry;
}

function resolveBaseline(name) {
	const registry = loadBaselines();
	const selected = name ?? registry.default_baseline;
	const entry = registry.baselines[selected];
	if (!entry) {
		throw new Error(
			`unknown baseline ${JSON.stringify(selected)}; registered baselines: ` +
				Object.keys(registry.baselines).sort().join(", "),
		);
	}
	return { registry, name: selected, ...entry };
}

// The historical bundle occupies the fixture root (bundle_path ""), so a later bundle's
// directory nests inside it. Ownership therefore resolves by the LONGEST matching bundle
// path, not by "is this path inside some other bundle".
function owningBaseline(registry, target, fixtureRoot) {
	const relativeTarget = relative(fixtureRoot, target);
	let owner = null;
	for (const [name, entry] of Object.entries(registry.baselines)) {
		const bundlePath = entry.bundle_path;
		const matches =
			bundlePath === ""
				? true
				: relativeTarget === bundlePath || relativeTarget.startsWith(`${bundlePath}/`);
		if (!matches) {
			continue;
		}
		if (owner === null || bundlePath.length > owner.entry.bundle_path.length) {
			owner = { name, entry };
		}
	}
	return owner;
}

function assertWritesOwnBundle(registry, target, baseline, fixtureRoot) {
	const owner = owningBaseline(registry, target, fixtureRoot);
	if (owner && owner.name !== baseline.name) {
		throw new Error(
			`refusing to write ${target}: it belongs to bundle ${JSON.stringify(owner.name)}, ` +
				`but this run records baseline ${JSON.stringify(baseline.name)}`,
		);
	}
}

function parseArgs(argv) {
	const args = {
		fixtureRoot: resolve("fixtures/pi-ai"),
		output: undefined,
		piRoot: undefined,
		baseline: undefined,
	};
	for (let index = 0; index < argv.length; index += 1) {
		const argument = argv[index];
		if (argument === "--fixture-root") {
			args.fixtureRoot = resolve(argv[++index]);
		} else if (argument === "--output") {
			args.output = resolve(argv[++index]);
		} else if (argument === "--pi-root") {
			args.piRoot = resolve(argv[++index]);
		} else if (argument === "--baseline") {
			args.baseline = argv[++index];
		} else {
			throw new Error(`unknown argument: ${argument}`);
		}
	}
	if (!args.piRoot) {
		throw new Error("--pi-root is required");
	}
	return args;
}

function gitRevision(piRoot) {
	return execFileSync("git", ["-C", piRoot, "rev-parse", "HEAD"], {
		encoding: "utf8",
	}).trim();
}

function readJson(path) {
	return JSON.parse(readFileSync(path, "utf8"));
}

function sha256(value) {
	return createHash("sha256").update(value).digest("hex");
}

function sortKeys(value) {
	if (Array.isArray(value)) return value.map(sortKeys);
	if (value && typeof value === "object") {
		return Object.fromEntries(
			Object.entries(value)
				.sort(([left], [right]) => left.localeCompare(right))
				.map(([key, child]) => [key, sortKeys(child)]),
		);
	}
	return value;
}

function sourceMeasurement(path, root) {
	const source = readFileSync(path, "utf8");
	return {
		path: relative(root, path),
		bytes: Buffer.byteLength(source),
		lines: source.split("\n").length - 1,
	};
}

function tool() {
	return {
		name: "lookup",
		description: "Look up a value",
		parameters: {
			type: "object",
			properties: { q: { type: "string" } },
			required: ["q"],
		},
	};
}

function context() {
	return {
		systemPrompt: "system",
		messages: [
			{
				role: "user",
				content: [{ type: "text", text: "hello" }],
				timestamp: 1,
			},
		],
		tools: [tool()],
	};
}

function chatCompletionSse() {
	return [
		'data: {"id":"probe","object":"chat.completion.chunk","created":0,"model":"probe","choices":[{"index":0,"delta":{"role":"assistant","content":"ok"},"finish_reason":null}]}',
		'data: {"id":"probe","object":"chat.completion.chunk","created":0,"model":"probe","choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":1,"completion_tokens":1,"total_tokens":2}}',
		"data: [DONE]",
		"",
	].join("\n\n");
}

function anthropicSse() {
	return [
		'event: message_start\ndata: {"type":"message_start","message":{"id":"msg_probe","type":"message","role":"assistant","content":[],"model":"probe","stop_reason":null,"stop_sequence":null,"usage":{"input_tokens":1}}}',
		'event: message_delta\ndata: {"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":1}}',
		'event: message_stop\ndata: {"type":"message_stop"}',
		"",
	].join("\n\n");
}

function responsesSse() {
	return [
		'event: response.created\ndata: {"type":"response.created","response":{"id":"resp_probe","object":"response","status":"in_progress","model":"probe","output":[]}}',
		'event: response.output_text.delta\ndata: {"type":"response.output_text.delta","item_id":"msg_probe","output_index":0,"content_index":0,"delta":"ok"}',
		'event: response.completed\ndata: {"type":"response.completed","response":{"id":"resp_probe","status":"completed","output":[],"usage":{"input_tokens":1,"output_tokens":1,"total_tokens":2}}}',
		"",
	].join("\n\n");
}

function captureFetch(responseBody, capture) {
	return async (input, init) => {
		const rawBody = typeof init?.body === "string" ? init.body : "";
		capture.transport_calls += 1;
		capture.url = String(input);
		capture.method = init?.method ?? "GET";
		capture.raw_body = rawBody;
		capture.body = rawBody ? JSON.parse(rawBody) : null;
		return new Response(responseBody, {
			status: 200,
			headers: { "content-type": "text/event-stream" },
		});
	};
}

async function runStream({ label, streamSimple, model, options, responseBody, sourceRoot }) {
	const capture = {
		label,
		on_payload_seen: false,
		transport_calls: 0,
		url: null,
		method: null,
		raw_body: "",
		body: null,
		payload: null,
	};
	const stream = streamSimple(model, (await import(pathToFileURL(join(sourceRoot, "src/utils/transcript.ts")))).normalizeContext(context()), {
		...options,
		fetch: captureFetch(responseBody, capture),
		onPayload: (payload) => {
			capture.on_payload_seen = true;
			capture.payload = payload;
		},
	});
	const result = await stream.result();
	capture.stream_stop_reason = result.stopReason;
	capture.stream_error = result.errorMessage ?? null;
	capture.raw_body_sha256 = sha256(capture.raw_body);
	return capture;
}

function bundleDirectory(fixtureRoot, baseline) {
	return baseline.bundle_path === ""
		? fixtureRoot
		: join(fixtureRoot, baseline.bundle_path);
}

function providerModel(bundleRoot, provider, api, modelId) {
	// The catalog must come from the selected baseline's own bundle, never the repository
	// root: reading v0.87.1 models under a v1.0.0 report would be false evidence.
	const catalog = readJson(join(bundleRoot, "models", "providers", `${provider}.json`));
	const model = catalog[api]?.[modelId];
	if (!model) throw new Error(`missing ${provider}/${modelId} in provenance fixture`);
	return model;
}

function assertEqual(actual, expected, label) {
	if (actual !== expected) {
		throw new Error(`${label}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
	}
}

async function main() {
	const args = parseArgs(process.argv.slice(2));
	// The expected revision is the registry entry for the baseline under test, not a
	// module constant, so checkout, guard, and artifact cannot drift apart (ADR 0065).
	const baseline = resolveBaseline(args.baseline);
	const revision = gitRevision(args.piRoot);
	assertEqual(revision, baseline.revision, `pi revision (baseline ${baseline.name})`);

	const sourceRoot = resolve(args.piRoot, "packages/ai");
	const [completionsModule, anthropicModule, responsesModule] = await Promise.all([
		import(pathToFileURL(join(sourceRoot, "src/api/openai-completions.ts"))),
		import(pathToFileURL(join(sourceRoot, "src/api/anthropic-messages.ts"))),
		import(pathToFileURL(join(sourceRoot, "src/api/openai-responses.ts"))),
	]);

	const bundleRoot = bundleDirectory(args.fixtureRoot, baseline);
	const completionsModel = providerModel(bundleRoot, "deepseek", "openai-completions", "deepseek-flash");
	const anthropicModel = providerModel(bundleRoot, "opencode-go", "anthropic-messages", "minimax-m3");
	const responsesModel = providerModel(bundleRoot, "openai", "openai-responses", "gpt-4");

	const completions = await runStream({
		label: "openai-completions",
		streamSimple: completionsModule.streamSimple,
		model: completionsModel,
		options: {
			apiKey: "dummy-probe-key",
			cacheRetention: "none",
			maxTokens: 4096,
			reasoning: "high",
		},
		responseBody: chatCompletionSse(),
		sourceRoot,
	});
	const anthropicBudget = await runStream({
		label: "anthropic-budget-thinking",
		streamSimple: anthropicModule.streamSimple,
		model: anthropicModel,
		options: {
			apiKey: "dummy-probe-key",
			cacheRetention: "none",
			maxTokens: 4096,
			reasoning: "high",
			thinkingBudgets: { high: 2048 },
		},
		responseBody: anthropicSse(),
		sourceRoot,
	});
	const responsesStrictFalse = await runStream({
		label: "responses-strict-false",
		streamSimple: responsesModule.streamSimple,
		model: responsesModel,
		options: {
			apiKey: "dummy-probe-key",
			cacheRetention: "none",
			maxTokens: 64,
		},
		responseBody: responsesSse(),
		sourceRoot,
	});

	assertEqual(completions.transport_calls, 1, "completions transport calls");
	assertEqual(completions.on_payload_seen, true, "completions onPayload");
	assertEqual(anthropicBudget.transport_calls, 1, "anthropic transport calls");
	assertEqual(anthropicBudget.on_payload_seen, true, "anthropic onPayload");
	assertEqual(responsesStrictFalse.transport_calls, 1, "responses transport calls");
	assertEqual(responsesStrictFalse.on_payload_seen, true, "responses onPayload");

	const report = {
		schema: "cpp-coding-harness/issue-758-t0-cost-probe/1",
		probe: {
			baseline: baseline.name,
			pi_revision: revision,
			transport: "in-process fake fetch; no provider network or credentials",
			context: "one system prompt, one user text, one ordinary JSON-schema tool",
		},
		measured_source_surface: [
			sourceMeasurement(join(sourceRoot, "src/api/openai-completions.ts"), args.piRoot),
			sourceMeasurement(join(sourceRoot, "src/api/anthropic-messages.ts"), args.piRoot),
			sourceMeasurement(join(sourceRoot, "src/api/openai-responses.ts"), args.piRoot),
			sourceMeasurement(join(sourceRoot, "src/api/openai-responses-shared.ts"), args.piRoot),
		],
		cases: {
			"openai-completions": completions,
			"anthropic-budget-thinking": anthropicBudget,
			"responses-strict-false": responsesStrictFalse,
		},
		limitations: [
			"The probe measures real baseline request construction and the current C++ payload seam; it is not a production adapter implementation.",
			"Fake SSE only proves request construction and parser entry; provider acceptance and billing require manual live validation with credentials.",
			"Source byte/line counts are an observed scope indicator, not an engineering-time estimate.",
		],
	};
	const output = `${JSON.stringify(sortKeys(report), null, 2)}\n`;
	if (args.output) {
		// Refuse to write a probe into a bundle owned by another baseline.
		assertWritesOwnBundle(baseline.registry, args.output, baseline, args.fixtureRoot);
		mkdirSync(dirname(args.output), { recursive: true });
		writeFileSync(args.output, output, "utf8");
	}
	else process.stdout.write(output);
}

main().catch((error) => {
	console.error(error instanceof Error ? error.message : String(error));
	process.exitCode = 1;
});
