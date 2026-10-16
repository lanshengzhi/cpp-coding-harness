import path from "node:path";
import { pathToFileURL } from "node:url";

/// Independent CLI TUI-mode observations from frozen pi-v1.0.4 (#1017): the
/// `--tui-mode` branch of `parseArgs`, its help line, the
/// `getTuiMode`/`setTuiMode` settings read/write, and the resolution
/// expression `options.tuiMode ?? this.settingsManager.getTuiMode()` applied
/// to the observed operand pairs. Expectations come from the frozen checkout
/// only; nothing here is derived from Pike.
///
/// The pane carries a nominal terminal context (`dimensions`): the CLI parse
/// and the settings reads are terminal-independent, so the value only names the
/// terminal the resolved mode would drive.
export async function captureCliTuiMode(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/coding-agent");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const args = await import(source("src/cli/args.ts"));
	const { SettingsManager } = await import(source("src/core/settings-manager.ts"));

	/// A storage double holding one global/project JSON document, so the
	/// settings observations load and persist through the real
	/// `tryLoadFromStorage`/`save` paths without touching a filesystem.
	const storageWith = (initialGlobal: string | undefined) => {
		let global = initialGlobal;
		let project: string | undefined;
		return {
			readGlobal: () => global,
			readProject: () => project,
			withLock: (scope: "global" | "project", fn: (current: string | undefined) => string | undefined) => {
				const current = scope === "global" ? global : project;
				const next = fn(current);
				if (next !== undefined) {
					if (scope === "global") global = next;
					else project = next;
				}
			},
		};
	};

	/// Every `parseArgs` field the mode branch can move: the mode itself, the
	/// positional messages it may or may not consume, the print flag a
	/// `-`-prefixed token keeps, the collected diagnostics, and the unknown-flag
	/// bucket every other spelling of the option lands in.
	const parseCase = (name: string, argv: string[]) => {
		const parsed: any = args.parseArgs([...argv]);
		return {
			name,
			argv,
			expected: {
				tuiMode: parsed.tuiMode ?? null,
				messages: [...parsed.messages],
				print: parsed.print === true,
				diagnostics: parsed.diagnostics.map((entry: any) => ({ type: entry.type, message: entry.message })),
				unknownFlags: Object.fromEntries([...parsed.unknownFlags.entries()].sort()),
			},
		};
	};

	let helpOutput = "";
	const originalWrite = process.stdout.write.bind(process.stdout);
	(process.stdout.write as any) = (chunk: any) => {
		helpOutput += String(chunk);
		return true;
	};
	args.printHelp();
	(process.stdout.write as any) = originalWrite;
	const helpLines = helpOutput.split("\n").filter((line) => line.includes("--tui-mode"));

	/// The stored `tuiMode` values a settings document can carry, including one
	/// outside the union: `getTuiMode` compares against `"regular"` only. Each
	/// read records both the document it loaded and the raw value inside it.
	const storedValues: (string | undefined)[] = [undefined, "regular", "fullscreen", "bogus"];
	const storedObservations = storedValues.map((stored) => {
		const storedFile = stored === undefined ? null : JSON.stringify({ tuiMode: stored });
		const storage = storageWith(storedFile ?? undefined);
		const manager = SettingsManager.fromStorage(storage, { projectTrusted: true });
		return { storedFile, storedValue: stored ?? null, resolved: manager.getTuiMode() };
	});
	const resolvedByStoredValue = new Map<string, string>(
		storedObservations.map((observation) => [observation.storedValue ?? "unset", observation.resolved]),
	);

	const writeStorage = storageWith(JSON.stringify({ theme: "dark" }));
	const writeManager = SettingsManager.fromStorage(writeStorage, { projectTrusted: true });
	writeManager.setTuiMode("regular");
	await writeManager.flush();
	const persistedGlobal = JSON.parse(writeStorage.readGlobal() as string);

	/// The resolution expression is frozen at
	/// `packages/coding-agent/src/modes/interactive/interactive-mode.ts:604`
	/// (`const tuiMode = options.tuiMode ?? this.settingsManager.getTuiMode()`).
	/// Reaching it needs the full interactive runtime, so this replay records
	/// the expression applied to the two observed operands — the `tuiMode`
	/// `parseArgs` assigned and the `getTuiMode()` value of the *same* stored
	/// document — and claims no execution of `runInteractiveMode` itself.
	const precedenceInputs = [
		{ storedFile: JSON.stringify({ tuiMode: "fullscreen" }), "--tui-mode": "regular" },
		{ storedFile: JSON.stringify({ tuiMode: "regular" }), "--tui-mode": "fullscreen" },
		{ storedFile: JSON.stringify({ tuiMode: "regular" }), "--tui-mode": null },
		{ storedFile: undefined, "--tui-mode": null },
		{ storedFile: JSON.stringify({ tuiMode: "bogus" }), "--tui-mode": "bogus" },
	] as const;
	const precedence = precedenceInputs.map((entry) => {
		const argv = entry["--tui-mode"] === null ? [] : ["--tui-mode", entry["--tui-mode"] as string];
		const cliTuiMode: string | null = args.parseArgs(argv).tuiMode ?? null;
		const storedKey = entry.storedFile === undefined ? "unset" : (JSON.parse(entry.storedFile).tuiMode as string);
		const settingsTuiMode = resolvedByStoredValue.get(storedKey) ?? null;
		return {
			argv,
			storedFile: entry.storedFile ?? null,
			cliTuiMode,
			settingsTuiMode,
			resolved: cliTuiMode ?? settingsTuiMode,
		};
	});

	return {
		sourceEndpoints: [
			"packages/coding-agent/src/cli/args.ts:219",
			"packages/coding-agent/src/cli/args.ts:333",
			"packages/coding-agent/src/core/settings-manager.ts:1348",
			"packages/coding-agent/src/core/settings-manager.ts:1352",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:604",
		],
		scenarios: [
			{
				name: "tui-mode-flag-accepts-the-two-roots",
				dimensions: { columns: 80, rows: 24 },
				inputs: {
					cases: [
						["--tui-mode", "regular", "hello"],
						["--tui-mode", "fullscreen"],
						["--tui-mode=regular"],
						[],
					],
				},
				expected: {
					cases: [
						parseCase("regular-then-message", ["--tui-mode", "regular", "hello"]),
						parseCase("fullscreen-alone", ["--tui-mode", "fullscreen"]),
						// The frozen branch matches the exact token `--tui-mode`, so the
						// inline spelling never reaches it: the mode stays unset and the
						// token lands in `unknownFlags` with no mode diagnostic.
						parseCase("inline-regular-lands-in-unknown-flags", ["--tui-mode=regular"]),
						parseCase("absent-flag", []),
					],
				},
			},
			{
				name: "tui-mode-flag-rejects-a-missing-value-without-consuming",
				dimensions: { columns: 80, rows: 24 },
				inputs: {
					cases: [["--tui-mode"], ["--tui-mode", "--print", "hello"], ["--tui-mode", "--tui-mode", "regular"]],
				},
				expected: {
					cases: [
						parseCase("bare-flag", ["--tui-mode"]),
						// A `-`-prefixed token is never taken as the mode: `--print`
						// stays a flag and `hello` stays a positional message.
						parseCase("option-like-value-stays-an-option", ["--tui-mode", "--print", "hello"]),
						// One diagnostic and a resolved mode: the loop collects and
						// continues instead of returning after the rejection.
						parseCase("repeat-after-rejection", ["--tui-mode", "--tui-mode", "regular"]),
					],
				},
			},
			{
				name: "tui-mode-flag-rejects-an-unknown-mode-after-consuming-it",
				dimensions: { columns: 80, rows: 24 },
				inputs: {
					cases: [["--tui-mode", "bogus"], ["--tui-mode", "bogus", "hello"], ["--tui-mode", "regular", "--tui-mode", "bogus"]],
				},
				expected: {
					cases: [
						parseCase("unknown-mode", ["--tui-mode", "bogus"]),
						// The rejected value is consumed, so the next token is still a
						// positional message.
						parseCase("unknown-mode-then-message", ["--tui-mode", "bogus", "hello"]),
						// A rejected repeat never clears a mode the earlier token set.
						parseCase("accepted-then-rejected", ["--tui-mode", "regular", "--tui-mode", "bogus"]),
					],
				},
			},
			{
				name: "tui-mode-help-line",
				dimensions: { columns: 80, rows: 24 },
				inputs: { helpCapture: "process.stdout printHelp()" },
				expected: { lines: helpLines },
			},
			{
				name: "settings-tui-mode-read-normalization",
				dimensions: { columns: 80, rows: 24 },
				inputs: { storedFiles: storedValues.map((stored) => (stored === undefined ? null : JSON.stringify({ tuiMode: stored }))) },
				expected: { reads: storedObservations },
			},
			{
				name: "settings-tui-mode-write-persists-the-global-key",
				dimensions: { columns: 80, rows: 24 },
				inputs: { initialGlobal: JSON.stringify({ theme: "dark" }), setTuiMode: "regular" },
				expected: {
					persistedGlobal,
					resolved: writeManager.getTuiMode(),
					errors: writeManager.drainErrors().map((entry: any) => String(entry.error)),
				},
			},
			{
				name: "tui-mode-flag-over-settings-precedence",
				dimensions: { columns: 80, rows: 24 },
				inputs: {
					expression: "options.tuiMode ?? this.settingsManager.getTuiMode()",
					expressionEndpoint: "packages/coding-agent/src/modes/interactive/interactive-mode.ts:604",
					note: "operands observed from frozen parseArgs/getTuiMode; the expression itself is not executable without the full interactive runtime",
					cases: precedence.map(({ argv, storedFile }) => ({ argv, storedFile })),
				},
				expected: { resolutions: precedence },
			},
		],
	};
}
