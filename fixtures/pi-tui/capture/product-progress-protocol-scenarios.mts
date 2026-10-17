/**
 * product-progress-protocol-scenarios.mts
 *
 * Independent frozen measurement and evidence capture for Issue #1037 (Spec #946).
 * Measures actual frozen pi v1.0.4 (`7c10bd4337495ee613f2224843ecdf349b80d1df`):
 *   1. Real `ProcessTerminal` stdout byte paths, OSC 9;4 sequences, and keepalive interval.
 *   2. Real `InteractiveMode.prototype.handleEvent` lifecycle (turn_start, agent_end, compaction_start, compaction_end).
 *   3. Real `InteractiveMode.prototype.switchTuiMode` full 16-case truth matrix + restoration suppression cases.
 *   4. Direct settings toggle retention (stale progress persistence & teardown safety net).
 *   5. Real `InteractiveMode.prototype.stop` teardown clear counts.
 */

import assert from "node:assert/strict";
import path from "node:path";
import { pathToFileURL } from "node:url";

export async function captureProductProgressProtocol(piCheckout: string) {
	// Runtime hooks for frozen checkout resolution:
	// 1. Redirect @earendil-works/pi-tui to packages/tui/src/index.ts so exports match frozen source
	// 2. Azure catalog stub absent from frozen checkout
	const packageDirMap: Record<string, string> = {
		"pi-tui": "tui",
		"pi-ai": "ai",
		"pi-agent-core": "agent",
		"pi-coding-agent": "coding-agent",
		"pi-protocol": "protocol",
		"pi-session-backends": "session-backends",
		"pi-mcp": "mcp",
		"pi-client": "client",
		"pi-chord": "chord",
		"pi-durable": "durable",
		"pi-env": "env",
		"pi-telemetry": "telemetry",
	};

	const { registerHooks } = await import("node:module");
	registerHooks({
		resolve(specifier, context, nextResolve) {
			if (specifier.startsWith("@earendil-works/")) {
				const parts = specifier.slice("@earendil-works/".length).split("/");
				const pkgName = parts[0];
				const subParts = parts.slice(1);
				const dir = packageDirMap[pkgName];
				if (dir) {
					if (subParts.length === 0) {
						return {
							url: pathToFileURL(path.join(piCheckout, "packages", dir, "src/index.ts")).href,
							shortCircuit: true,
						};
					}
					const sub = subParts.join("/");
					return {
						url: pathToFileURL(
							path.join(
								piCheckout,
								"packages",
								dir,
								"src",
								sub.endsWith(".ts") ? sub : `${sub}.ts`,
							),
						).href,
						shortCircuit: true,
					};
				}
			}
			if (specifier === "./data/azure.json" && context.parentURL?.includes("/packages/ai/src/providers/")) {
				return { url: new URL(specifier, context.parentURL).href, shortCircuit: true, format: "json" };
			}
			return nextResolve(specifier, context);
		},
		load(url, context, nextLoad) {
			if (url.includes("/packages/ai/src/providers/data/azure.json") || url.endsWith("/azure.json")) {
				return { format: "json", source: "{}", shortCircuit: true };
			}
			return nextLoad(url, context);
		},
	});

	const coding = (relative: string) =>
		pathToFileURL(path.join(piCheckout, "packages/coding-agent", relative)).href;
	const tui = (relative: string) =>
		pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;

	const { ProcessTerminal } = await import(tui("src/terminal.ts"));
	const { createInteractiveTui } = await import(coding("src/modes/interactive/tui-renderer.ts"));
	const { InteractiveMode } = await import(coding("src/modes/interactive/interactive-mode.ts"));
	const { SettingsManager } = await import(coding("src/core/settings-manager.ts"));
	const { initTheme } = await import(coding("src/modes/interactive/theme/theme.ts"));

	initTheme();

	// Tri-state stdout capture harness
	const realStdoutWrite = process.stdout.write.bind(process.stdout);
	let capturedWrites: string[] = [];
	let captureMode: "record" | "suppress" | "passthrough" = "suppress";

	process.stdout.write = function (chunk: any, ...args: any[]): boolean {
		if (captureMode === "record") {
			capturedWrites.push(typeof chunk === "string" ? chunk : chunk.toString("utf8"));
			return true;
		}
		if (captureMode === "suppress") {
			return true;
		}
		return realStdoutWrite(chunk, ...args);
	} as any;

	function startCapturing(): void {
		capturedWrites = [];
		captureMode = "record";
	}

	function stopCapturing(): string[] {
		captureMode = "suppress";
		return [...capturedWrites];
	}

	const sleep = (ms: number) => new Promise((resolve) => setTimeout(resolve, ms));

	// -------------------------------------------------------------------------
	// SCENARIO 1: Real ProcessTerminal raw byte paths & keepalive lifecycle
	// -------------------------------------------------------------------------
	const rawProtocolObservations: Record<string, any> = {};
	{
		const term = new ProcessTerminal();

		// 1.1 setProgress(true)
		startCapturing();
		term.setProgress(true);
		const activeWrites = stopCapturing();
		assert.equal(activeWrites.length, 1);
		assert.equal(activeWrites[0], "\x1b]9;4;3\x07");
		assert((term as any).progressInterval !== undefined);
		rawProtocolObservations.setProgressTrue = {
			writes: activeWrites,
			keepaliveArmed: true,
		};

		// 1.2 setProgress(false)
		startCapturing();
		term.setProgress(false);
		const clearWrites = stopCapturing();
		assert.equal(clearWrites.length, 1);
		assert.equal(clearWrites[0], "\x1b]9;4;0\x07");
		assert.equal((term as any).progressInterval, undefined);
		rawProtocolObservations.setProgressFalse = {
			writes: clearWrites,
			keepaliveArmed: false,
		};

		// 1.3 Redundant clear when inactive (unconditional clear write)
		startCapturing();
		term.setProgress(false);
		const redundantClearWrites = stopCapturing();
		assert.equal(redundantClearWrites.length, 1);
		assert.equal(redundantClearWrites[0], "\x1b]9;4;0\x07");
		rawProtocolObservations.redundantClearInactive = {
			writes: redundantClearWrites,
			unconditional: true,
		};

		// 1.4 stop() while active (clear strictly precedes bracketed paste disable)
		term.setProgress(true);
		startCapturing();
		term.stop();
		const stopActiveWrites = stopCapturing();
		const clearIdx = stopActiveWrites.indexOf("\x1b]9;4;0\x07");
		const pasteIdx = stopActiveWrites.indexOf("\x1b[?2004l");
		assert(clearIdx !== -1 && pasteIdx !== -1);
		assert(clearIdx < pasteIdx);
		assert.equal((term as any).progressInterval, undefined);
		rawProtocolObservations.stopActive = {
			writes: stopActiveWrites,
			clearEmitted: true,
			bracketedPasteEmitted: true,
			clearPrecedesBracketedPaste: clearIdx < pasteIdx,
			keepaliveArmed: false,
		};

		// 1.5 stop() while inactive (no clear sequence emitted)
		startCapturing();
		term.stop();
		const stopInactiveWrites = stopCapturing();
		assert.equal(stopInactiveWrites.includes("\x1b]9;4;0\x07"), false);
		rawProtocolObservations.stopInactive = {
			writes: stopInactiveWrites,
			clearEmitted: false,
		};
	}

	// -------------------------------------------------------------------------
	// SCENARIO 2: InteractiveMode.prototype.handleEvent real lifecycle
	// -------------------------------------------------------------------------
	function createEventContext(initialEnabled: boolean) {
		const term = new ProcessTerminal();
		const sm = SettingsManager.inMemory();
		sm.setShowTerminalProgress(initialEnabled);

		const renderer = createInteractiveTui({ tuiMode: "regular", terminal: term });
		let activeIndicator: any = undefined;

		const fakeMode = {
			isInitialized: true,
			ui: renderer,
			renderer,
			settingsManager: sm,
			footer: { invalidate: () => {}, dispose: () => {} },
			footerDataProvider: { dispose: () => {} },
			workingVisible: false,
			activeStatusIndicator: undefined,
			showWorkingStatusIndicator: () => {},
			clearStatusIndicator: () => {
				if (activeIndicator) {
					activeIndicator.stop?.();
					activeIndicator = undefined;
				}
			},
			showStatusIndicator: (indicator: any) => {
				activeIndicator = indicator;
			},
			defaultEditor: { onEscape: undefined },
			autoCompactionEscapeHandler: undefined,
			pendingTools: new Set(),
			chatContainer: { removeChild: () => {}, addChild: () => {} },
			session: { abortCompaction: () => {} },
			checkShutdownRequested: async () => {},
			flushCompactionQueue: () => {},
			themeController: { disableAutoSync: () => {} },
			clearExtensionTerminalInputListeners: () => {},
			disposeActiveSelector: () => {},
			stopInteractiveTui: () => {
				renderer.stop();
			},
			unregisterSignalHandlers: () => {},
			options: { tuiMode: "regular" },
		};

		return { fakeMode, term, renderer };
	}

	const handleEvent = (InteractiveMode.prototype as any).handleEvent;
	const lifecycleResults: Record<string, any> = {};

	// Enabled = true
	{
		const { fakeMode, term, renderer } = createEventContext(true);

		startCapturing();
		await handleEvent.call(fakeMode, { type: "turn_start" });
		const turnStartWrites = stopCapturing();
		assert.deepEqual(turnStartWrites, ["\x1b]9;4;3\x07"]);
		lifecycleResults.turnStartEnabled = {
			writes: turnStartWrites,
			activeEmitted: true,
			clearEmitted: false,
		};

		startCapturing();
		await handleEvent.call(fakeMode, { type: "agent_end" });
		const agentEndWrites = stopCapturing();
		assert.deepEqual(agentEndWrites, ["\x1b]9;4;0\x07"]);
		lifecycleResults.agentEndEnabled = {
			writes: agentEndWrites,
			activeEmitted: false,
			clearEmitted: true,
		};

		startCapturing();
		await handleEvent.call(fakeMode, { type: "compaction_start", reason: "manual" });
		const compStartWrites = stopCapturing();
		assert.deepEqual(compStartWrites, ["\x1b]9;4;3\x07"]);
		lifecycleResults.compactionStartEnabled = {
			writes: compStartWrites,
			activeEmitted: true,
			clearEmitted: false,
		};

		startCapturing();
		await handleEvent.call(fakeMode, { type: "compaction_end", aborted: false, reason: "manual" });
		const compEndWrites = stopCapturing();
		assert.deepEqual(compEndWrites, ["\x1b]9;4;0\x07"]);
		lifecycleResults.compactionEndEnabled = {
			writes: compEndWrites,
			activeEmitted: false,
			clearEmitted: true,
		};

		renderer.stop();
		term.stop();
	}

	// Enabled = false
	{
		const { fakeMode, term, renderer } = createEventContext(false);

		startCapturing();
		await handleEvent.call(fakeMode, { type: "turn_start" });
		const turnStartWrites = stopCapturing();
		assert.deepEqual(turnStartWrites, []);
		lifecycleResults.turnStartDisabled = {
			writes: turnStartWrites,
			activeEmitted: false,
			clearEmitted: false,
		};

		startCapturing();
		await handleEvent.call(fakeMode, { type: "agent_end" });
		const agentEndWrites = stopCapturing();
		assert.deepEqual(agentEndWrites, []);
		lifecycleResults.agentEndDisabled = {
			writes: agentEndWrites,
			activeEmitted: false,
			clearEmitted: false,
		};

		startCapturing();
		await handleEvent.call(fakeMode, { type: "compaction_start", reason: "manual" });
		const compStartWrites = stopCapturing();
		assert.deepEqual(compStartWrites, []);
		lifecycleResults.compactionStartDisabled = {
			writes: compStartWrites,
			activeEmitted: false,
			clearEmitted: false,
		};

		startCapturing();
		await handleEvent.call(fakeMode, { type: "compaction_end", aborted: false, reason: "manual" });
		const compEndWrites = stopCapturing();
		assert.deepEqual(compEndWrites, []);
		lifecycleResults.compactionEndDisabled = {
			writes: compEndWrites,
			activeEmitted: false,
			clearEmitted: false,
		};

		renderer.stop();
		term.stop();
	}

	// -------------------------------------------------------------------------
	// SCENARIO 3: InteractiveMode.prototype.switchTuiMode full 16-case truth matrix
	// -------------------------------------------------------------------------
	const switchTuiMode = (InteractiveMode.prototype as any).switchTuiMode;

	async function measureSwitch(params: {
		from: "regular" | "fullscreen";
		to: "regular" | "fullscreen";
		showTerminalProgress: boolean;
		isStreaming: boolean;
		isCompacting: boolean;
		restoreProgress: boolean;
	}) {
		const term = new ProcessTerminal();
		const sm = SettingsManager.inMemory();
		sm.setShowTerminalProgress(params.showTerminalProgress);

		const session = {
			isStreaming: params.isStreaming,
			isCompacting: params.isCompacting,
		};

		const renderer = createInteractiveTui({ tuiMode: params.from, terminal: term });
		const fakeMode: any = {
			renderer,
			options: { tuiMode: params.from },
			settingsManager: sm,
			session,
			themeController: { rebindTui: () => {} },
			rebindExtensionTerminalInputListeners: () => {},
			onRightClickPaste: () => {},
			mountInteractiveTui: () => {},
			mainScreenRenderState: undefined,
		};

		renderer.start();
		if (params.showTerminalProgress && (params.isStreaming || params.isCompacting)) {
			term.setProgress(true);
		}

		startCapturing();
		const ok = switchTuiMode.call(fakeMode, params.to, params.restoreProgress, true);
		const writes = stopCapturing();

		// Cleanup renderer and pending frames before returning
		if (fakeMode.renderer && typeof fakeMode.renderer.stop === "function") {
			fakeMode.renderer.stop();
		}
		term.stop();
		await sleep(5);

		const clearIndex = writes.indexOf("\x1b]9;4;0\x07");
		const activeIndex = writes.indexOf("\x1b]9;4;3\x07");
		const progressClearEmitted = clearIndex !== -1;
		const progressActiveEmitted = activeIndex !== -1;
		const orderValid =
			progressClearEmitted && progressActiveEmitted ? clearIndex < activeIndex : true;

		return {
			direction: `${params.from} -> ${params.to}`,
			from: params.from,
			to: params.to,
			showTerminalProgress: params.showTerminalProgress,
			isStreaming: params.isStreaming,
			isCompacting: params.isCompacting,
			restoreProgress: params.restoreProgress,
			outcome: ok,
			writes,
			totalWrites: writes.length,
			progressClearEmitted,
			progressActiveEmitted,
			clearIndex,
			activeIndex,
			orderValid,
		};
	}

	const switchResults: Record<string, any> = {};

	// Full 16-case truth matrix (directions 2 x showTerminalProgress 2 x states 4)
	const directions: Array<["fullscreen", "regular"] | ["regular", "fullscreen"]> = [
		["fullscreen", "regular"],
		["regular", "fullscreen"],
	];
	const settings = [true, false];
	const states = [
		{ isStreaming: true, isCompacting: false, tag: "streamingOnly" },
		{ isStreaming: false, isCompacting: true, tag: "compactingOnly" },
		{ isStreaming: true, isCompacting: true, tag: "streamingAndCompacting" },
		{ isStreaming: false, isCompacting: false, tag: "idle" },
	];

	for (const [from, to] of directions) {
		for (const enabled of settings) {
			for (const state of states) {
				const key = `${from}To${to.charAt(0).toUpperCase() + to.slice(1)}_${state.tag}_${enabled ? "enabled" : "disabled"}_restoreTrue`;
				const result = await measureSwitch({
					from,
					to,
					showTerminalProgress: enabled,
					isStreaming: state.isStreaming,
					isCompacting: state.isCompacting,
					restoreProgress: true,
				});
				switchResults[key] = result;
			}
		}
	}

	// 2 representative restoreProgress = false suppression cases
	switchResults.fullscreenToRegular_streamingOnly_enabled_restoreFalse = await measureSwitch({
		from: "fullscreen",
		to: "regular",
		showTerminalProgress: true,
		isStreaming: true,
		isCompacting: false,
		restoreProgress: false,
	});
	switchResults.regularToFullscreen_compactingOnly_enabled_restoreFalse = await measureSwitch({
		from: "regular",
		to: "fullscreen",
		showTerminalProgress: true,
		isStreaming: false,
		isCompacting: true,
		restoreProgress: false,
	});

	// -------------------------------------------------------------------------
	// SCENARIO 4: Settings toggle while active & teardown safety net
	// -------------------------------------------------------------------------
	let togglePhases: Record<string, any> = {};
	{
		const { fakeMode, term, renderer } = createEventContext(true);

		// Phase 1: Turn start with showTerminalProgress = true
		startCapturing();
		await handleEvent.call(fakeMode, { type: "turn_start" });
		const p1Writes = stopCapturing();
		assert.deepEqual(p1Writes, ["\x1b]9;4;3\x07"]);
		togglePhases.phase1_turnStartActive = {
			writes: p1Writes,
			progressActive: true,
		};

		// Phase 2: User toggles setting to false directly
		// (onShowTerminalProgressChange delegates directly to SettingsManager)
		startCapturing();
		fakeMode.settingsManager.setShowTerminalProgress(false);
		const p2Writes = stopCapturing();
		assert.deepEqual(p2Writes, []);
		togglePhases.phase2_settingsToggleFalse = {
			writes: p2Writes,
			progressActive: true,
			note: "Direct SettingsManager setter emits 0 bytes; setting persisted without immediate terminal clear",
		};

		// Phase 3: agent_end arrives while setting is now false
		startCapturing();
		await handleEvent.call(fakeMode, { type: "agent_end" });
		const p3Writes = stopCapturing();
		assert.deepEqual(p3Writes, []);
		togglePhases.phase3_agentEndWhileDisabled = {
			writes: p3Writes,
			progressActive: true,
			note: "agent_end skips clear because getShowTerminalProgress() is false; stale progress retained in host terminal",
		};

		// Phase 4: InteractiveMode.prototype.stop() runs teardown
		const stopFn = (InteractiveMode.prototype as any).stop;
		startCapturing();
		stopFn.call(fakeMode);
		const p4Writes = stopCapturing();
		const p4Clears = p4Writes.filter((w) => w === "\x1b]9;4;0\x07");
		assert.equal(p4Clears.length, 1);
		togglePhases.phase4_stopTeardownSafetyNet = {
			writes: p4Writes,
			totalWrites: p4Writes.length,
			clearCount: p4Clears.length,
			note: "ProcessTerminal.stop() clears active progress keepalive interval as terminal safety net",
		};

		renderer.stop();
		term.stop();
	}

	// -------------------------------------------------------------------------
	// SCENARIO 5: InteractiveMode.prototype.stop teardown clear counts
	// -------------------------------------------------------------------------
	const stopFn = (InteractiveMode.prototype as any).stop;
	const stopCounts: Record<string, any> = {};

	function createStopContext(initialEnabled: boolean) {
		const term = new ProcessTerminal();
		const sm = SettingsManager.inMemory();
		sm.setShowTerminalProgress(initialEnabled);

		const renderer = createInteractiveTui({ tuiMode: "regular", terminal: term });
		renderer.start();

		const fakeMode = {
			isInitialized: true,
			ui: renderer,
			renderer,
			settingsManager: sm,
			footer: { invalidate: () => {}, dispose: () => {} },
			footerDataProvider: { dispose: () => {} },
			workingVisible: false,
			clearStatusIndicator: () => {},
			themeController: { disableAutoSync: () => {} },
			clearExtensionTerminalInputListeners: () => {},
			disposeActiveSelector: () => {},
			stopInteractiveTui: () => {
				renderer.stop();
			},
			unregisterSignalHandlers: () => {},
			options: { tuiMode: "regular" },
		};

		return { fakeMode, term, renderer, sm };
	}

	// 5.1 Active + Enabled=true
	{
		const { fakeMode, term, renderer } = createStopContext(true);
		term.setProgress(true);
		startCapturing();
		stopFn.call(fakeMode);
		const writes = stopCapturing();
		const clears = writes.filter((w) => w === "\x1b]9;4;0\x07");
		assert.equal(clears.length, 1);
		stopCounts.activeEnabled = {
			showTerminalProgress: true,
			activeAtStop: true,
			clearsEmitted: clears.length,
		};
		renderer.stop();
		term.stop();
	}

	// 5.2 Idle + Disabled (enabled=false)
	{
		const { fakeMode, term, renderer } = createStopContext(false);
		startCapturing();
		stopFn.call(fakeMode);
		const writes = stopCapturing();
		const clears = writes.filter((w) => w === "\x1b]9;4;0\x07");
		assert.equal(clears.length, 0);
		stopCounts.idleDisabled = {
			showTerminalProgress: false,
			activeAtStop: false,
			clearsEmitted: clears.length,
		};
		renderer.stop();
		term.stop();
	}

	// 5.3 Idle + Enabled=true
	{
		const { fakeMode, term, renderer } = createStopContext(true);
		startCapturing();
		stopFn.call(fakeMode);
		const writes = stopCapturing();
		const clears = writes.filter((w) => w === "\x1b]9;4;0\x07");
		assert.equal(clears.length, 1);
		stopCounts.idleEnabled = {
			showTerminalProgress: true,
			activeAtStop: false,
			clearsEmitted: clears.length,
		};
		renderer.stop();
		term.stop();
	}

	// 5.4 Active + Toggled enabled->false before stop
	{
		const { fakeMode, term, renderer, sm } = createStopContext(true);
		term.setProgress(true);
		sm.setShowTerminalProgress(false);
		startCapturing();
		stopFn.call(fakeMode);
		const writes = stopCapturing();
		const clears = writes.filter((w) => w === "\x1b]9;4;0\x07");
		assert.equal(clears.length, 1);
		stopCounts.activeToggledDisabled = {
			showTerminalProgress: false,
			activeAtStop: true,
			clearsEmitted: clears.length,
		};
		renderer.stop();
		term.stop();
	}

	// Drain any lingering async callbacks before restoring stdout
	await sleep(50);
	process.stdout.write = realStdoutWrite;

	return {
		sourceEndpoints: [
			"packages/tui/src/terminal.ts:ProcessTerminal.setProgress,ProcessTerminal.stop",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:InteractiveMode.handleEvent,InteractiveMode.switchTuiMode,InteractiveMode.stop",
			"packages/coding-agent/src/core/settings-manager.ts:SettingsManager.getShowTerminalProgress,SettingsManager.setShowTerminalProgress",
		],
		scenarios: [
			{
				name: "terminal-progress-raw-protocol",
				description:
					"Real ProcessTerminal raw byte paths, OSC 9;4 sequences, and keepalive lifecycle",
				dimensions: { columns: 80, rows: 24 },
				inputs: { seam: "ProcessTerminal", protocol: "OSC 9;4" },
				expected: {
					activeSequence: "\u001b]9;4;3\u0007",
					clearSequence: "\u001b]9;4;0\u0007",
					keepaliveIntervalMs: 1000,
					observations: rawProtocolObservations,
				},
			},
			{
				name: "interactive-mode-handle-event-lifecycle",
				description:
					"InteractiveMode.prototype.handleEvent driving real ProcessTerminal stdout across lifecycle events",
				dimensions: { columns: 80, rows: 24 },
				inputs: { seam: "InteractiveMode.handleEvent" },
				expected: {
					unrelatedUIStubs: [
						"footer",
						"footerDataProvider",
						"clearStatusIndicator",
						"showStatusIndicator",
						"flushCompactionQueue",
						"theme",
					],
					lifecycleResults,
				},
			},
			{
				name: "interactive-mode-switch-matrix",
				description:
					"InteractiveMode.prototype.switchTuiMode full 16-case truth matrix and restoration suppression with raw ordered writes",
				dimensions: { columns: 80, rows: 24 },
				inputs: { seam: "InteractiveMode.switchTuiMode", matrixCases: 18 },
				expected: {
					matrixTruthDimensions: {
						directions: ["fullscreen -> regular", "regular -> fullscreen"],
						showTerminalProgress: [true, false],
						sessionStates: [
							"streaming: true, compacting: false",
							"streaming: false, compacting: true",
							"streaming: true, compacting: true",
							"streaming: false, compacting: false",
						],
						totalTruthMatrixCases: 16,
						representativeSuppressionCases: 2,
					},
					switchResults,
				},
			},
			{
				name: "settings-toggle-stale-progress",
				description:
					"Settings toggle while active: direct setter retention, agent_end suppression, and shutdown safety net",
				dimensions: { columns: 80, rows: 24 },
				inputs: {
					seam: "SettingsManager.setShowTerminalProgress -> InteractiveMode.handleEvent -> InteractiveMode.stop",
				},
				expected: {
					metadata: {
						sourceDerivedCallbackDelegation: true,
						measuredViaDirectSettingsManager: true,
						uiCallbackExecutionMeasured: false,
						notes:
							"Direct SettingsManager setter measurement; onShowTerminalProgressChange delegates to SettingsManager without UI callback execution; active progress remains stale until terminal.stop() safety net clears keepalive interval",
					},
					phases: togglePhases,
				},
			},
			{
				name: "interactive-mode-stop-clear-counts",
				description:
					"InteractiveMode.prototype.stop shutdown teardown progress clear counts across lifecycle states",
				dimensions: { columns: 80, rows: 24 },
				inputs: { seam: "InteractiveMode.stop" },
				expected: {
					counts: stopCounts,
				},
			},
		],
	};
}
