import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { registerHooks } from "node:module";

export async function captureFullscreenExitOutput(piCheckout: string) {
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
	const { InteractiveMode, formatResumeCommand } = await import(source("coding-agent/src/modes/interactive/interactive-mode.ts"));
	const { createInteractiveTui, createInteractiveTuiReference } = await import(source("coding-agent/src/modes/interactive/tui-renderer.ts"));
	const { createChatViewport } = await import(source("coding-agent/src/modes/interactive/chat-viewport.ts"));
	const { Container, Text } = await import(source("tui/src/index.ts"));
	const { VirtualTerminal } = await import(source("tui/test/virtual-terminal.ts"));
	const { isViewportTUI } = await import(source("tui/src/tui.ts"));
	const chalk = (await import(pathToFileURL(path.join(piCheckout, "node_modules/chalk/source/index.js")).href)).default;

	class ExitTerminal extends VirtualTerminal {
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
		getNonEmptyScrollback(): string[] {
			return this.getScrollBuffer().map((l: string) => l.trim()).filter((l: string) => l.length > 0);
		}
	}

	function createHarness(options: {
		initialMode: "regular" | "fullscreen";
		columns: number;
		rows: number;
		initialMainBuffer?: string[];
		documentLines: string[];
		editorText?: string;
		statusText?: string;
		footerText?: string;
		showTerminalProgress?: boolean;
	}) {
		const term = new ExitTerminal(options.columns, options.rows);
		term.start(() => {}, () => {});

		if (options.initialMainBuffer) {
			for (const line of options.initialMainBuffer) {
				term.write(`${line}\r\n`);
			}
		}

		const docContainer = new Container();
		for (const line of options.documentLines) {
			docContainer.addChild(new Text(line, 0, 0));
		}
		const pendingContainer = new Container();
		const statusContainer = new Container();
		if (options.statusText) {
			statusContainer.addChild(new Text(options.statusText, 0, 0));
		}
		const editorContainer = new Container();
		editorContainer.addChild(new Text(options.editorText ?? "prompt editor", 0, 0));
		const footerContainer = new Container();
		if (options.footerText) {
			footerContainer.addChild(new Text(options.footerText, 0, 0));
		}

		const viewport = createChatViewport({
			document: docContainer,
			pendingMessages: pendingContainer,
			status: statusContainer,
			editor: editorContainer,
			footer: footerContainer,
			scrollbar: "hidden",
		});

		const regions = [docContainer, pendingContainer, statusContainer, editorContainer, footerContainer];
		const order: string[] = [];

		const context = Object.assign(Object.create(InteractiveMode.prototype), {
			runtimeHost: {
				dispose: async () => { order.push("runtimeHost.dispose"); },
				session: {
					settingsManager: {
						getFullscreenCopyOnSelect: () => true,
						getFullscreenWheelScrollLines: () => "auto",
						getShowTerminalProgress: () => options.showTerminalProgress ?? false,
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
				rebindTui: () => { order.push("theme.rebindTui"); },
				disableAutoSync: () => { order.push("theme.disableAutoSync"); },
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

		return {
			term,
			context,
			viewport,
			docContainer,
			order,
			async stopInteractiveTui(output: "transcript" | "resume-hint") {
				(InteractiveMode.prototype as any).stopInteractiveTui.call(context, output);
				await term.waitForRender();
			},
		};
	}

	const scenarios: any[] = [];

	// 1. Fullscreen transcript exit
	{
		const harness = createHarness({
			initialMode: "fullscreen",
			columns: 40,
			rows: 10,
			initialMainBuffer: ["Header line 1", "Header line 2"],
			documentLines: ["User: prompt text", "Assistant: response text"],
			editorText: "editor text",
		});
		await harness.term.waitForRender();
		assert.equal(harness.term.xterm.buffer.active.type, "alternate");
		const startCountBefore = harness.term.startCount;
		const stopCountBefore = harness.term.stopCount;
		await harness.stopInteractiveTui("transcript");

		scenarios.push({
			name: "fullscreen-transcript-exit",
			dimensions: { columns: 40, rows: 10 },
			inputs: {
				initialMode: "fullscreen",
				fullscreenExitOutput: "transcript",
				initialMainBuffer: ["Header line 1", "Header line 2"],
				documentLines: ["User: prompt text", "Assistant: response text"],
				editorText: "editor text",
			},
			expected: {
				bufferActiveTypeBefore: "alternate",
				bufferActiveTypeAfter: harness.term.xterm.buffer.active.type,
				modeBefore: "fullscreen",
				modeAfter: harness.context.renderer.mode,
				startCountDelta: harness.term.startCount - startCountBefore,
				stopCountDelta: harness.term.stopCount - stopCountBefore,
				mainBufferLines: harness.term.getNonEmptyScrollback(),
			},
		});
	}

	// 2. Fullscreen resume-hint exit
	{
		const harness = createHarness({
			initialMode: "fullscreen",
			columns: 40,
			rows: 10,
			initialMainBuffer: ["Header line 1", "Header line 2"],
			documentLines: ["User: prompt text", "Assistant: response text"],
			editorText: "editor text",
		});
		await harness.term.waitForRender();
		assert.equal(harness.term.xterm.buffer.active.type, "alternate");
		const startCountBefore = harness.term.startCount;
		const stopCountBefore = harness.term.stopCount;
		await harness.stopInteractiveTui("resume-hint");

		scenarios.push({
			name: "fullscreen-resume-hint-exit",
			dimensions: { columns: 40, rows: 10 },
			inputs: {
				initialMode: "fullscreen",
				fullscreenExitOutput: "resume-hint",
				initialMainBuffer: ["Header line 1", "Header line 2"],
				documentLines: ["User: prompt text", "Assistant: response text"],
				editorText: "editor text",
			},
			expected: {
				bufferActiveTypeBefore: "alternate",
				bufferActiveTypeAfter: harness.term.xterm.buffer.active.type,
				modeBefore: "fullscreen",
				modeAfter: harness.context.renderer.mode,
				startCountDelta: harness.term.startCount - startCountBefore,
				stopCountDelta: harness.term.stopCount - stopCountBefore,
				mainBufferLines: harness.term.getNonEmptyScrollback(),
			},
		});
	}

	// 3. Regular mode exit
	{
		const harness = createHarness({
			initialMode: "regular",
			columns: 40,
			rows: 10,
			initialMainBuffer: ["Shell line 1"],
			documentLines: ["User: reg prompt", "Assistant: reg reply"],
			editorText: "reg editor",
		});
		await harness.term.waitForRender();
		assert.equal(harness.term.xterm.buffer.active.type, "normal");
		const startCountBefore = harness.term.startCount;
		const stopCountBefore = harness.term.stopCount;
		await harness.stopInteractiveTui("transcript");

		scenarios.push({
			name: "regular-mode-exit",
			dimensions: { columns: 40, rows: 10 },
			inputs: {
				initialMode: "regular",
				fullscreenExitOutput: "transcript",
				initialMainBuffer: ["Shell line 1"],
				documentLines: ["User: reg prompt", "Assistant: reg reply"],
				editorText: "reg editor",
			},
			expected: {
				bufferActiveTypeBefore: "normal",
				bufferActiveTypeAfter: harness.term.xterm.buffer.active.type,
				modeBefore: "regular",
				modeAfter: harness.context.renderer.mode,
				startCountDelta: harness.term.startCount - startCountBefore,
				stopCountDelta: harness.term.stopCount - stopCountBefore,
				mainBufferLines: harness.term.getNonEmptyScrollback(),
			},
		});
	}

	// 4. Empty transcript exit
	{
		const harness = createHarness({
			initialMode: "fullscreen",
			columns: 40,
			rows: 10,
			initialMainBuffer: ["Top banner"],
			documentLines: [],
			editorText: "blank draft",
		});
		await harness.term.waitForRender();
		await harness.stopInteractiveTui("transcript");

		scenarios.push({
			name: "empty-transcript-exit",
			dimensions: { columns: 40, rows: 10 },
			inputs: {
				initialMode: "fullscreen",
				fullscreenExitOutput: "transcript",
				initialMainBuffer: ["Top banner"],
				documentLines: [],
				editorText: "blank draft",
			},
			expected: {
				modeAfter: harness.context.renderer.mode,
				bufferActiveTypeAfter: harness.term.xterm.buffer.active.type,
				mainBufferLines: harness.term.getNonEmptyScrollback(),
			},
		});
	}

	// 5. Long transcript exit
	{
		const longLines = Array.from({ length: 12 }, (_, i) => `Turn message #${i + 1}`);
		const harness = createHarness({
			initialMode: "fullscreen",
			columns: 40,
			rows: 8,
			initialMainBuffer: ["History start"],
			documentLines: longLines,
			editorText: "bottom editor",
		});
		await harness.term.waitForRender();
		await harness.stopInteractiveTui("transcript");

		scenarios.push({
			name: "long-transcript-exit",
			dimensions: { columns: 40, rows: 8 },
			inputs: {
				initialMode: "fullscreen",
				fullscreenExitOutput: "transcript",
				initialMainBuffer: ["History start"],
				documentLines: longLines,
				editorText: "bottom editor",
			},
			expected: {
				modeAfter: harness.context.renderer.mode,
				bufferActiveTypeAfter: harness.term.xterm.buffer.active.type,
				mainBufferLines: harness.term.getNonEmptyScrollback(),
			},
		});
	}

	// 6. Overlay dismissal on transcript exit
	{
		const harness = createHarness({
			initialMode: "fullscreen",
			columns: 40,
			rows: 10,
			initialMainBuffer: ["Main buffer row"],
			documentLines: ["User message"],
			editorText: "active prompt",
		});
		await harness.term.waitForRender();
		const overlay = new Text("active overlay modal", 0, 0);
		harness.context.renderer.showOverlay(overlay);
		assert.equal(harness.context.renderer.hasOverlayEntries, true);
		await harness.stopInteractiveTui("transcript");

		scenarios.push({
			name: "overlay-dismissal-exit",
			dimensions: { columns: 40, rows: 10 },
			inputs: {
				initialMode: "fullscreen",
				fullscreenExitOutput: "transcript",
				overlayText: "active overlay modal",
			},
			expected: {
				hasOverlayEntriesAfter: harness.context.renderer.hasOverlayEntries,
				mainBufferLines: harness.term.getNonEmptyScrollback(),
			},
		});
	}

	// 7. Format resume command matrix
	{
		const tmpDir = fs.mkdtempSync(path.join(os.tmpdir(), "pi-exit-scenarios-"));
		const sessionFile = path.join(tmpDir, "session.jsonl");
		fs.writeFileSync(sessionFile, "{}\n");

		Object.defineProperty(process.stdout, "isTTY", { configurable: true, value: true });

		const smDefault = {
			isPersisted: () => true,
			getSessionFile: () => sessionFile,
			usesDefaultSessionDir: () => true,
			getSessionDir: () => "/default/sessions",
			getSessionId: () => "0197f6e4-4cf9-7f44-a2d8-f8f7f49ee9d3",
		};
		const cmdDefault = formatResumeCommand(smDefault as any);

		const smCustomNoSpace = {
			isPersisted: () => true,
			getSessionFile: () => sessionFile,
			usesDefaultSessionDir: () => false,
			getSessionDir: () => "/custom/sessions",
			getSessionId: () => "session-abc-123",
		};
		const cmdCustomNoSpace = formatResumeCommand(smCustomNoSpace as any);

		const smCustomSpaces = {
			isPersisted: () => true,
			getSessionFile: () => sessionFile,
			usesDefaultSessionDir: () => false,
			getSessionDir: () => "/my custom sessions/dir",
			getSessionId: () => "session-spaces-456",
		};
		const cmdCustomSpaces = formatResumeCommand(smCustomSpaces as any);

		const smCustomSingleQuote = {
			isPersisted: () => true,
			getSessionFile: () => sessionFile,
			usesDefaultSessionDir: () => false,
			getSessionDir: () => "/my user's sessions/dir",
			getSessionId: () => "session-quotes-789",
		};
		const cmdCustomSingleQuote = formatResumeCommand(smCustomSingleQuote as any);

		const smNotPersisted = {
			isPersisted: () => false,
			getSessionFile: () => sessionFile,
			usesDefaultSessionDir: () => true,
			getSessionDir: () => "/default/sessions",
			getSessionId: () => "test-session",
		};
		const cmdNotPersisted = formatResumeCommand(smNotPersisted as any);

		const smMissingFile = {
			isPersisted: () => true,
			getSessionFile: () => path.join(tmpDir, "nonexistent.jsonl"),
			usesDefaultSessionDir: () => true,
			getSessionDir: () => "/default/sessions",
			getSessionId: () => "test-session",
		};
		const cmdMissingFile = formatResumeCommand(smMissingFile as any);

		Object.defineProperty(process.stdout, "isTTY", { configurable: true, value: false });
		const cmdNotTTY = formatResumeCommand(smDefault as any);
		Object.defineProperty(process.stdout, "isTTY", { configurable: true, value: true });

		chalk.level = 1;
		const styledNotice = `${chalk.dim("To resume this session:")} ${cmdDefault}\n`;

		fs.rmSync(tmpDir, { recursive: true, force: true });

		scenarios.push({
			name: "format-resume-command-matrix",
			dimensions: { columns: 80, rows: 24 },
			inputs: {
				defaultSession: { sessionId: "0197f6e4-4cf9-7f44-a2d8-f8f7f49ee9d3" },
				customNoSpace: { sessionDir: "/custom/sessions", sessionId: "session-abc-123" },
				customSpaces: { sessionDir: "/my custom sessions/dir", sessionId: "session-spaces-456" },
				customSingleQuote: { sessionDir: "/my user's sessions/dir", sessionId: "session-quotes-789" },
			},
			expected: {
				cmdDefault,
				cmdCustomNoSpace,
				cmdCustomSpaces,
				cmdCustomSingleQuote,
				cmdNotPersisted: cmdNotPersisted ?? null,
				cmdMissingFile: cmdMissingFile ?? null,
				cmdNotTTY: cmdNotTTY ?? null,
				styledNotice,
				chalkDimHeader: chalk.dim("To resume this session:"),
			},
		});
	}

	return {
		sourceEndpoints: [
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:338",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:874",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:4284",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:7053",
			"packages/coding-agent/src/core/settings-manager.ts:55",
			"packages/coding-agent/src/core/settings-manager.ts:1358",
			"packages/tui/src/tui.ts:970",
			"packages/tui/src/tui-main-screen.ts:169",
			"packages/tui/src/tui-alt-screen.ts:399",
		],
		scenarios,
	};
}
