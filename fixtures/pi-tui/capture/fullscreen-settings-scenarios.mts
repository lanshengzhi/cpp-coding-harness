import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { registerHooks } from "node:module";

/// Independent pi-v1.0.4 observations of the fullscreen settings family (#1020, spec #946 group E20).
///
/// Every value below is produced by frozen pi code through its own seams:
/// `SettingsManager` parsing/merge/getters/setters/reload, the real
/// `SettingsSelectorComponent` rows and cycle actions, `ScrollView` +
/// `renderLayoutFrame` scrollbar cells and column reservation, a real
/// `TuiAltScreen` over pi's `VirtualTerminal` for selection/copy and wheel
/// routing, `createInteractiveTui` option forwarding, and controlled
/// `InteractiveMode.prototype` contexts for `applyRuntimeSettings`,
/// `switchTuiMode` and `handleCopyCommand`. No Pike-side expected value is
/// used to construct these observations.
///
/// Identity gates: ambient `PI_*` variables are removed, `PI_PACKAGE_DIR` is
/// pinned to the frozen checkout, `VERSION` must be 1.0.4, and every
/// `@earendil-works/*` specifier must resolve inside the frozen checkout. The
/// Azure model catalog is only stubbed because the read-only checkout has no
/// generated `packages/ai/src/providers/data/azure.json`; the stub is
/// synchronous through `registerHooks` and is disclosed in `identity`.
export async function captureFullscreenSettings(piCheckout: string) {
	const sanitizedPiVariables: string[] = [];
	for (const key of Object.keys(process.env)) {
		if (key.startsWith("PI_")) {
			sanitizedPiVariables.push(key);
			delete process.env[key];
		}
	}
	const piPackageDir = path.join(piCheckout, "packages/coding-agent");
	process.env.PI_PACKAGE_DIR = piPackageDir;

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
	const piRootUrl = pathToFileURL(piCheckout).href;
	const packageResolutions: string[] = [];
	const foreignResolutions: string[] = [];
	registerHooks({
		resolve(specifier, context, next) {
			if (specifier.startsWith("@earendil-works/")) {
				const [name, ...parts] = specifier.slice("@earendil-works/".length).split("/");
				const directory = directories[name];
				if (directory) {
					const relative = parts.length ? parts.join("/") : "index.ts";
					const url = pathToFileURL(
						path.join(piCheckout, "packages", directory, "src", relative.endsWith(".ts") ? relative : `${relative}.ts`),
					).href;
					packageResolutions.push(`${specifier} -> ${url}`);
					return { url, shortCircuit: true };
				}
			}
			if (specifier === "./data/azure.json" && context.parentURL?.includes("/packages/ai/src/providers/")) {
				return { url: new URL(specifier, context.parentURL).href, shortCircuit: true, format: "json" };
			}
			const resolved = next(specifier, context);
			if (
				!specifier.startsWith("node:") &&
				typeof resolved?.url === "string" &&
				resolved.url.startsWith("file:") &&
				!resolved.url.startsWith(piRootUrl)
			) {
				foreignResolutions.push(`${specifier} -> ${resolved.url}`);
			}
			return resolved;
		},
		load(url, context, next) {
			if (url.endsWith("/providers/data/azure.json")) return { format: "json", source: "{}", shortCircuit: true };
			return next(url, context);
		},
	});

	const source = (relative: string) => pathToFileURL(path.join(piCheckout, "packages", relative)).href;
	const { SettingsManager, InMemorySettingsStorage } = await import(source("coding-agent/src/core/settings-manager.ts"));
	const { VERSION } = await import(source("coding-agent/src/config.ts"));
	const { InteractiveMode } = await import(source("coding-agent/src/modes/interactive/interactive-mode.ts"));
	const { initTheme } = await import(source("coding-agent/src/modes/interactive/theme/theme.ts"));
	const { SettingsSelectorComponent } = await import(source("coding-agent/src/modes/interactive/components/settings-selector.ts"));
	const { createChatViewport } = await import(source("coding-agent/src/modes/interactive/chat-viewport.ts"));
	const { createInteractiveTui, createInteractiveTuiReference } = await import(source("coding-agent/src/modes/interactive/tui-renderer.ts"));
	const { KeybindingsManager } = await import(source("coding-agent/src/core/keybindings.ts"));
	const { Container, Text, setKeybindings } = await import(source("tui/src/index.ts"));
	const { TuiAltScreen } = await import(source("tui/src/tui-alt-screen.ts"));
	const { ScrollView } = await import(source("tui/src/components/scroll-view.ts"));
	const { renderLayoutFrame } = await import(source("tui/src/layout.ts"));
	const { stripTerminalSequences } = await import(source("tui/src/utils.ts"));
	const { WheelScrollAccelerator } = await import(source("tui/src/wheel-scroll.ts"));
	const { VirtualTerminal } = await import(source("tui/test/virtual-terminal.ts"));

	if (VERSION !== "1.0.4") throw new Error(`unexpected frozen VERSION: ${VERSION}`);
	if (foreignResolutions.length > 0) throw new Error(`foreign module resolution: ${foreignResolutions.join(", ")}`);

	initTheme("dark");
	setKeybindings(new KeybindingsManager());

	const identity = {
		frozenVersion: VERSION,
		piPackageDir,
		sanitizedPiVariables,
		azureStub:
			"registerHooks resolve/load stub for packages/ai/src/providers/data/azure.json (generated catalog absent in the read-only checkout)",
		packageSourceResolutions: [...new Set(packageResolutions.map((entry) => entry.split(" -> ")[0]))],
		foreignResolutions,
	};

	const managerWith = (global: Record<string, unknown>, project: Record<string, unknown> = {}) => {
		const storage = new InMemorySettingsStorage();
		storage.withLock("global", () => JSON.stringify(global));
		storage.withLock("project", () => JSON.stringify(project));
		return SettingsManager.fromStorage(storage);
	};
	const readResolved = (manager: any) => {
		const scrollbar = manager.getFullscreenScrollbar();
		const copyOnSelect = manager.getFullscreenCopyOnSelect();
		const wheelScrollLines = manager.getFullscreenWheelScrollLines();
		return {
			fullscreenScrollbar: { value: scrollbar, type: typeof scrollbar },
			fullscreenCopyOnSelect: { value: copyOnSelect, type: typeof copyOnSelect, truthy: Boolean(copyOnSelect) },
			fullscreenWheelScrollLines: { value: wheelScrollLines, type: typeof wheelScrollLines },
		};
	};
	const plainLines = (lines: readonly string[]) => lines.map((line) => stripTerminalSequences(line));
	const sgr = (code: number, x: number, y: number, release = false) => `\x1b[<${code};${x};${y}${release ? "m" : "M"}`;
	const drag = (terminal: any, start: { x: number; y: number }, end: { x: number; y: number }) => {
		terminal.sendInput(sgr(0, start.x, start.y));
		terminal.sendInput(sgr(32, end.x, end.y));
		terminal.sendInput(sgr(0, end.x, end.y, true));
	};

	/// A real fullscreen `TuiAltScreen` over pi's `VirtualTerminal`, mounted with
	/// the product `createChatViewport` layout. `copySelection` is injected so
	/// the frozen copy path is observed instead of writing to the host clipboard.
	function makeFullscreenHarness(options: {
		columns: number;
		rows: number;
		documentLines: string[];
		copyOnSelect: unknown;
		copied: string[];
		wheelScrollLines?: unknown;
	}) {
		const terminal = new VirtualTerminal(options.columns, options.rows);
		terminal.start(() => {}, () => {});
		const document = new Container();
		for (const line of options.documentLines) document.addChild(new Text(line, 0, 0));
		const pendingMessages = new Container();
		const status = new Container();
		const editor = new Container();
		editor.addChild(new Text("editor", 0, 0));
		const footer = new Container();
		const viewport = createChatViewport({
			document,
			pendingMessages,
			status,
			editor,
			footer,
			scrollbar: "hidden",
		});
		const renderer = new TuiAltScreen(terminal, false, "/tmp", {
			copyOnSelect: options.copyOnSelect as any,
			wheelScrollLines: (options.wheelScrollLines ?? "auto") as any,
			copySelection: async (text: string) => {
				options.copied.push(text);
				return true;
			},
		});
		for (const region of [document, pendingMessages, status, editor, footer]) renderer.addChild(region);
		renderer.setLayoutRoot(viewport.root);
		renderer.start();
		return { terminal, renderer, viewport, document };
	}

	const frameOf = (view: any, columns: number, rows: number, requestRender: () => void = () => {}) => {
		const painted = renderLayoutFrame(view, columns, rows, requestRender);
		const findScrollBox = (box: any): any => {
			if (!box) return undefined;
			if (box.scrollView) return box;
			for (const child of box.children ?? []) {
				const found = findScrollBox(child);
				if (found) return found;
			}
			return undefined;
		};
		const scrollBox = findScrollBox(painted.root);
		const child = scrollBox?.children?.[0];
		return {
			lines: plainLines(painted.lines).map((line) => line.trimEnd()),
			window: scrollBox
				? {
						width: scrollBox.rect.width,
						height: scrollBox.rect.height,
						childWidth: child ? child.rect.width : 0,
						childHeight: child ? child.rect.height : 0,
					}
				: null,
		};
	};

	const scenarios: any[] = [];

	// 1. Frozen getter resolution: defaults, valid values, wrong types, bounds,
	// runtime overrides and project precedence. Sources:
	// settings-manager.ts:1368-1400 (getters/setters), :195-212 (deep merge),
	// :649-657 (applyOverrides).
	{
		const cases: any[] = [];
		const pushCase = (name: string, global: Record<string, unknown>, project: Record<string, unknown>, override?: string) => {
			const manager: any = managerWith(global, project);
			if (override) {
				const before = readResolved(manager);
				if (override === "NaN") manager.applyOverrides({ fullscreenWheelScrollLines: Number.NaN });
				if (override === "Infinity") manager.applyOverrides({ fullscreenWheelScrollLines: Number.POSITIVE_INFINITY });
				if (override === "undefined-scrollbar") manager.applyOverrides({ fullscreenScrollbar: undefined });
				if (override === "hidden-and-zero") manager.applyOverrides({ fullscreenScrollbar: "hidden", fullscreenCopyOnSelect: 0 });
				cases.push({ name, inputs: { global, project, override }, expected: { beforeOverride: before, afterOverride: readResolved(manager) } });
				return;
			}
			cases.push({ name, inputs: { global, project }, expected: readResolved(manager) });
		};
		pushCase("defaults", {}, {});
		pushCase("global-valid", { fullscreenScrollbar: "always", fullscreenCopyOnSelect: false, fullscreenWheelScrollLines: 7 });
		pushCase("global-scrollbar-invalid-string", { fullscreenScrollbar: "bogus" });
		pushCase("global-scrollbar-invalid-number", { fullscreenScrollbar: 42 });
		pushCase("global-scrollbar-case-sensitive", { fullscreenScrollbar: "Always" });
		pushCase("global-copy-wrong-type-string-false", { fullscreenCopyOnSelect: "false" });
		pushCase("global-copy-wrong-type-zero", { fullscreenCopyOnSelect: 0 });
		pushCase("global-copy-null-falls-back-true", { fullscreenCopyOnSelect: null });
		pushCase("global-wheel-wrong-type-string", { fullscreenWheelScrollLines: "5" });
		pushCase("global-wheel-zero-floors", { fullscreenWheelScrollLines: 0 });
		pushCase("global-wheel-negative-floors", { fullscreenWheelScrollLines: -5 });
		pushCase("global-wheel-fraction-floors", { fullscreenWheelScrollLines: 1.9 });
		pushCase("global-wheel-high-fraction-clamps", { fullscreenWheelScrollLines: 100.5 });
		pushCase("global-wheel-over-clamps", { fullscreenWheelScrollLines: 1000 });
		pushCase("global-wheel-null-resolves-auto", { fullscreenWheelScrollLines: null });
		pushCase("global-wheel-bool-resolves-auto", { fullscreenWheelScrollLines: true });
		pushCase(
			"project-valid-overrides-global",
			{ fullscreenScrollbar: "auto", fullscreenCopyOnSelect: true, fullscreenWheelScrollLines: 3 },
			{ fullscreenScrollbar: "always", fullscreenCopyOnSelect: false, fullscreenWheelScrollLines: 9 },
		);
		pushCase(
			"project-invalid-overrides-global-valid",
			{ fullscreenScrollbar: "always", fullscreenCopyOnSelect: false, fullscreenWheelScrollLines: 9 },
			{ fullscreenScrollbar: "bogus", fullscreenCopyOnSelect: "false", fullscreenWheelScrollLines: 0 },
		);
		pushCase("project-null-copy-overrides-global-false", { fullscreenCopyOnSelect: false }, { fullscreenCopyOnSelect: null });
		pushCase("apply-overrides-nan-wheel", { fullscreenWheelScrollLines: 7 }, {}, "NaN");
		pushCase("apply-overrides-infinity-wheel", { fullscreenWheelScrollLines: 7 }, {}, "Infinity");
		pushCase("apply-overrides-undefined-scrollbar-keeps", { fullscreenScrollbar: "always" }, {}, "undefined-scrollbar");
		pushCase("apply-overrides-hidden-and-zero-copy", {}, {}, "hidden-and-zero");
		scenarios.push({
			name: "settings-resolution-matrix",
			dimensions: { columns: 80, rows: 24 },
			inputs: {
				seam: "SettingsManager.fromStorage(InMemorySettingsStorage) getters and applyOverrides",
				dimensionsNote: "no terminal is involved; 80x24 is the evidence-schema nominal size",
				cases,
			},
			expected: { cases },
		});
	}

	// 2. File-backed setters: surgical field-level writes into the global file,
	// project file untouched, reload, clamped/invalid setter persistence.
	// Sources: settings-manager.ts:736-768 (save), :682-735 (persistScopedSettings),
	// :605-637 (reload), :1368-1400 (setters).
	{
		const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "pi-1020-settings-"));
		const cwd = path.join(tmp, "project");
		const agentDir = path.join(tmp, "agent");
		fs.mkdirSync(path.join(cwd, ".pi"), { recursive: true });
		fs.mkdirSync(agentDir, { recursive: true });
		const globalPath = path.join(agentDir, "settings.json");
		const projectPath = path.join(cwd, ".pi", "settings.json");
		const globalSeed = {
			theme: "dark",
			fullscreenScrollbar: "auto",
			fullscreenCopyOnSelect: true,
			fullscreenWheelScrollLines: "auto",
			lastChangelogVersion: "0.9.9",
		};
		const projectSeed = { fullscreenWheelScrollLines: 12, unrelatedProject: true };
		fs.writeFileSync(globalPath, `${JSON.stringify(globalSeed, null, 2)}\n`);
		fs.writeFileSync(projectPath, `${JSON.stringify(projectSeed, null, 2)}\n`);
		const manager: any = SettingsManager.create(cwd, agentDir);
		const phase = () => ({
			globalText: fs.readFileSync(globalPath, "utf8"),
			projectText: fs.readFileSync(projectPath, "utf8"),
			resolved: readResolved(manager),
		});
		const before = phase();
		manager.setFullscreenScrollbar("hidden");
		manager.setFullscreenCopyOnSelect(false);
		manager.setFullscreenWheelScrollLines(7);
		await manager.flush();
		const afterSetters = phase();
		await manager.reload();
		const afterReload = readResolved(manager);
		manager.setFullscreenScrollbar("bogus" as any);
		manager.setFullscreenCopyOnSelect("false" as any);
		manager.setFullscreenWheelScrollLines(-3 as any);
		await manager.flush();
		const afterInvalidSetters = phase();
		manager.setFullscreenWheelScrollLines(2.7 as any);
		await manager.flush();
		const afterFractionSetter = phase();
		manager.setFullscreenWheelScrollLines(100000 as any);
		await manager.flush();
		const afterOverSetter = phase();
		manager.setFullscreenWheelScrollLines("auto" as any);
		await manager.flush();
		const afterAutoSetter = phase();
		fs.rmSync(tmp, { recursive: true, force: true });
		scenarios.push({
			name: "settings-setters-surgery-reload",
			dimensions: { columns: 80, rows: 24 },
			inputs: {
				seam: "SettingsManager.create(FileSettingsStorage) setters + flush + reload; exact settings.json texts",
				dimensionsNote: "no terminal is involved; 80x24 is the evidence-schema nominal size",
				globalSeed,
				projectSeed,
				setterCalls: [
					"setFullscreenScrollbar('hidden')",
					"setFullscreenCopyOnSelect(false)",
					"setFullscreenWheelScrollLines(7)",
					"setFullscreenScrollbar('bogus' as any)",
					"setFullscreenCopyOnSelect('false' as any)",
					"setFullscreenWheelScrollLines(-3 as any)",
					"setFullscreenWheelScrollLines(2.7 as any)",
					"setFullscreenWheelScrollLines(100000 as any)",
					"setFullscreenWheelScrollLines('auto' as any)",
				],
			},
			expected: {
				before,
				afterSetters,
				afterReload,
				afterInvalidSetters,
				afterFractionSetter,
				afterOverSetter,
				afterAutoSetter,
			},
		});
	}

	// 3. Real settings-selector rows and cycle actions. The selector's
	// `SettingsList` is driven with Enter (`tui.select.confirm`) exactly like
	// the product overlay, so the recorded callbacks are the actual dispatch
	// values including the wheel `parseInt`. Sources: settings-selector.ts:707-746
	// (rows), :937-978 (callback switch), settings-list.ts:activateItem.
	{
		const baseConfig: any = {
			autoCompact: true,
			defaultModel: "not set",
			availableDefaultModels: [],
			showImages: true,
			imageWidthCells: 60,
			autoResizeImages: true,
			blockImages: false,
			enableSkillCommands: true,
			steeringMode: "one-at-a-time",
			followUpMode: "one-at-a-time",
			transport: "sse",
			httpIdleTimeoutMs: 60000,
			cacheWarmingMode: "off",
			thinkingLevel: "medium",
			availableThinkingLevels: [],
			modelThinkingLevels: {},
			currentTheme: "dark",
			terminalTheme: "dark",
			availableThemes: ["dark", "light"],
			hideThinkingBlock: false,
			mermaidRenderingMode: "off",
			showCacheMissNotices: false,
			collapseChangelog: false,
			enableInstallTelemetry: false,
			doubleEscapeAction: "fork",
			treeFilterMode: "default",
			showHardwareCursor: false,
			editorPaddingX: 0,
			outputPad: 0,
			autocompleteMaxVisible: 5,
			quietStartup: "false",
			defaultProjectTrust: "ask",
			clearOnShrink: false,
			showTerminalProgress: false,
			tuiMode: "fullscreen",
			fullscreenExitOutput: "transcript",
			fullscreenScrollbar: "auto",
			fullscreenCopyOnSelect: true,
			fullscreenWheelScrollLines: "auto",
			warnings: {},
		};
		const observedIds = ["fullscreen-scrollbar", "fullscreen-copy-on-select", "fullscreen-wheel-scroll-lines"];
		const observeSelector = (config: any) => {
			const changes: any[] = [];
			const callbacks: any = {
				onFullscreenScrollbarChange: (value: unknown) => changes.push({ id: "fullscreen-scrollbar", value, type: typeof value }),
				onFullscreenCopyOnSelectChange: (value: unknown) => changes.push({ id: "fullscreen-copy-on-select", value, type: typeof value }),
				onFullscreenWheelScrollLinesChange: (value: unknown) => changes.push({ id: "fullscreen-wheel-scroll-lines", value, type: typeof value }),
				onCancel: () => {},
			};
			const selector = new SettingsSelectorComponent(config, callbacks as any);
			const list: any = selector.getSettingsList();
			const itemObservation = (id: string) => {
				list.selectItem(id);
				const rendered = plainLines(list.render(120));
				const selectedIndex = rendered.findIndex((line) => line.startsWith("→ "));
				const row = selectedIndex >= 0 ? rendered[selectedIndex].trimEnd() : null;
				const description: string[] = [];
				const indicatorIndex = rendered.findIndex((line) => /^\(\d+\/\d+\)$/.test(line.trim()));
				let descriptionIndex = (indicatorIndex >= 0 ? indicatorIndex : selectedIndex) + 1;
				while (descriptionIndex < rendered.length && rendered[descriptionIndex].trim().length === 0) descriptionIndex += 1;
				for (let index = descriptionIndex; index < rendered.length; index++) {
					const line = rendered[index];
					if (line.includes("Type to search") || line.trim().length === 0) break;
					description.push(line.trimEnd());
				}
				return { row, description };
			};
			const rows: Record<string, unknown> = {};
			for (const id of observedIds) rows[id] = itemObservation(id);
			const cycles: Record<string, unknown> = {};
			for (const id of observedIds) {
				list.selectItem(id);
				const steps: any[] = [];
				for (let index = 0; index < 8; index++) {
					const before = changes.length;
					list.handleInput("\r");
					const change = changes[before];
					if (change) steps.push(change);
					const after = itemObservation(id);
					const firstRow = (rows[id] as any).row as string | null;
					if (index > 0 && after.row === firstRow) break;
				}
				cycles[id] = steps;
			}
			return { rows, cycles, changes };
		};
		const defaultObservation = observeSelector({ ...baseConfig });
		const nonDefaultObservation = observeSelector({ ...baseConfig, fullscreenScrollbar: "hidden", fullscreenCopyOnSelect: false, fullscreenWheelScrollLines: 7 });
		const wrongTypeManager = managerWith({ fullscreenCopyOnSelect: "false" });
		const wrongTypeObservation = observeSelector({
			...baseConfig,
			fullscreenCopyOnSelect: wrongTypeManager.getFullscreenCopyOnSelect(),
		});
		scenarios.push({
			name: "settings-selector-rows-and-cycles",
			dimensions: { columns: 120, rows: 24 },
			inputs: {
				seam: "real SettingsSelectorComponent + SettingsList handleInput('\\r'), render(120)",
				observedIds,
				defaultConfig: { fullscreenScrollbar: "auto", fullscreenCopyOnSelect: true, fullscreenWheelScrollLines: "auto" },
				nonDefaultConfig: { fullscreenScrollbar: "hidden", fullscreenCopyOnSelect: false, fullscreenWheelScrollLines: 7 },
				wrongTypeConfig: { fullscreenCopyOnSelectGetter: wrongTypeManager.getFullscreenCopyOnSelect(), getterType: typeof wrongTypeManager.getFullscreenCopyOnSelect() },
			},
			expected: {
				defaultConfig: defaultObservation,
				nonDefaultConfig: nonDefaultObservation,
				wrongTypeCopyOnSelectConfig: wrongTypeObservation,
			},
		});
	}

	// 4. Actual ScrollView scrollbar cells and column reservation for the
	// resolved settings values, including the auto transient timer and the live
	// `setScrollbar` re-render request. Sources: scroll-view.ts:setScrollbar,
	// getContentWidth, isScrollbarVisible, scrollBy; layout.ts:getScrollbarGeometry,
	// paintScrollbar, renderLayoutFrame.
	{
		const viewFor = (resolved: string) => {
			const content = new Text(Array.from({ length: 8 }, (_, index) => `line ${index + 1}`).join("\n"), 0, 0);
			return new ScrollView(content, { scrollbar: resolved as any, follow: "end" });
		};
		const requestRenderCount = { value: 0 };
		const requestRender = () => {
			requestRenderCount.value += 1;
		};
		const observeMode = async (name: string, resolvedFrom: any, resolvedValue: string) => {
			const view: any = viewFor(resolvedValue);
			const initialFrame = frameOf(view, 10, 4, requestRender);
			const initial = {
				resolvedFrom,
				scrollbar: view.scrollbar,
				isScrollbarVisible: view.isScrollbarVisible,
				isScrollbarActive: view.isScrollbarActive,
				scrollTop: view.scrollTop,
				viewportHeight: view.viewportHeight,
				contentWidthAtTen: view.getContentWidth(10),
				contentWidthAtOne: view.getContentWidth(1),
				frame: initialFrame,
			};
			view.scrollBy(-2);
			const scrolledFrame = frameOf(view, 10, 4, requestRender);
			const scrolled = {
				scrollTop: view.scrollTop,
				isScrollbarVisible: view.isScrollbarVisible,
				isScrollbarActive: view.isScrollbarActive,
				contentWidthAtTen: view.getContentWidth(10),
				frame: scrolledFrame,
			};
			await new Promise((resolve) => setTimeout(resolve, 1100));
			const afterTimerFrame = frameOf(view, 10, 4, requestRender);
			const afterTimer = {
				scrollTop: view.scrollTop,
				isScrollbarVisible: view.isScrollbarVisible,
				contentWidthAtTen: view.getContentWidth(10),
				frame: afterTimerFrame,
			};
			const requestRenderBeforeChange = requestRenderCount.value;
			view.setScrollbar(resolvedValue as any);
			const repeatedRequestRenderDelta = requestRenderCount.value - requestRenderBeforeChange;
			const alternate = resolvedValue === "hidden" ? "always" : "hidden";
			view.setScrollbar(alternate as any);
			const changedRequestRenderDelta = requestRenderCount.value - requestRenderBeforeChange;
			const alternateFrame = frameOf(view, 10, 4, requestRender);
			return {
				name,
				resolvedValue,
				initial,
				scrolled,
				afterTimer,
				setScrollbarSameValueRequestRenderDelta: repeatedRequestRenderDelta,
				setScrollbarChangedValueRequestRenderDelta: changedRequestRenderDelta,
				alternateMode: alternate,
				alternateFrame: { lines: alternateFrame.lines, window: alternateFrame.window, isScrollbarVisible: view.isScrollbarVisible },
			};
		};
		const cases: any[] = [];
		cases.push(await observeMode("default-auto", "SettingsManager.getFullscreenScrollbar() (default)", managerWith({}).getFullscreenScrollbar()));
		cases.push(await observeMode("valid-always", "SettingsManager.getFullscreenScrollbar() (persisted always)", managerWith({ fullscreenScrollbar: "always" }).getFullscreenScrollbar()));
		cases.push(await observeMode("valid-hidden", "SettingsManager.getFullscreenScrollbar() (persisted hidden)", managerWith({ fullscreenScrollbar: "hidden" }).getFullscreenScrollbar()));
		cases.push(await observeMode("invalid-resolves-auto", "SettingsManager.getFullscreenScrollbar() (persisted bogus)", managerWith({ fullscreenScrollbar: "bogus" }).getFullscreenScrollbar()));
		scenarios.push({
			name: "scrollbar-modes-cells-and-reservation",
			dimensions: { columns: 10, rows: 4 },
			inputs: {
				seam: "real ScrollView over Text with renderLayoutFrame(root, 10, 4) and frozen auto hide timer (1100 ms wait)",
				contentLines: 8,
				viewportRows: 4,
				cases: cases.map((entry) => ({ name: entry.name, resolvedValue: entry.resolvedValue, resolvedFrom: entry.initial.resolvedFrom })),
			},
			expected: { cases },
		});
	}

	// 5. Resolved copy-on-select value at the real fullscreen consumer, the
	// manual Ctrl+X product branch, the live `setCopyOnSelect` transition, and
	// `createInteractiveTui` option forwarding. Sources: settings-manager.ts:1380-1386;
	// tui-renderer.ts:37; tui-alt-screen.ts:handleSelectionMouseEvent,
	// copyActiveSelectionToClipboard, setCopyOnSelect; interactive-mode.ts:6600-6618
	// (handleCopyCommand preferSelection).
	{
		const cases: any[] = [];
		const resolvedCases = [
			{ name: "default-true", manager: managerWith({}) },
			{ name: "valid-false", manager: managerWith({ fullscreenCopyOnSelect: false }) },
			{ name: "wrong-type-string-false", manager: managerWith({ fullscreenCopyOnSelect: "false" }) },
			{ name: "wrong-type-zero", manager: managerWith({ fullscreenCopyOnSelect: 0 }) },
		];
		for (const entry of resolvedCases) {
			const resolved = entry.manager.getFullscreenCopyOnSelect();
			const copied: string[] = [];
			const harness = makeFullscreenHarness({
				columns: 20,
				rows: 6,
				documentLines: ["alpha beta gamma", "delta epsilon"],
				copyOnSelect: resolved,
				copied,
			});
			await harness.terminal.waitForRender();
			const getter = harness.renderer.getCopyOnSelect();
			drag(harness.terminal, { x: 1, y: 1 }, { x: 6, y: 1 });
			await harness.terminal.waitForRender();
			const selectionText = harness.renderer.getActiveSelectionText();
			const hasSelectionBeforeManual = harness.renderer.hasActiveSelection();
			const afterRelease = [...copied];
			const errors: string[] = [];
			const context: any = Object.create(InteractiveMode.prototype);
			context.ui = harness.renderer;
			context.runtimeHost = { session: { getLastAssistantText: () => undefined } };
			context.showError = (message: string) => errors.push(message);
			context.showStatus = () => {};
			await (InteractiveMode.prototype as any).handleCopyCommand.call(context, { flashConfirmation: true, preferSelection: true });
			await harness.terminal.waitForRender();
			const afterManualCtrlX = [...copied];
			const hasSelectionAfterManual = harness.renderer.hasActiveSelection();
			harness.renderer.stop();
			const forwarded = createInteractiveTui({
				tuiMode: "fullscreen",
				showHardwareCursor: false,
				logDirectory: "/tmp",
				terminal: new VirtualTerminal(20, 6),
				fullscreenCopyOnSelect: resolved as any,
				fullscreenWheelScrollLines: "auto",
			} as any);
			cases.push({
				name: entry.name,
				resolvedGetter: { value: resolved, type: typeof resolved, truthy: Boolean(resolved) },
				rendererGetter: { value: getter, type: typeof getter },
				hasSelectionBeforeManual,
				hasSelectionAfterManual,
				selectionText,
				afterRelease,
				afterManualCtrlX,
				manualCtrlXErrors: errors,
				createdTuiForwardedCopyOnSelect: { value: forwarded.getCopyOnSelect(), type: typeof forwarded.getCopyOnSelect() },
			});
			forwarded.stop();
		}
		// Live setter transition used by the settings-selector callback.
		const copied: string[] = [];
		const harness = makeFullscreenHarness({ columns: 20, rows: 6, documentLines: ["alpha beta gamma"], copyOnSelect: false, copied });
		await harness.terminal.waitForRender();
		drag(harness.terminal, { x: 1, y: 1 }, { x: 6, y: 1 });
		await harness.terminal.waitForRender();
		const falsePhase = { getter: harness.renderer.getCopyOnSelect(), afterRelease: [...copied] };
		harness.renderer.setCopyOnSelect(true);
		const truePhaseBeforeDrag = { getter: harness.renderer.getCopyOnSelect(), afterRelease: [...copied] };
		drag(harness.terminal, { x: 1, y: 1 }, { x: 6, y: 1 });
		await harness.terminal.waitForRender();
		const truePhaseAfterDrag = { getter: harness.renderer.getCopyOnSelect(), afterRelease: [...copied] };
		harness.renderer.stop();
		scenarios.push({
			name: "copy-on-select-settings-and-ctrl-x",
			dimensions: { columns: 20, rows: 6 },
			inputs: {
				seam:
					"real TuiAltScreen over VirtualTerminal with injected copySelection sink; controlled InteractiveMode.prototype.handleCopyCommand context (runtimeHost.session.getLastAssistantText -> undefined)",
				drag: [{ x: 1, y: 1 }, { x: 6, y: 1 }],
				resolvedCases: resolvedCases.map((entry) => ({ name: entry.name })),
			},
			expected: { cases, liveSetCopyOnSelect: { falsePhase, truePhaseBeforeDrag, truePhaseAfterDrag } },
		});
	}

	// 6. Wheel-scroll settings: the frozen accelerator matrix and per-renderer
	// scroll deltas for resolved fixed/auto values, plus live setWheelScrollLines
	// and createInteractiveTui forwarding. Sources: wheel-scroll.ts:next,setLines;
	// tui-alt-screen.ts:parseWheelEvent,routeWheel,setWheelScrollLines;
	// tui-renderer.ts:38.
	{
		const acceleratorMatrix = () => {
			const autoBurst20 = new WheelScrollAccelerator("auto");
			const autoSteady100 = new WheelScrollAccelerator("auto");
			const directionChange = new WheelScrollAccelerator("auto");
			const noAccelerate = new WheelScrollAccelerator("auto", false);
			const fixedFive = new WheelScrollAccelerator(5);
			const fixedZero = new WheelScrollAccelerator(0);
			const fixedFraction = new WheelScrollAccelerator(2.9);
			return {
				autoBurst20: [0, 20, 40, 60].map((now) => autoBurst20.next(-1, now)),
				autoSteady100: [0, 100, 200, 300].map((now) => autoSteady100.next(-1, now)),
				directionChange: [directionChange.next(-1, 0), directionChange.next(-1, 20), directionChange.next(1, 40), directionChange.next(1, 60)],
				noAccelerate: [noAccelerate.next(-1, 0), noAccelerate.next(-1, 20), noAccelerate.next(-1, 40)],
				fixedFive: [fixedFive.next(-1, 0), fixedFive.next(-1, 10), fixedFive.next(-1, 20)],
				fixedZero: [fixedZero.next(-1, 0), fixedZero.next(-1, 10)],
				fixedFraction: [fixedFraction.next(-1, 0), fixedFraction.next(-1, 10)],
			};
		};
		const documentLines = Array.from({ length: 40 }, (_, index) => `line ${index + 1}`);
		const wheelDeltas = async (wheelValue: unknown, clockSteps: number[] | undefined) => {
			const copied: string[] = [];
			const harness = makeFullscreenHarness({ columns: 20, rows: 10, documentLines, copyOnSelect: true, copied, wheelScrollLines: wheelValue });
			await harness.terminal.waitForRender();
			const transcript: any = harness.viewport.transcript;
			const originalNow = performance.now;
			let clock = 0;
			if (clockSteps) Object.defineProperty(performance, "now", { configurable: true, value: () => clock });
			const deltas: any[] = [];
			try {
				if (clockSteps) {
					for (const advanceMs of clockSteps) {
						clock += advanceMs;
						const before = transcript.scrollTop;
						harness.terminal.sendInput(sgr(64, 2, 2));
						await harness.terminal.waitForRender();
						deltas.push({ advanceMs, before, after: transcript.scrollTop, delta: transcript.scrollTop - before });
					}
				} else {
					for (let index = 0; index < 3; index++) {
						const before = transcript.scrollTop;
						harness.terminal.sendInput(sgr(64, 2, 2));
						await harness.terminal.waitForRender();
						deltas.push({ before, after: transcript.scrollTop, delta: transcript.scrollTop - before });
					}
				}
			} finally {
				if (clockSteps) Object.defineProperty(performance, "now", { configurable: true, value: originalNow });
			}
			const scrolledTop = transcript.scrollTop;
			harness.renderer.stop();
			return { deltas, scrolledTop };
		};
		const fixedFive = await wheelDeltas(managerWith({ fullscreenWheelScrollLines: 5 }).getFullscreenWheelScrollLines(), undefined);
		const invalidZero = await wheelDeltas(managerWith({ fullscreenWheelScrollLines: 0 }).getFullscreenWheelScrollLines(), undefined);
		const auto = await wheelDeltas(managerWith({}).getFullscreenWheelScrollLines(), [20, 20, 20, 300]);
		const invalidString = await wheelDeltas(managerWith({ fullscreenWheelScrollLines: "5" }).getFullscreenWheelScrollLines(), [20, 20, 20, 300]);
		// Live setWheelScrollLines and createInteractiveTui forwarding.
		const liveCopied: string[] = [];
		const liveHarness = makeFullscreenHarness({ columns: 20, rows: 10, documentLines, copyOnSelect: true, copied: liveCopied, wheelScrollLines: "auto" });
		await liveHarness.terminal.waitForRender();
		const liveTranscript: any = liveHarness.viewport.transcript;
		liveHarness.renderer.setWheelScrollLines(5);
		const liveFixed: number[] = [];
		for (let index = 0; index < 2; index++) {
			const before = liveTranscript.scrollTop;
			liveHarness.terminal.sendInput(sgr(64, 2, 2));
			await liveHarness.terminal.waitForRender();
			liveFixed.push(liveTranscript.scrollTop - before);
		}
		liveHarness.renderer.setWheelScrollLines("auto");
		const originalNow = performance.now;
		let clock = 0;
		Object.defineProperty(performance, "now", { configurable: true, value: () => clock });
		const liveAuto: any[] = [];
		try {
			for (const advanceMs of [20, 20]) {
				clock += advanceMs;
				const before = liveTranscript.scrollTop;
				liveHarness.terminal.sendInput(sgr(64, 2, 2));
				await liveHarness.terminal.waitForRender();
				liveAuto.push({ advanceMs, delta: liveTranscript.scrollTop - before });
			}
		} finally {
			Object.defineProperty(performance, "now", { configurable: true, value: originalNow });
		}
		liveHarness.renderer.stop();
		const forwardedTerminal = new VirtualTerminal(20, 10);
		const forwardedFive = createInteractiveTui({
			tuiMode: "fullscreen",
			showHardwareCursor: false,
			logDirectory: "/tmp",
			terminal: forwardedTerminal,
			fullscreenCopyOnSelect: true,
			fullscreenWheelScrollLines: 5,
		} as any);
		const forwardedDocument = new Container();
		for (const line of documentLines) forwardedDocument.addChild(new Text(line, 0, 0));
		const forwardedPending = new Container();
		const forwardedStatus = new Container();
		const forwardedEditor = new Container();
		forwardedEditor.addChild(new Text("editor", 0, 0));
		const forwardedFooter = new Container();
		const forwardedViewport = createChatViewport({
			document: forwardedDocument,
			pendingMessages: forwardedPending,
			status: forwardedStatus,
			editor: forwardedEditor,
			footer: forwardedFooter,
			scrollbar: "hidden",
		});
		for (const region of [forwardedDocument, forwardedPending, forwardedStatus, forwardedEditor, forwardedFooter]) forwardedFive.addChild(region);
		forwardedFive.setLayoutRoot(forwardedViewport.root);
		forwardedFive.start();
		await forwardedTerminal.waitForRender();
		const forwardedDeltas: number[] = [];
		for (let index = 0; index < 2; index++) {
			const before = forwardedViewport.transcript.scrollTop;
			forwardedTerminal.sendInput(sgr(64, 2, 2));
			await forwardedTerminal.waitForRender();
			forwardedDeltas.push(forwardedViewport.transcript.scrollTop - before);
		}
		forwardedFive.stop();
		scenarios.push({
			name: "wheel-settings-fixed-vs-auto",
			dimensions: { columns: 20, rows: 10 },
			inputs: {
				seam:
					"real WheelScrollAccelerator.next explicit clocks; real TuiAltScreen + createChatViewport wheel SGR events with controlled performance.now for auto; createInteractiveTui option forwarding",
				contentLines: 40,
				autoClockStepsMs: [20, 20, 20, 300],
			},
			expected: {
				acceleratorMatrix: acceleratorMatrix(),
				resolvedFixedFive: fixedFive,
				resolvedInvalidZero: invalidZero,
				resolvedAuto: auto,
				resolvedInvalidString: invalidString,
				liveSetWheelScrollLines: { fixedFiveDeltas: liveFixed, autoDeltas: liveAuto },
				createInteractiveTuiForwardedFixedFiveDeltas: forwardedDeltas,
			},
		});
	}

	// 7. Product live application and mode continuity: the actual private
	// `applyRuntimeSettings` applies the resolved values to the real transcript
	// ScrollView and fullscreen renderer; the actual `switchTuiMode` builds the
	// next renderer from the settings manager and updates `options.tuiMode`.
	// Sources: interactive-mode.ts:2039-2034(applyFullscreenScrollbarSetting,
	// applyRuntimeSettings), :883-931 (switchTuiMode), :903-911 (renderer options).
	{
		const settings: any = managerWith({ fullscreenScrollbar: "auto", fullscreenCopyOnSelect: true, fullscreenWheelScrollLines: "auto" });
		const copied: string[] = [];
		const harness = makeFullscreenHarness({
			columns: 20,
			rows: 10,
			documentLines: Array.from({ length: 40 }, (_, index) => `line ${index + 1}`),
			copyOnSelect: settings.getFullscreenCopyOnSelect(),
			copied,
			wheelScrollLines: settings.getFullscreenWheelScrollLines(),
		});
		await harness.terminal.waitForRender();
		const defaultEditor = { setPaddingX: () => {}, setAutocompleteMaxVisible: () => {} };
		const context: any = Object.create(InteractiveMode.prototype);
		context.runtimeHost = {
			session: {
				settingsManager: settings,
				sessionManager: { getCwd: () => "/tmp" },
				autoCompactionEnabled: false,
				isStreaming: false,
				isCompacting: false,
			},
		};
		context.renderer = harness.renderer;
		context.fullscreenLayoutRoot = harness.viewport.root;
		context.transcriptScrollView = harness.viewport.transcript;
		context.statusContainer = { clear: () => {} };
		context.footer = { setSession: () => {}, setAutoCompactEnabled: () => {} };
		context.footerDataProvider = { setCwd: () => {} };
		context.options = { tuiMode: "fullscreen" };
		context.themeController = { rebindTui: () => {} };
		context.extensionTerminalInputSubscriptions = new Set();
		context.defaultEditor = defaultEditor;
		context.editor = defaultEditor;
		context.ui = createInteractiveTuiReference(() => context.renderer);
		const transcript: any = harness.viewport.transcript;
		const wheelUp = () => harness.terminal.sendInput(sgr(64, 2, 2));
		const beforeApply = {
			transcriptScrollbar: transcript.scrollbar,
			rendererCopyOnSelect: harness.renderer.getCopyOnSelect(),
			optionsTuiMode: context.options.tuiMode,
		};
		settings.setFullscreenScrollbar("always");
		settings.setFullscreenCopyOnSelect(false);
		settings.setFullscreenWheelScrollLines(5);
		(InteractiveMode.prototype as any).applyRuntimeSettings.call(context);
		const appliedDeltas: number[] = [];
		for (let index = 0; index < 2; index++) {
			const before = transcript.scrollTop;
			wheelUp();
			await harness.terminal.waitForRender();
			appliedDeltas.push(transcript.scrollTop - before);
		}
		const afterApply = {
			transcriptScrollbar: transcript.scrollbar,
			rendererCopyOnSelect: harness.renderer.getCopyOnSelect(),
			wheelDeltas: appliedDeltas,
			settingsResolved: readResolved(settings),
			optionsTuiMode: context.options.tuiMode,
		};
		const regularResult = (InteractiveMode.prototype as any).switchTuiMode.call(context, "regular", false, true);
		await harness.terminal.waitForRender();
		const afterRegularSwitch = {
			result: regularResult,
			rendererMode: context.renderer.mode,
			optionsTuiMode: context.options.tuiMode,
			transcriptScrollbar: transcript.scrollbar,
		};
		const backResult = (InteractiveMode.prototype as any).switchTuiMode.call(context, "fullscreen", false, true);
		await harness.terminal.waitForRender();
		const backRenderer: any = context.renderer;
		const backDeltas: number[] = [];
		for (let index = 0; index < 2; index++) {
			const before = transcript.scrollTop;
			wheelUp();
			await harness.terminal.waitForRender();
			backDeltas.push(transcript.scrollTop - before);
		}
		const afterBackSwitch = {
			result: backResult,
			rendererMode: backRenderer.mode,
			rendererCopyOnSelect: { value: backRenderer.getCopyOnSelect(), type: typeof backRenderer.getCopyOnSelect() },
			wheelDeltas: backDeltas,
			optionsTuiMode: context.options.tuiMode,
			transcriptScrollbar: transcript.scrollbar,
			transcriptScrollTop: transcript.scrollTop,
		};
		backRenderer.stop();
		scenarios.push({
			name: "product-live-apply-and-mode-switch",
			dimensions: { columns: 20, rows: 10 },
			inputs: {
				seam:
					"controlled InteractiveMode.prototype context (private applyRuntimeSettings / switchTuiMode) over a real TuiAltScreen + createChatViewport; stubs: footer, footerDataProvider, statusContainer, defaultEditor padding setters, themeController.rebindTui, runtimeHost.session",
				settingsBefore: { fullscreenScrollbar: "auto", fullscreenCopyOnSelect: true, fullscreenWheelScrollLines: "auto" },
				settingsAfter: { fullscreenScrollbar: "always", fullscreenCopyOnSelect: false, fullscreenWheelScrollLines: 5 },
			},
			expected: { beforeApply, afterApply, afterRegularSwitch, afterBackSwitch },
		});
	}

	return {
		sourceEndpoints: [
			"packages/coding-agent/src/core/settings-manager.ts:getFullscreenScrollbar,setFullscreenScrollbar,getFullscreenCopyOnSelect,setFullscreenCopyOnSelect,getFullscreenWheelScrollLines,setFullscreenWheelScrollLines",
			"packages/coding-agent/src/core/settings-manager.ts:deepMergeObjects,applyOverrides,save,reload,persistScopedSettings,fromStorage",
			"packages/coding-agent/src/modes/interactive/components/settings-selector.ts:fullscreen-scrollbar,fullscreen-copy-on-select,fullscreen-wheel-scroll-lines,onFullscreenScrollbarChange,onFullscreenCopyOnSelectChange,onFullscreenWheelScrollLinesChange",
			"packages/tui/src/components/settings-list.ts:SettingsList.activateItem,SettingsList.handleInput,SettingsList.render",
			"packages/tui/src/components/scroll-view.ts:ScrollView.setScrollbar,ScrollView.getContentWidth,ScrollView.isScrollbarVisible,ScrollView.scrollBy",
			"packages/tui/src/layout.ts:getScrollbarGeometry,paintScrollbar,renderLayoutFrame",
			"packages/tui/src/tui-alt-screen.ts:handleSelectionMouseEvent,copySelectionToClipboard,copyActiveSelectionToClipboard,setCopyOnSelect,setWheelScrollLines,parseWheelEvent,routeWheel",
			"packages/tui/src/wheel-scroll.ts:WheelScrollAccelerator.next,WheelScrollAccelerator.setLines",
			"packages/coding-agent/src/modes/interactive/tui-renderer.ts:createInteractiveTui",
			"packages/coding-agent/src/modes/interactive/chat-viewport.ts:createChatViewport",
			"packages/coding-agent/src/modes/interactive/interactive-mode.ts:applyFullscreenScrollbarSetting,applyRuntimeSettings,switchTuiMode,handleCopyCommand",
			"packages/tui/test/virtual-terminal.ts:VirtualTerminal.start,sendInput,waitForRender",
		],
		scenarios,
		identity,
	};
}
