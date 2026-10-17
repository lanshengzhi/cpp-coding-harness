// Scenarios for artifact 34: fullscreen-exit-protocol.json
// Measures exact phase boundaries, raw writes, control tokens, cursor coordinates,
// and paired contrast between frozen stop and forced renderNow on regular exit.

import assert from "node:assert/strict";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { registerHooks } from "node:module";

export async function captureFullscreenExitProtocol(piCheckout: string) {
	const directories: Record<string, string> = {
		"pi-tui": "tui", "pi-ai": "ai", "pi-agent-core": "agent", "pi-coding-agent": "coding-agent",
		"pi-protocol": "protocol", "pi-session-backends": "session-backends", "pi-mcp": "mcp",
		"pi-client": "client", "pi-chord": "chord", "pi-durable": "durable", "pi-env": "env", "pi-telemetry": "telemetry",
	};
	registerHooks({
		resolve(specifier, context, next) {
			if (specifier.startsWith("@earendil-works/")) {
				const [name, ...parts] = specifier.slice("@earendil-works/".length).split("/");
				if (directories[name]) {
					const relative = parts.length ? parts.join("/") : "index.ts";
					return { url: pathToFileURL(path.join(piCheckout, "packages", directories[name], "src",
						relative.endsWith(".ts") ? relative : `${relative}.ts`)).href, shortCircuit: true };
				}
			}
			if (specifier === "./data/azure.json" && context.parentURL?.includes("/packages/ai/src/providers/")) {
				return { url: new URL(specifier, context.parentURL).href, shortCircuit: true, format: "json" };
			}
			return next(specifier, context);
		},
		load(url, context, next) {
			if (url.endsWith("/providers/data/azure.json")) return { format: "json", source: "{}", shortCircuit: true };
			return next(url, context);
		},
	});

	const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages", relative)).href;
	const { InteractiveMode } = await import(source("coding-agent/src/modes/interactive/interactive-mode.ts"));
	const { createInteractiveTui, createInteractiveTuiReference } = await import(source("coding-agent/src/modes/interactive/tui-renderer.ts"));
	const { createChatViewport } = await import(source("coding-agent/src/modes/interactive/chat-viewport.ts"));
	const { Container, Text } = await import(source("tui/src/index.ts"));
	const { VirtualTerminal } = await import(source("tui/test/virtual-terminal.ts"));
	const { isViewportTUI } = await import(source("tui/src/tui.ts"));

	class InstrumentedTerminal extends (VirtualTerminal as any) {
		writes: string[] = [];
		startCount = 0;
		stopCount = 0;
		override write(data: string): void {
			this.writes.push(data);
			super.write(data);
		}
		override start(input: (data: string) => void, resize: () => void): void {
			this.startCount++;
			super.start(input, resize);
		}
		override stop(): void {
			this.stopCount++;
			super.stop();
		}
	}

	function extractControlTokens(rawText: string): string[] {
		const matches = rawText.match(/(\x1b\[\??[0-9;]*[a-zA-Z]|\x1b\][^\x07]*\x07|\r\n|\r|\n)/g);
		return matches ? Array.from(matches) : [];
	}

	interface PhaseBoundaryRecord {
		name: string;
		writeIndex: number;
		startCount: number;
		stopCount: number;
		renderNowCount: number;
	}

	async function replaySnapshot(
		columns: number,
		rows: number,
		allWrites: string[],
		boundary: PhaseBoundaryRecord,
		prevBoundary?: PhaseBoundaryRecord,
	) {
		const replayVt = new (VirtualTerminal as any)(columns, rows);
		for (let i = 0; i < boundary.writeIndex; i++) {
			replayVt.write(allWrites[i]);
		}
		await replayVt.flush();

		const prevIndex = prevBoundary ? prevBoundary.writeIndex : 0;
		const phaseRawWrites = allWrites.slice(prevIndex, boundary.writeIndex);
		const phaseCombined = phaseRawWrites.join("");
		const phaseControlTokens = extractControlTokens(phaseCombined);

		const buf = replayVt.xterm.buffer.active;
		const screenWithSpaces: string[] = [];
		const screenTrimmed: string[] = [];
		for (let r = 0; r < rows; r++) {
			const line = buf.getLine(buf.viewportY + r);
			screenWithSpaces.push(line ? line.translateToString(false) : "");
			screenTrimmed.push(line ? line.translateToString(true) : "");
		}

		const scrollbackTrimmed: string[] = [];
		const scrollbackWithSpaces: string[] = [];
		for (let r = 0; r < buf.length; r++) {
			const line = buf.getLine(r);
			const trimmed = line ? line.translateToString(true) : "";
			scrollbackTrimmed.push(trimmed);
			scrollbackWithSpaces.push(line ? line.translateToString(false) : "");
		}

		const totalBytes = allWrites.slice(0, boundary.writeIndex).reduce((sum, w) => sum + Buffer.byteLength(w, "utf8"), 0);

		return {
			phase: boundary.name,
			activeBuffer: buf.type as ("normal" | "alternate"),
			cursor: {
				x: buf.cursorX,
				y: buf.cursorY,
				baseY: buf.baseY,
				viewportY: buf.viewportY,
			},
			cursorHidden: !!(replayVt.xterm as any)._core?.coreService?.isCursorHidden,
			modes: {
				bracketedPasteMode: !!replayVt.xterm.modes.bracketedPasteMode,
				alternateScreen: buf.type === "alternate",
				wraparoundMode: !!replayVt.xterm.modes.wraparoundMode,
			},
			screenLinesWithSpaces: screenWithSpaces,
			screenLinesTrimmed: screenTrimmed,
			scrollbackLinesWithSpaces: scrollbackWithSpaces,
			scrollbackLinesTrimmed: scrollbackTrimmed,
			startCount: boundary.startCount,
			stopCount: boundary.stopCount,
			renderNowCount: boundary.renderNowCount,
			rawWritesTotalCount: boundary.writeIndex,
			rawWritesTotalBytes: totalBytes,
			phaseRawWrites,
			phaseControlTokens,
		};
	}

	async function runCase(options: {
		name: string;
		initialMode: "regular" | "fullscreen";
		fullscreenExitOutput: "transcript" | "resume-hint";
		columns: number;
		rows: number;
		initialMainBuffer: string[];
		documentLines: string[];
		editorText: string;
		pendingMutation?: string[];
		forcedRenderNowBeforeStop?: boolean;
	}) {
		const term = new InstrumentedTerminal(options.columns, options.rows);
		term.start(() => {}, () => {});

		for (const line of options.initialMainBuffer) {
			term.write(`${line}\r\n`);
		}

		const docContainer = new Container();
		for (const line of options.documentLines) {
			docContainer.addChild(new Text(line, 0, 0));
		}
		const pendingContainer = new Container();
		const statusContainer = new Container();
		const editorContainer = new Container();
		editorContainer.addChild(new Text(options.editorText, 0, 0));
		const footerContainer = new Container();

		const viewport = createChatViewport({
			document: docContainer,
			pendingMessages: pendingContainer,
			status: statusContainer,
			editor: editorContainer,
			footer: footerContainer,
			scrollbar: "hidden",
		});

		const regions = [docContainer, pendingContainer, statusContainer, editorContainer, footerContainer];

		const context = Object.assign(Object.create(InteractiveMode.prototype), {
			runtimeHost: {
				dispose: async () => {},
				session: {
					settingsManager: {
						getFullscreenCopyOnSelect: () => true,
						getFullscreenWheelScrollLines: () => "auto",
						getShowTerminalProgress: () => false,
					},
					isStreaming: false,
					isCompacting: false,
				},
			},
			renderer: undefined as any,
			ui: undefined as any,
			fullscreenLayoutRoot: viewport.root,
			options: { tuiMode: options.initialMode },
			themeController: {
				rebindTui: () => {},
				disableAutoSync: () => {},
			},
			extensionTerminalInputSubscriptions: new Set(),
			mainScreenRenderState: undefined,
		});

		function makeRenderer(mode: "regular" | "fullscreen") {
			return createInteractiveTui({
				tuiMode: mode,
				showHardwareCursor: false,
				logDirectory: "/tmp",
				terminal: term,
				fullscreenCopyOnSelect: true,
				fullscreenWheelScrollLines: "auto",
			});
		}

		context.ui = createInteractiveTuiReference(() => context.renderer);
		context.renderer = makeRenderer(options.initialMode);
		for (const r of regions) context.renderer.addChild(r);
		if (isViewportTUI(context.renderer)) context.renderer.setLayoutRoot(viewport.root);

		context.renderer.start();
		await term.waitForRender();

		const boundaries: PhaseBoundaryRecord[] = [];
		let renderNowCount = 0;

		const startCountInitial = term.startCount;
		const stopCountInitial = term.stopCount;

		// 1. Settled before exit
		boundaries.push({
			name: "settledbeforeexit",
			writeIndex: term.writes.length,
			startCount: term.startCount,
			stopCount: term.stopCount,
			renderNowCount,
		});

		let inSwitch = false;

		function instrumentLiveRenderer(ren: any) {
			const origRenderNow = ren.renderNow.bind(ren);
			ren.renderNow = () => {
				renderNowCount++;
				origRenderNow();
				boundaries.push({
					name: "finalrender",
					writeIndex: term.writes.length,
					startCount: term.startCount,
					stopCount: term.stopCount,
					renderNowCount,
				});
			};
			const origStop = ren.stop.bind(ren);
			ren.stop = (opts: any) => {
				origStop(opts);
				const phaseName = inSwitch ? "switchstop" : "finalstop";
				boundaries.push({
					name: phaseName,
					writeIndex: term.writes.length,
					startCount: term.startCount,
					stopCount: term.stopCount,
					renderNowCount,
				});
			};
		}

		instrumentLiveRenderer(context.renderer);

		const origSwitch = context.switchTuiMode.bind(context);
		context.switchTuiMode = function(...args: any[]) {
			inSwitch = true;
			const res = origSwitch(...args);
			inSwitch = false;
			instrumentLiveRenderer(context.renderer);
			return res;
		};

		// If pending mutation specified, apply it
		if (options.pendingMutation && options.pendingMutation.length > 0) {
			for (const mut of options.pendingMutation) {
				docContainer.addChild(new Text(mut, 0, 0));
			}
			boundaries.push({
				name: "pendingmutation",
				writeIndex: term.writes.length,
				startCount: term.startCount,
				stopCount: term.stopCount,
				renderNowCount,
			});
		}

		// If forced renderNow before stop
		if (options.forcedRenderNowBeforeStop) {
			context.renderer.renderNow();
		}

		// Perform frozen exit
		(InteractiveMode.prototype as any).stopInteractiveTui.call(context, options.fullscreenExitOutput);
		await term.waitForRender();

		// Ensure finalstop boundary is recorded if ui.stop didn't hit our wrapper
		if (!boundaries.some(b => b.name === "finalstop")) {
			boundaries.push({
				name: "finalstop",
				writeIndex: term.writes.length,
				startCount: term.startCount,
				stopCount: term.stopCount,
				renderNowCount,
			});
		}

		// Build snapshot for each phase
		const phases: any[] = [];
		for (let i = 0; i < boundaries.length; i++) {
			const prev = i > 0 ? boundaries[i - 1] : undefined;
			phases.push(await replaySnapshot(options.columns, options.rows, term.writes, boundaries[i], prev));
		}

		const finalPhase = phases[phases.length - 1];
		const settledPhase = phases[0];

		const allRawWrites = term.writes;
		const totalRawBytes = allRawWrites.reduce((sum, w) => sum + Buffer.byteLength(w, "utf8"), 0);
		const allControlTokens = extractControlTokens(allRawWrites.join(""));

		const finalStopBoundary = boundaries.find(b => b.name === "finalstop");
		const priorToStopBoundary = boundaries[boundaries.indexOf(finalStopBoundary!) - 1];
		const finalCursorCleanupWrites = priorToStopBoundary
			? allRawWrites.slice(priorToStopBoundary.writeIndex, finalStopBoundary!.writeIndex)
			: [];

		const nonEmptyScrollback = finalPhase.scrollbackLinesTrimmed.filter((l: string) => l.length > 0);

		return {
			name: options.name,
			dimensions: {
				columns: options.columns,
				rows: options.rows,
			},
			inputs: {
				initialMode: options.initialMode,
				fullscreenExitOutput: options.fullscreenExitOutput,
				initialMainBuffer: options.initialMainBuffer,
				documentLines: options.documentLines,
				editorText: options.editorText,
				pendingMutation: options.pendingMutation ?? [],
				forcedRenderNowBeforeStop: options.forcedRenderNowBeforeStop ?? false,
			},
			expected: {
				modeBefore: options.initialMode,
				modeAfter: context.renderer.mode,
				bufferActiveTypeBefore: settledPhase.activeBuffer,
				bufferActiveTypeAfter: finalPhase.activeBuffer,
				startCountDelta: term.startCount - startCountInitial,
				stopCountDelta: term.stopCount - stopCountInitial,
				renderNowCountDelta: renderNowCount,
				totalRawWritesCount: allRawWrites.length,
				totalRawBytes,
				relevantControlTokens: allControlTokens,
				finalCursor: finalPhase.cursor,
				finalCursorVisible: !finalPhase.cursorHidden,
				finalModes: finalPhase.modes,
				finalScreenLinesWithSpaces: finalPhase.screenLinesWithSpaces,
				finalScreenLinesTrimmed: finalPhase.screenLinesTrimmed,
				finalScrollbackLinesWithSpaces: finalPhase.scrollbackLinesWithSpaces,
				finalScrollbackLinesTrimmed: finalPhase.scrollbackLinesTrimmed,
				finalNonEmptyScrollback: nonEmptyScrollback,
				finalCursorCleanupWrites,
				phases,
			},
		};
	}

	const scenarios: any[] = [];

	// 1. fullscreen-transcript-empty
	scenarios.push(await runCase({
		name: "fullscreen-transcript-empty",
		initialMode: "fullscreen",
		fullscreenExitOutput: "transcript",
		columns: 40,
		rows: 10,
		initialMainBuffer: ["[Main Banner Line 1]", "[Main Banner Line 2]"],
		documentLines: [],
		editorText: "blank draft",
	}));

	// 2. fullscreen-transcript-short
	scenarios.push(await runCase({
		name: "fullscreen-transcript-short",
		initialMode: "fullscreen",
		fullscreenExitOutput: "transcript",
		columns: 40,
		rows: 10,
		initialMainBuffer: ["[Main Banner Line 1]", "[Main Banner Line 2]"],
		documentLines: ["User: prompt text", "Assistant: response text"],
		editorText: "editor text",
	}));

	// 3. fullscreen-transcript-long
	scenarios.push(await runCase({
		name: "fullscreen-transcript-long",
		initialMode: "fullscreen",
		fullscreenExitOutput: "transcript",
		columns: 40,
		rows: 8,
		initialMainBuffer: ["[Main Banner Line 1]"],
		documentLines: [
			"Turn message #1", "Turn message #2", "Turn message #3", "Turn message #4",
			"Turn message #5", "Turn message #6", "Turn message #7", "Turn message #8",
			"Turn message #9", "Turn message #10", "Turn message #11", "Turn message #12",
		],
		editorText: "bottom editor",
	}));

	// 4. fullscreen-resume-hint
	scenarios.push(await runCase({
		name: "fullscreen-resume-hint",
		initialMode: "fullscreen",
		fullscreenExitOutput: "resume-hint",
		columns: 40,
		rows: 10,
		initialMainBuffer: ["[Main Banner Line 1]", "[Main Banner Line 2]"],
		documentLines: ["User: prompt text", "Assistant: response text"],
		editorText: "editor text",
	}));

	// 5. regular-clean-transcript
	scenarios.push(await runCase({
		name: "regular-clean-transcript",
		initialMode: "regular",
		fullscreenExitOutput: "transcript",
		columns: 40,
		rows: 10,
		initialMainBuffer: ["[Main Banner Shell 1]", "[Main Banner Shell 2]"],
		documentLines: ["User: prompt text", "Assistant: response text"],
		editorText: "editor text",
	}));

	// 6. regular-clean-resume-hint
	scenarios.push(await runCase({
		name: "regular-clean-resume-hint",
		initialMode: "regular",
		fullscreenExitOutput: "resume-hint",
		columns: 40,
		rows: 10,
		initialMainBuffer: ["[Main Banner Shell 1]", "[Main Banner Shell 2]"],
		documentLines: ["User: prompt text", "Assistant: response text"],
		editorText: "editor text",
	}));

	// 7. regular-pending-mutation-stop (frozen behavior: stopInteractiveTui does NOT render pending mutation)
	scenarios.push(await runCase({
		name: "regular-pending-mutation-stop",
		initialMode: "regular",
		fullscreenExitOutput: "transcript",
		columns: 40,
		rows: 10,
		initialMainBuffer: ["[Main Banner Shell 1]", "[Main Banner Shell 2]"],
		documentLines: ["User: prompt text", "Assistant: response text"],
		editorText: "editor text",
		pendingMutation: ["Mutation: unpainted pending response"],
		forcedRenderNowBeforeStop: false,
	}));

	// 8. regular-pending-mutation-render-now-stop (paired contrast: forced renderNow BEFORE stop emits pending mutation)
	scenarios.push(await runCase({
		name: "regular-pending-mutation-render-now-stop",
		initialMode: "regular",
		fullscreenExitOutput: "transcript",
		columns: 40,
		rows: 10,
		initialMainBuffer: ["[Main Banner Shell 1]", "[Main Banner Shell 2]"],
		documentLines: ["User: prompt text", "Assistant: response text"],
		editorText: "editor text",
		pendingMutation: ["Mutation: unpainted pending response"],
		forcedRenderNowBeforeStop: true,
	}));

	const sourceEndpoints = [
		"packages/coding-agent/src/modes/interactive/interactive-mode.ts:stopInteractiveTui,switchTuiMode",
		"packages/tui/src/tui-main-screen.ts:beforeTerminalStop",
		"packages/tui/src/tui-alt-screen.ts:afterTerminalStop",
		"packages/tui/src/tui.ts:Tui.stop,Tui.renderNow",
	];

	const observationRun = {
		timestamp: new Date().toISOString(),
		node: process.version,
		platform: process.platform,
		arch: process.arch,
		caller: "recapture-fullscreen-exit-protocol.mts",
		scenarioCount: scenarios.length,
	};

	return { sourceEndpoints, scenarios, observationRun };
}
