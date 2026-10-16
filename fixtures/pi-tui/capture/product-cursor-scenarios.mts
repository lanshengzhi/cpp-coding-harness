#!/usr/bin/env tsx
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";

export interface TerminalEvent {
	op: "write" | "showCursor" | "hideCursor";
	bytes?: string;
}

export interface ProductCursorScenario {
	name: string;
	description?: string;
	dimensions: { columns: number; rows: number };
	inputs: Record<string, any>;
	expected: Record<string, any>;
}

export async function captureProductCursor(piCheckout: string) {
	const coding = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/coding-agent", relative)).href;
	const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;
	const requirePi = createRequire(path.join(piCheckout, "packages/tui/package.json"));
	const { Terminal: XtermTerminal } = requirePi("@xterm/headless");

	const { SettingsManager } = await import(coding("src/core/settings-manager.ts"));
	const { createInteractiveTui, createInteractiveTuiReference, InteractiveMode } = await import(
		coding("src/modes/interactive/interactive-mode.ts")
	);
	const { Container, CURSOR_MARKER } = await import(source("src/tui.ts"));
	const { Box } = await import(source("src/components/box.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));
	const { defaultEditorTheme } = await import(source("test/test-themes.ts"));

	class ProductCursorTerminal extends VirtualTerminal {
		readonly emulator: any;
		cursorVisible = false;
		events: TerminalEvent[] = [];
		writes: string[] = [];

		constructor(columns: number, rows: number) {
			super(columns, rows);
			this.emulator = new XtermTerminal({ cols: columns, rows, allowProposedApi: true });
		}

		override write(data: string) {
			this.writes.push(data);
			this.events.push({ op: "write", bytes: data });
			const lastShow = data.lastIndexOf("\x1b[?25h");
			const lastHide = data.lastIndexOf("\x1b[?25l");
			if (lastShow !== -1 || lastHide !== -1) {
				this.cursorVisible = lastShow > lastHide;
			}
			super.write(data);
			this.emulator.write(data);
		}

		override hideCursor() {
			this.cursorVisible = false;
			this.events.push({ op: "hideCursor" });
			super.hideCursor();
		}

		override showCursor() {
			this.cursorVisible = true;
			this.events.push({ op: "showCursor" });
			super.showCursor();
		}

		takeEvents(): TerminalEvent[] {
			const res = [...this.events];
			this.events.length = 0;
			return res;
		}

		takeWrites(): string[] {
			const res = [...this.writes];
			this.writes.length = 0;
			return res;
		}

		async snapshot() {
			await this.flush();
			await new Promise<void>((resolve) => this.emulator.write("", () => resolve()));
			const buffer = this.emulator.buffer.active;
			const screen = this.getViewport();
			return {
				screen,
				cursor: { col: buffer.cursorX, row: buffer.cursorY },
				cursorVisible: this.cursorVisible,
			};
		}
	}

	class TestCursorEditor {
		focused = true;
		constructor(
			public lines: string[],
			public cursorCol?: number,
			public cursorRow: number = 0,
		) {}

		invalidate(): void {}

		render(_width: number): string[] {
			const result = [...this.lines];
			if (this.focused && this.cursorCol !== undefined && this.cursorRow < result.length) {
				const line = result[this.cursorRow] ?? "";
				const before = line.slice(0, this.cursorCol);
				const after = line.slice(this.cursorCol);
				result[this.cursorRow] = before + CURSOR_MARKER + after;
			}
			return result;
		}
	}

	const sourceEndpoints = [
		"packages/coding-agent/src/core/settings-manager.ts:SettingsManager.getShowHardwareCursor,SettingsManager.setShowHardwareCursor",
		"packages/coding-agent/src/modes/interactive/interactive-mode.ts:InteractiveMode.switchTuiMode",
		"packages/coding-agent/src/modes/interactive/tui-renderer.ts:createInteractiveTui",
	];

	const scenarios: ProductCursorScenario[] = [];

	// 1. Settings resolution observations
	{
		const prevEnv = process.env.PI_HARDWARE_CURSOR;

		const runCase = (globalJson: any, projectJson: any, trusted: boolean, envVal?: string) => {
			const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "pi-settings-test-"));
			const globalDir = path.join(tempDir, "global");
			const projectDir = path.join(tempDir, "project");
			fs.mkdirSync(globalDir, { recursive: true });
			fs.mkdirSync(path.join(projectDir, ".pi"), { recursive: true });

			if (globalJson !== undefined) {
				fs.writeFileSync(path.join(globalDir, "settings.json"), JSON.stringify(globalJson));
			}
			if (projectJson !== undefined) {
				fs.writeFileSync(path.join(projectDir, ".pi", "settings.json"), JSON.stringify(projectJson));
			}
			if (envVal !== undefined) process.env.PI_HARDWARE_CURSOR = envVal;
			else delete process.env.PI_HARDWARE_CURSOR;

			const sm = SettingsManager.create(projectDir, globalDir, { projectTrusted: trusted });
			const result = sm.getShowHardwareCursor();
			fs.rmSync(tempDir, { recursive: true, force: true });
			return result;
		};

		const settingsResults = {
			defaultUnset: runCase(undefined, undefined, false, undefined),
			envFallbackActive: runCase(undefined, undefined, false, "1"),
			envFallbackInactiveZero: runCase(undefined, undefined, false, "0"),
			envFallbackInactiveTrue: runCase(undefined, undefined, false, "true"),
			explicitFalseOverridesEnv: runCase({ showHardwareCursor: false }, undefined, false, "1"),
			explicitTrue: runCase({ showHardwareCursor: true }, undefined, false, undefined),
			projectScopeTrustedWins: runCase({ showHardwareCursor: false }, { showHardwareCursor: true }, true, undefined),
			projectScopeUntrustedIgnored: runCase({ showHardwareCursor: false }, { showHardwareCursor: true }, false, undefined),
		};

		if (prevEnv !== undefined) process.env.PI_HARDWARE_CURSOR = prevEnv;
		else delete process.env.PI_HARDWARE_CURSOR;

		scenarios.push({
			name: "product-settings-resolution",
			description: "Resolution matrix for showHardwareCursor across scopes and env fallback",
			dimensions: { columns: 80, rows: 24 },
			inputs: { operation: "getShowHardwareCursor" },
			expected: { settingsResults },
		});
	}

	// Helper for live switch scenarios
	const runSwitchScenario = async (
		name: string,
		description: string,
		initialMode: "regular" | "fullscreen",
		targetMode: "regular" | "fullscreen",
		initialPref: boolean,
		livePref: boolean,
		lines: string[],
		cursorCol: number,
		cursorRow?: number,
		paddingX = 0,
		paddingY = 0,
		cols = 40,
		rows = 10,
	): Promise<void> => {
		const terminal = new ProductCursorTerminal(cols, rows);
		const initialRenderer = createInteractiveTui({
			tuiMode: initialMode,
			showHardwareCursor: initialPref,
			logDirectory: "/tmp",
			terminal,
		});

		const editor = new TestCursorEditor(lines, cursorCol, cursorRow);
		let mountedComponent: any = editor;
		if (paddingX > 0 || paddingY > 0) {
			const box = new Box(paddingX, paddingY);
			box.addChild(editor);
			mountedComponent = box;
		}

		const layoutRoot = new Container();
		layoutRoot.addChild(mountedComponent);

		initialRenderer.addChild(mountedComponent);
		initialRenderer.setFocus(editor);
		initialRenderer.start();
		initialRenderer.renderNow();

		// Live preference modification before switch
		initialRenderer.setShowHardwareCursor(livePref);
		initialRenderer.renderNow();

		terminal.takeEvents();
		terminal.takeWrites();

		const context: any = Object.assign(Object.create(InteractiveMode.prototype), {
			renderer: initialRenderer,
			options: { tuiMode: initialMode },
			runtimeHost: {
				session: {
					settingsManager: {
						getFullscreenCopyOnSelect: () => false,
						getFullscreenWheelScrollLines: () => "auto",
						getShowTerminalProgress: () => false,
					},
					isStreaming: false,
					isCompacting: false,
				},
			},
			fullscreenLayoutRoot: layoutRoot,
			mainScreenRenderState: undefined,
			onRightClickPaste: undefined,
			themeController: { rebindTui: () => {} },
			extensionTerminalInputSubscriptions: new Set(),
		});

		await context.switchTuiMode(targetMode);
		context.renderer.renderNow();

		const snap = await terminal.snapshot();
		scenarios.push({
			name,
			description,
			dimensions: { columns: cols, rows },
			inputs: {
				initialMode,
				targetMode,
				initialPreference: initialPref,
				livePreference: livePref,
				lines,
				cursorCol: cursorCol ?? null,
				cursorRow: cursorRow ?? null,
				paddingX,
				paddingY,
			},
			expected: {
				showHardwareCursor: context.renderer.getShowHardwareCursor(),
				cursorVisible: snap.cursorVisible,
				cursor: snap.cursor,
				screen: snap.screen,
				rawWrites: terminal.takeWrites(),
				terminalEvents: terminal.takeEvents(),
			},
		});
	};

	// 2. Regular -> Fullscreen with live true
	await runSwitchScenario(
		"switch-regular-to-fullscreen-live-true",
		"Live switch from regular to fullscreen preserving live preference true",
		"regular",
		"fullscreen",
		false,
		true,
		["first line", "second line here", "third"],
		5,
		1,
	);

	// 3. Fullscreen -> Regular with live false
	await runSwitchScenario(
		"switch-fullscreen-to-regular-live-false",
		"Live switch from fullscreen to regular preserving live preference false",
		"fullscreen",
		"regular",
		true,
		false,
		["first line", "second line here", "third"],
		5,
		1,
	);

	// 4. Regular -> Fullscreen with live false
	await runSwitchScenario(
		"switch-regular-to-fullscreen-live-false",
		"Live switch from regular to fullscreen with live preference false",
		"regular",
		"fullscreen",
		false,
		false,
		["first line", "second line here", "third"],
		5,
		1,
	);

	// 5. Fullscreen -> Regular with live true
	await runSwitchScenario(
		"switch-fullscreen-to-regular-live-true",
		"Live switch from fullscreen to regular with live preference true",
		"fullscreen",
		"regular",
		false,
		true,
		["first line", "second line here", "third"],
		5,
		1,
	);

	// 6. Component with null/undefined cursor hides hardware cursor despite preference true
	await runSwitchScenario(
		"switch-null-cursor-hides",
		"Switch with null/undefined cursor hides hardware cursor despite preference true",
		"regular",
		"fullscreen",
		true,
		true,
		["line 0 without cursor", "line 1 without cursor"],
		undefined,
		0,
	);

	// 7. Component nested in Box with padding preserves adjusted cursor coordinates across switch
	await runSwitchScenario(
		"switch-nested-box-padding",
		"Switch with editor nested in padded Box preserves adjusted coordinates and preference true",
		"regular",
		"fullscreen",
		false,
		true,
		["first line", "second line here", "third"],
		5,
		1,
		2, // paddingX = 2
		1, // paddingY = 1
	);

	return {
		sourceEndpoints,
		scenarios,
	};
}
