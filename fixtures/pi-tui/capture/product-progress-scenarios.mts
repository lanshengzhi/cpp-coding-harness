#!/usr/bin/env tsx
import assert from "node:assert/strict";
import { createRequire } from "node:module";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";

export interface ProductProgressScenario {
	name: string;
	description?: string;
	dimensions: { columns: number; rows: number };
	inputs: Record<string, any>;
	expected: Record<string, any>;
}

export async function captureProductProgress(piCheckout: string) {
	const coding = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/coding-agent", relative)).href;
	const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages/tui", relative)).href;

	const { SettingsManager } = await import(coding("src/core/settings-manager.ts"));
	const { createInteractiveTui, createInteractiveTuiReference, InteractiveMode } = await import(
		coding("src/modes/interactive/interactive-mode.ts")
	);
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));
	class ProgressRecordingTerminal extends VirtualTerminal {
		writes: string[] = [];
		events: any[] = [];
		progressActive = false;

		override write(data: string) {
			this.writes.push(data);
			this.events.push({ op: "write", bytes: data });
			super.write(data);
		}

		override setProgress(active: boolean) {
			this.progressActive = active;
			const seq = active ? "\x1b]9;4;3\x07" : "\x1b]9;4;0\x07";
			this.writes.push(seq);
			this.events.push({ op: "setProgress", active, bytes: seq });
			super.setProgress(active);
		}

		clearProgressInterval(): boolean {
			if (!this.progressActive) return false;
			this.progressActive = false;
			return true;
		}

		override stop() {
			this.events.push({ op: "terminal.stop" });
			if (this.clearProgressInterval()) {
				this.write("\x1b]9;4;0\x07");
			}
			super.stop();
		}
	}

	const sourceEndpoints = [
		"packages/coding-agent/src/core/settings-manager.ts:SettingsManager.getShowTerminalProgress,SettingsManager.setShowTerminalProgress",
		"packages/coding-agent/src/modes/interactive/interactive-mode.ts:InteractiveMode.switchTuiMode",
		"packages/tui/src/terminal.ts:Terminal.setProgress",
	];

	const scenarios: ProductProgressScenario[] = [];

	// 1. Settings resolution observations
	{
		const runCase = (globalJson: any, projectJson: any, trusted: boolean) => {
			const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "pi-settings-progress-test-"));
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

			const sm = SettingsManager.create(projectDir, globalDir, { projectTrusted: trusted });
			const result = sm.getShowTerminalProgress();
			fs.rmSync(tempDir, { recursive: true, force: true });
			return result;
		};

		const settingsResults = {
			defaultUnset: runCase(undefined, undefined, false),
			explicitGlobalTrue: runCase({ terminal: { showTerminalProgress: true } }, undefined, false),
			explicitGlobalFalse: runCase({ terminal: { showTerminalProgress: false } }, undefined, false),
			projectScopeTrustedWinsTrue: runCase(
				{ terminal: { showTerminalProgress: false } },
				{ terminal: { showTerminalProgress: true } },
				true,
			),
			projectScopeTrustedWinsFalse: runCase(
				{ terminal: { showTerminalProgress: true } },
				{ terminal: { showTerminalProgress: false } },
				true,
			),
			projectScopeUntrustedIgnored: runCase(
				{ terminal: { showTerminalProgress: true } },
				{ terminal: { showTerminalProgress: false } },
				false,
			),
			projectScopeUntrustedGlobalDefault: runCase(
				undefined,
				{ terminal: { showTerminalProgress: true } },
				false,
			),
			projectScopeTrustedGlobalUnset: runCase(
				undefined,
				{ terminal: { showTerminalProgress: true } },
				true,
			),
		};

		scenarios.push({
			name: "product-progress-settings-resolution",
			description: "Resolution matrix for terminal.showTerminalProgress across scopes",
			dimensions: { columns: 80, rows: 24 },
			inputs: { operation: "getShowTerminalProgress" },
			expected: { settingsResults },
		});
	}

	// 2. Settings persistence (surgical nested write)
	{
		const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), "pi-settings-persist-test-"));
		const globalDir = path.join(tempDir, "global");
		const projectDir = path.join(tempDir, "project");
		fs.mkdirSync(globalDir, { recursive: true });
		fs.mkdirSync(path.join(projectDir, ".pi"), { recursive: true });

		fs.writeFileSync(
			path.join(globalDir, "settings.json"),
			JSON.stringify({ theme: "dark", terminal: { clearOnShrink: true, unknownProp: 42 } }),
		);

		const sm = SettingsManager.create(projectDir, globalDir, { projectTrusted: true });
		const initialResolved = sm.getShowTerminalProgress();
		sm.setShowTerminalProgress(true);
		const afterSetTrue = sm.getShowTerminalProgress();
		const savedFileContent1 = JSON.parse(fs.readFileSync(path.join(globalDir, "settings.json"), "utf8"));

		sm.setShowTerminalProgress(false);
		const afterSetFalse = sm.getShowTerminalProgress();
		const savedFileContent2 = JSON.parse(fs.readFileSync(path.join(globalDir, "settings.json"), "utf8"));

		fs.rmSync(tempDir, { recursive: true, force: true });

		scenarios.push({
			name: "product-progress-settings-persistence",
			description: "Surgical nested persistence of terminal.showTerminalProgress in global settings",
			dimensions: { columns: 80, rows: 24 },
			inputs: { operation: "setShowTerminalProgress" },
			expected: {
				initialResolved,
				afterSetTrue,
				afterSetFalse,
				savedFileContent1,
				savedFileContent2,
			},
		});
	}

	// 3. Mode switch restoration and progress lifecycle on InteractiveMode
	{
		const runSwitchTest = async (
			initialMode: "regular" | "fullscreen",
			targetMode: "regular" | "fullscreen",
			showProgress: boolean,
			isStreaming: boolean,
			isCompacting: boolean,
			restoreProgress: boolean,
		) => {
			const sm = SettingsManager.inMemory();
			sm.setShowTerminalProgress(showProgress);

			const mockSession = {
				isStreaming,
				isCompacting,
				subscribe: () => () => {},
				scopedModels: [],
			};

			const customTerminal = new ProgressRecordingTerminal(80, 24);
			const initialRenderer = createInteractiveTui({
				tuiMode: initialMode,
				terminal: customTerminal,
				showHardwareCursor: false,
			});

			const fakeMode: any = {
				options: { tuiMode: initialMode },
				settingsManager: sm,
				session: mockSession,
				terminal: customTerminal,
				renderer: initialRenderer,
				themeController: { rebindTui: () => {} },
				rebindExtensionTerminalInputListeners: () => {},
				mountInteractiveTui: () => {},
				mainScreenRenderState: undefined,
			};

			initialRenderer.start();
			if (showProgress && (isStreaming || isCompacting)) {
				customTerminal.setProgress(true);
			}
			customTerminal.writes.length = 0;
			customTerminal.events.length = 0;

			const outcome = Reflect.apply(
				(InteractiveMode.prototype as any).switchTuiMode,
				fakeMode,
				[targetMode, restoreProgress, true],
			);

			const capturedWrites = [...customTerminal.writes];
			const capturedEvents = [...customTerminal.events];

			return {
				outcome,
				writes: capturedWrites,
				events: capturedEvents,
				progressActiveEmitted: capturedWrites.includes("\x1b]9;4;3\x07"),
				progressClearEmitted: capturedWrites.includes("\x1b]9;4;0\x07"),
			};
		};

		const switchResults = {
			streamingEnabledRestore: await runSwitchTest("fullscreen", "regular", true, true, false, true),
			compactingEnabledRestore: await runSwitchTest("regular", "fullscreen", true, false, true, true),
			idleEnabledNoRestore: await runSwitchTest("fullscreen", "regular", true, false, false, true),
			streamingDisabledNoRestore: await runSwitchTest("fullscreen", "regular", false, true, false, true),
			streamingEnabledSuppressed: await runSwitchTest("fullscreen", "regular", true, true, false, false),
		};

		scenarios.push({
			name: "product-progress-mode-switch",
			description: "Mode switch restoration conditions and sequence emission",
			dimensions: { columns: 80, rows: 24 },
			inputs: { operation: "switchTuiMode" },
			expected: { switchResults },
		});
	}

	// 4. Session lifecycle events sequence assertions
	{
		const activeSeq = "\x1b]9;4;3\x07";
		const clearSeq = "\x1b]9;4;0\x07";

		scenarios.push({
			name: "product-progress-protocol-constants",
			description: "Exact escape sequences for terminal progress active and clear",
			dimensions: { columns: 80, rows: 24 },
			inputs: { protocol: "OSC 9;4" },
			expected: {
				activeSequence: activeSeq,
				clearSequence: clearSeq,
				turnStartAction: "active",
				agentEndAction: "clear",
				compactionStartAction: "active",
				compactionEndAction: "clear",
				stopAction: "clear",
			},
		});
	}

	return { sourceEndpoints, scenarios };
}
