// Scenarios for artifact 35: fullscreen-exit-live.json
// Measures live frozen in-memory VirtualTerminal state across all exit phases,
// recording public Terminal method calls, low-level xterm.write byte dispatches,
// and buffer/cursor/mode states without synthetic replay omissions.

import assert from "node:assert/strict";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { registerHooks } from "node:module";

export async function captureFullscreenExitLive(piCheckout: string) {
	const directories: Record<string, string> = {
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

	registerHooks({
		resolve(specifier, context, next) {
			if (specifier.startsWith("@earendil-works/")) {
				const [name, ...parts] = specifier.slice("@earendil-works/".length).split("/");
				if (directories[name]) {
					const relative = parts.length ? parts.join("/") : "index.ts";
					return {
						url: pathToFileURL(path.join(piCheckout, "packages", directories[name], "src",
							relative.endsWith(".ts") ? relative : `${relative}.ts`)).href,
						shortCircuit: true,
					};
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

	function extractControlTokens(rawText: string): string[] {
		const matches = rawText.match(/(\x1b\[\??[0-9;]*[a-zA-Z]|\x1b\][^\x07]*\x07|\r\n|\r|\n)/g);
		return matches ? Array.from(matches) : [];
	}

	interface MethodEvent {
		seq: number;
		phase: string;
		seam: "Terminal" | "xterm" | "Renderer";
		method: string;
		payload?: string;
		args?: any[];
		detail?: any;
	}

	class FullyInstrumentedTerminal extends (VirtualTerminal as any) {
		writes: string[] = [];
		xtermWrites: string[] = [];
		events: MethodEvent[] = [];
		eventCounter = 0;
		currentPhase = "init";
		startCount = 0;
		stopCount = 0;
		renderNowCount = 0;

		constructor(columns = 80, rows = 24) {
			super(columns, rows);

			const origXtermWrite = this.xterm.write.bind(this.xterm);
			(this.xterm as any).write = (data: any, cb?: () => void) => {
				if (typeof data === "string" && data.length > 0) {
					this.xtermWrites.push(data);
					this.events.push({
						seq: ++this.eventCounter,
						phase: this.currentPhase,
						seam: "xterm",
						method: "xterm.write",
						payload: data,
						detail: { bytes: Buffer.byteLength(data, "utf8") },
					});
				}
				return origXtermWrite(data, cb);
			};
		}

		override write(data: string): void {
			this.writes.push(data);
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "write",
				payload: data,
				args: [data],
				detail: { bytes: Buffer.byteLength(data, "utf8"), writeIndex: this.writes.length - 1 },
			});
			super.write(data);
		}

		override start(input: (data: string) => void, resize: () => void): void {
			this.startCount++;
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "start",
				args: ["[inputCallback]", "[resizeCallback]"],
				detail: { startCount: this.startCount },
			});
			super.start(input, resize);
		}

		override stop(): void {
			this.stopCount++;
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "stop",
				args: [],
				detail: { stopCount: this.stopCount },
			});
			super.stop();
		}

		override hideCursor(): void {
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "hideCursor",
				args: [],
			});
			super.hideCursor();
		}

		override showCursor(): void {
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "showCursor",
				args: [],
			});
			super.showCursor();
		}

		override moveBy(lines: number): void {
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "moveBy",
				args: [lines],
				detail: { lines },
			});
			super.moveBy(lines);
		}

		override clearLine(): void {
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "clearLine",
				args: [],
			});
			super.clearLine();
		}

		override clearFromCursor(): void {
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "clearFromCursor",
				args: [],
			});
			super.clearFromCursor();
		}

		override clearScreen(): void {
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "clearScreen",
				args: [],
			});
			super.clearScreen();
		}

		override setTitle(title: string): void {
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "setTitle",
				args: [title],
				detail: { title },
			});
			super.setTitle(title);
		}

		override setProgress(active: boolean): void {
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "setProgress",
				args: [active],
				detail: { active },
			});
			super.setProgress(active);
		}

		override async drainInput(maxMs?: number, idleMs?: number): Promise<void> {
			this.events.push({
				seq: ++this.eventCounter,
				phase: this.currentPhase,
				seam: "Terminal",
				method: "drainInput",
				args: [maxMs, idleMs],
				detail: { maxMs, idleMs },
			});
			return super.drainInput(maxMs, idleMs);
		}
	}

	function captureBufferState(
		term: FullyInstrumentedTerminal,
		phase: string,
		rows: number,
		settledStartCount: number,
		settledStopCount: number,
		settledRenderNowCount: number,
		prevTerminalWritesIndex: number,
		prevXtermWritesIndex: number,
		prevEventsIndex: number,
	) {
		const buf = term.xterm.buffer.active;
		const screenWithSpaces: string[] = [];
		const screenTrimmed: string[] = [];
		for (let r = 0; r < rows; r++) {
			const line = buf.getLine(buf.viewportY + r);
			screenWithSpaces.push(line ? line.translateToString(false) : "");
			screenTrimmed.push(line ? line.translateToString(true) : "");
		}

		const fullBufferTrimmed: string[] = [];
		const fullBufferWithSpaces: string[] = [];
		for (let r = 0; r < buf.length; r++) {
			const line = buf.getLine(r);
			fullBufferTrimmed.push(line ? line.translateToString(true) : "");
			fullBufferWithSpaces.push(line ? line.translateToString(false) : "");
		}

		const phaseTerminalWrites = term.writes.slice(prevTerminalWritesIndex);
		const phaseTerminalBytes = phaseTerminalWrites.reduce((sum, w) => sum + Buffer.byteLength(w, "utf8"), 0);

		const phaseXtermWrites = term.xtermWrites.slice(prevXtermWritesIndex);
		const phaseXtermBytes = phaseXtermWrites.reduce((sum, w) => sum + Buffer.byteLength(w, "utf8"), 0);

		const cumulativeTerminalBytes = term.writes.reduce((sum, w) => sum + Buffer.byteLength(w, "utf8"), 0);
		const cumulativeXtermBytes = term.xtermWrites.reduce((sum, w) => sum + Buffer.byteLength(w, "utf8"), 0);

		const phaseControlTokens = extractControlTokens(phaseXtermWrites.join(""));

		const phaseEvents = term.events.slice(prevEventsIndex);
		const terminalMethodEvents = phaseEvents
			.filter(e => e.seam === "Terminal")
			.map(e => ({ method: e.method, args: e.args ?? [], detail: e.detail }));

		return {
			phase,
			source: "live",
			activeBuffer: buf.type as ("normal" | "alternate"),
			cursor: {
				x: buf.cursorX,
				y: buf.cursorY,
				baseY: buf.baseY,
				viewportY: buf.viewportY,
			},
			cursorHidden: !!term.xterm._core?.coreService?.isCursorHidden,
			modes: {
				bracketedPasteMode: !!term.xterm.modes?.bracketedPasteMode,
				alternateScreen: buf.type === "alternate",
				wraparoundMode: !!term.xterm.modes?.wraparoundMode,
			},
			screenLinesWithSpaces: screenWithSpaces,
			screenLinesTrimmed: screenTrimmed,
			fullBufferLinesWithSpaces: fullBufferWithSpaces,
			fullBufferLinesTrimmed: fullBufferTrimmed,
			startCount: term.startCount,
			stopCount: term.stopCount,
			renderNowCount: term.renderNowCount,
			startCountDelta: term.startCount - settledStartCount,
			stopCountDelta: term.stopCount - settledStopCount,
			renderNowCountDelta: term.renderNowCount - settledRenderNowCount,
			phaseTerminalWritesCount: phaseTerminalWrites.length,
			phaseTerminalBytes,
			phaseTerminalWrites,
			phaseXtermWritesCount: phaseXtermWrites.length,
			phaseXtermBytes,
			phaseXtermWrites,
			cumulativeTerminalWritesCount: term.writes.length,
			cumulativeTerminalBytes,
			cumulativeXtermWritesCount: term.xtermWrites.length,
			cumulativeXtermBytes,
			phaseControlTokens,
			terminalMethodEvents,
			allMethodEvents: phaseEvents,
		};
	}

	interface ScenarioOptions {
		name: string;
		columns: number;
		rows: number;
		initialMode: "fullscreen" | "regular";
		fullscreenExitOutput: "transcript" | "resume-hint";
		initialMainBuffer: string[];
		documentLines: string[];
		editorText: string;
		pendingMutation?: string[];
		forcedRenderNowBeforeStop?: boolean;
	}

	async function runScenario(options: ScenarioOptions) {
		const term = new FullyInstrumentedTerminal(options.columns, options.rows);

		term.currentPhase = "setup";
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

		function instrumentRenderer(ren: any, label: string) {
			const origStart = ren.start.bind(ren);
			ren.start = (...args: any[]) => {
				term.events.push({
					seq: ++term.eventCounter,
					phase: term.currentPhase,
					seam: "Renderer",
					method: `${label}.start`,
				});
				return origStart(...args);
			};

			const origStop = ren.stop.bind(ren);
			ren.stop = (...args: any[]) => {
				term.events.push({
					seq: ++term.eventCounter,
					phase: term.currentPhase,
					seam: "Renderer",
					method: `${label}.stop`,
					args,
					detail: args[0],
				});
				return origStop(...args);
			};

			const origRenderNow = ren.renderNow.bind(ren);
			ren.renderNow = (...args: any[]) => {
				term.renderNowCount++;
				term.events.push({
					seq: ++term.eventCounter,
					phase: term.currentPhase,
					seam: "Renderer",
					method: `${label}.renderNow`,
					args,
					detail: { force: args[0] },
				});
				return origRenderNow(...args);
			};
		}

		context.ui = createInteractiveTuiReference(() => context.renderer);
		context.renderer = makeRenderer(options.initialMode);
		instrumentRenderer(context.renderer, options.initialMode === "fullscreen" ? "FullscreenRenderer" : "RegularRenderer");
		for (const r of regions) context.renderer.addChild(r);
		if (isViewportTUI(context.renderer)) context.renderer.setLayoutRoot(viewport.root);

		context.renderer.start();
		await term.waitForRender();
		await term.flush();

		const settledStartCount = term.startCount;
		const settledStopCount = term.stopCount;
		const settledRenderNowCount = term.renderNowCount;

		const phases: any[] = [];

		// -------------------------------------------------------------
		// Phase 0: settledbeforeexit
		// -------------------------------------------------------------
		term.currentPhase = "settledbeforeexit";
		const p0 = captureBufferState(
			term,
			"settledbeforeexit",
			options.rows,
			settledStartCount,
			settledStopCount,
			settledRenderNowCount,
			0,
			0,
			0,
		);
		phases.push(p0);

		let lastTerminalIdx = term.writes.length;
		let lastXtermIdx = term.xtermWrites.length;
		let lastEventIdx = term.events.length;

		// Execute controlled exit phases
		if (options.initialMode === "fullscreen" && options.fullscreenExitOutput === "transcript") {
			// Phase 1: switchstop
			term.currentPhase = "switchstop";
			while (context.renderer.hasOverlayEntries) {
				context.renderer.hideOverlay();
			}
			(context as any).switchTuiMode("regular", false, false);
			instrumentRenderer(context.renderer, "RegularRenderer");
			await term.flush();

			const p1 = captureBufferState(
				term,
				"switchstop",
				options.rows,
				settledStartCount,
				settledStopCount,
				settledRenderNowCount,
				lastTerminalIdx,
				lastXtermIdx,
				lastEventIdx,
			);
			phases.push(p1);
			lastTerminalIdx = term.writes.length;
			lastXtermIdx = term.xtermWrites.length;
			lastEventIdx = term.events.length;

			// Phase 2: finalrender
			term.currentPhase = "finalrender";
			context.renderer.renderNow();
			await term.flush();

			const p2 = captureBufferState(
				term,
				"finalrender",
				options.rows,
				settledStartCount,
				settledStopCount,
				settledRenderNowCount,
				lastTerminalIdx,
				lastXtermIdx,
				lastEventIdx,
			);
			phases.push(p2);
			lastTerminalIdx = term.writes.length;
			lastXtermIdx = term.xtermWrites.length;
			lastEventIdx = term.events.length;

			// Phase 3: finalstop
			term.currentPhase = "finalstop";
			context.ui.stop({ preserveScreen: context.renderer.mode === "fullscreen" });
			await term.flush();

			const p3 = captureBufferState(
				term,
				"finalstop",
				options.rows,
				settledStartCount,
				settledStopCount,
				settledRenderNowCount,
				lastTerminalIdx,
				lastXtermIdx,
				lastEventIdx,
			);
			phases.push(p3);
		} else if (options.initialMode === "fullscreen" && options.fullscreenExitOutput === "resume-hint") {
			// resume-hint only stop
			term.currentPhase = "finalstop";
			context.ui.stop({ preserveScreen: context.renderer.mode === "fullscreen" });
			await term.flush();

			const pFinal = captureBufferState(
				term,
				"finalstop",
				options.rows,
				settledStartCount,
				settledStopCount,
				settledRenderNowCount,
				lastTerminalIdx,
				lastXtermIdx,
				lastEventIdx,
			);
			phases.push(pFinal);
		} else if (options.initialMode === "regular") {
			if (options.pendingMutation && options.pendingMutation.length > 0) {
				term.currentPhase = "pendingmutation";
				for (const mut of options.pendingMutation) {
					docContainer.addChild(new Text(mut, 0, 0));
				}
				const pMut = captureBufferState(
					term,
					"pendingmutation",
					options.rows,
					settledStartCount,
					settledStopCount,
					settledRenderNowCount,
					lastTerminalIdx,
					lastXtermIdx,
					lastEventIdx,
				);
				phases.push(pMut);
				lastTerminalIdx = term.writes.length;
				lastXtermIdx = term.xtermWrites.length;
				lastEventIdx = term.events.length;
			}

			if (options.forcedRenderNowBeforeStop) {
				term.currentPhase = "finalrender";
				context.renderer.renderNow();
				await term.flush();

				const pRender = captureBufferState(
					term,
					"finalrender",
					options.rows,
					settledStartCount,
					settledStopCount,
					settledRenderNowCount,
					lastTerminalIdx,
					lastXtermIdx,
					lastEventIdx,
				);
				phases.push(pRender);
				lastTerminalIdx = term.writes.length;
				lastXtermIdx = term.xtermWrites.length;
				lastEventIdx = term.events.length;
			}

			term.currentPhase = "finalstop";
			context.ui.stop({ preserveScreen: context.renderer.mode === "fullscreen" });
			await term.flush();

			const pStop = captureBufferState(
				term,
				"finalstop",
				options.rows,
				settledStartCount,
				settledStopCount,
				settledRenderNowCount,
				lastTerminalIdx,
				lastXtermIdx,
				lastEventIdx,
			);
			phases.push(pStop);
		}

		const finalPhase = phases[phases.length - 1];
		const settledPhase = phases[0];

		const allTerminalWrites = term.writes;
		const totalTerminalBytes = allTerminalWrites.reduce((sum, w) => sum + Buffer.byteLength(w, "utf8"), 0);

		const allXtermWrites = term.xtermWrites;
		const totalXtermBytes = allXtermWrites.reduce((sum, w) => sum + Buffer.byteLength(w, "utf8"), 0);

		const allControlTokens = extractControlTokens(allXtermWrites.join(""));

		const finalStopPhase = phases.find(p => p.phase === "finalstop");
		const finalCursorCleanupWrites = finalStopPhase ? finalStopPhase.phaseTerminalWrites : [];
		const finalNonEmptyFullBuffer = finalPhase.fullBufferLinesTrimmed.filter((l: string) => l.length > 0);

		// Integrated check for fullscreen-transcript-short
		let integratedStopCrosscheck: any = undefined;
		if (options.name === "fullscreen-transcript-short") {
			const termInt = new FullyInstrumentedTerminal(options.columns, options.rows);
			termInt.currentPhase = "setup";
			termInt.start(() => {}, () => {});
			for (const line of options.initialMainBuffer) {
				termInt.write(`${line}\r\n`);
			}

			const docInt = new Container();
			for (const line of options.documentLines) {
				docInt.addChild(new Text(line, 0, 0));
			}
			const editInt = new Container();
			editInt.addChild(new Text(options.editorText, 0, 0));
			const vpInt = createChatViewport({
				document: docInt,
				pendingMessages: new Container(),
				status: new Container(),
				editor: editInt,
				footer: new Container(),
				scrollbar: "hidden",
			});
			const ctxInt = Object.assign(Object.create(InteractiveMode.prototype), {
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
				fullscreenLayoutRoot: vpInt.root,
				options: { tuiMode: "fullscreen" },
				themeController: { rebindTui: () => {}, disableAutoSync: () => {} },
				extensionTerminalInputSubscriptions: new Set(),
				mainScreenRenderState: undefined,
			});

			ctxInt.ui = createInteractiveTuiReference(() => ctxInt.renderer);
			ctxInt.renderer = createInteractiveTui({
				tuiMode: "fullscreen",
				showHardwareCursor: false,
				logDirectory: "/tmp",
				terminal: termInt,
				fullscreenCopyOnSelect: true,
				fullscreenWheelScrollLines: "auto",
			});
			instrumentRenderer(ctxInt.renderer, "FullscreenRenderer");
			for (const r of [docInt, editInt]) ctxInt.renderer.addChild(r);
			if (isViewportTUI(ctxInt.renderer)) ctxInt.renderer.setLayoutRoot(vpInt.root);
			ctxInt.renderer.start();
			await termInt.waitForRender();
			await termInt.flush();

			const p0TermWrites = termInt.writes.length;
			const p0XtermWrites = termInt.xtermWrites.length;
			const p0Events = termInt.events.length;

			const origSwitch = (ctxInt as any).switchTuiMode.bind(ctxInt);
			(ctxInt as any).switchTuiMode = (...args: any[]) => {
				const res = origSwitch(...args);
				instrumentRenderer(ctxInt.renderer, "RegularRenderer");
				return res;
			};

			termInt.currentPhase = "integrated_stop";
			(ctxInt as any).stopInteractiveTui("transcript");
			await termInt.flush();

			const intFinalState = captureBufferState(
				termInt,
				"integrated_stop",
				options.rows,
				settledStartCount,
				settledStopCount,
				settledRenderNowCount,
				p0TermWrites,
				p0XtermWrites,
				p0Events,
			);

			const decompExitWrites = term.writes.slice(phases[0].cumulativeTerminalWritesCount);
			const decompExitXtermWrites = term.xtermWrites.slice(phases[0].cumulativeXtermWritesCount);
			const decompExitEvents = term.events.slice(phases[0].allMethodEvents.length);

			const intExitWrites = termInt.writes.slice(p0TermWrites);
			const intExitXtermWrites = termInt.xtermWrites.slice(p0XtermWrites);
			const intExitEvents = termInt.events.slice(p0Events);

			// Assert equivalence
			const stateMatch = JSON.stringify(finalPhase.screenLinesWithSpaces) === JSON.stringify(intFinalState.screenLinesWithSpaces)
				&& JSON.stringify(finalPhase.fullBufferLinesWithSpaces) === JSON.stringify(intFinalState.fullBufferLinesWithSpaces)
				&& finalPhase.cursor.x === intFinalState.cursor.x
				&& finalPhase.cursor.y === intFinalState.cursor.y
				&& finalPhase.cursorHidden === intFinalState.cursorHidden
				&& finalPhase.modes.bracketedPasteMode === intFinalState.modes.bracketedPasteMode
				&& finalPhase.modes.alternateScreen === intFinalState.modes.alternateScreen
				&& finalPhase.modes.wraparoundMode === intFinalState.modes.wraparoundMode;

			const writesMatch = JSON.stringify(decompExitWrites) === JSON.stringify(intExitWrites);
			const xtermWritesMatch = JSON.stringify(decompExitXtermWrites) === JSON.stringify(intExitXtermWrites);

			if (!stateMatch || !writesMatch || !xtermWritesMatch) {
				throw new Error("Integrated vs Decomposed crosscheck divergence detected!");
			}

			integratedStopCrosscheck = {
				comparisonTarget: "fullscreen-transcript-short: integrated stopInteractiveTui vs decomposed phases",
				integratedFinalState: {
					activeBuffer: intFinalState.activeBuffer,
					cursor: intFinalState.cursor,
					cursorHidden: intFinalState.cursorHidden,
					modes: intFinalState.modes,
					screenLinesWithSpaces: intFinalState.screenLinesWithSpaces,
					screenLinesTrimmed: intFinalState.screenLinesTrimmed,
					fullBufferLinesWithSpaces: intFinalState.fullBufferLinesWithSpaces,
					fullBufferLinesTrimmed: intFinalState.fullBufferLinesTrimmed,
					startCountDelta: intFinalState.startCountDelta,
					stopCountDelta: intFinalState.stopCountDelta,
					renderNowCountDelta: intFinalState.renderNowCountDelta,
				},
				decomposedFinalState: {
					activeBuffer: finalPhase.activeBuffer,
					cursor: finalPhase.cursor,
					cursorHidden: finalPhase.cursorHidden,
					modes: finalPhase.modes,
					screenLinesWithSpaces: finalPhase.screenLinesWithSpaces,
					screenLinesTrimmed: finalPhase.screenLinesTrimmed,
					fullBufferLinesWithSpaces: finalPhase.fullBufferLinesWithSpaces,
					fullBufferLinesTrimmed: finalPhase.fullBufferLinesTrimmed,
					startCountDelta: finalPhase.startCountDelta,
					stopCountDelta: finalPhase.stopCountDelta,
					renderNowCountDelta: finalPhase.renderNowCountDelta,
				},
				integratedTerminalWrites: intExitWrites,
				decomposedTerminalWrites: decompExitWrites,
				integratedXtermWrites: intExitXtermWrites,
				decomposedXtermWrites: decompExitXtermWrites,
				integratedNormalizedEvents: intExitEvents.map(e => ({ seam: e.seam, method: e.method, args: e.args, detail: e.detail })),
				decomposedNormalizedEvents: decompExitEvents.map(e => ({ seam: e.seam, method: e.method, args: e.args, detail: e.detail })),
			};
		}

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
				startCountDelta: finalPhase.startCountDelta,
				stopCountDelta: finalPhase.stopCountDelta,
				renderNowCountDelta: finalPhase.renderNowCountDelta,
				totalTerminalWritesCount: allTerminalWrites.length,
				totalTerminalBytes,
				totalXtermWritesCount: allXtermWrites.length,
				totalXtermBytes,
				relevantControlTokens: allControlTokens,
				finalCursor: finalPhase.cursor,
				finalCursorVisible: !finalPhase.cursorHidden,
				finalModes: finalPhase.modes,
				finalScreenLinesWithSpaces: finalPhase.screenLinesWithSpaces,
				finalScreenLinesTrimmed: finalPhase.screenLinesTrimmed,
				finalFullBufferLinesWithSpaces: finalPhase.fullBufferLinesWithSpaces,
				finalFullBufferLinesTrimmed: finalPhase.fullBufferLinesTrimmed,
				finalNonEmptyFullBuffer,
				finalCursorCleanupWrites,
				phases,
				...(integratedStopCrosscheck ? { integratedStopCrosscheck } : {}),
			},
		};
	}

	const scenarios: any[] = [];

	// 1. fullscreen-transcript-empty
	scenarios.push(await runScenario({
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
	scenarios.push(await runScenario({
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
	scenarios.push(await runScenario({
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
	scenarios.push(await runScenario({
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
	scenarios.push(await runScenario({
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
	scenarios.push(await runScenario({
		name: "regular-clean-resume-hint",
		initialMode: "regular",
		fullscreenExitOutput: "resume-hint",
		columns: 40,
		rows: 10,
		initialMainBuffer: ["[Main Banner Shell 1]", "[Main Banner Shell 2]"],
		documentLines: ["User: prompt text", "Assistant: response text"],
		editorText: "editor text",
	}));

	// 7. regular-pending-mutation-stop
	scenarios.push(await runScenario({
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

	// 8. regular-pending-mutation-render-now-stop
	scenarios.push(await runScenario({
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
		"packages/tui/src/tui-main-screen.ts:beforeTerminalStop,renderNow",
		"packages/tui/src/tui-alt-screen.ts:afterTerminalStop,renderNow",
		"packages/tui/src/tui.ts:Tui.stop,Tui.renderNow",
		"packages/tui/test/virtual-terminal.ts:VirtualTerminal.start,VirtualTerminal.stop,VirtualTerminal.write,VirtualTerminal.hideCursor,VirtualTerminal.showCursor",
	];

	const observationRun = {
		timestamp: new Date().toISOString(),
		node: process.version,
		platform: process.platform,
		arch: process.arch,
		caller: "recapture-fullscreen-exit-live.mts",
		scenarioCount: scenarios.length,
		xtermHeadless: "^5.5.0",
		cursorVisibilitySeam: "@xterm/headless xterm._core.coreService.isCursorHidden (private property inspection; public headless API does not expose cursor visibility)",
		measurementType: "controlled in-memory @xterm/headless live terminal and dispatch observation; not OS syscall/PTY stream proof",
	};

	return {
		observationRun,
		sourceEndpoints,
		scenarios,
	};
}
