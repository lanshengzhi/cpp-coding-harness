import { pathToFileURL } from "node:url";
import path from "node:path";

/// Independent pi-v1.0.4 observation of normalized mouse routing (#1002).
///
/// Every value below comes from pi's own modules: `getLayoutBoxesAt` and
/// `dispatchMouseEvent`/`retargetMouseEvent` for the cached-frame hit test and
/// the coordinate transform, `MouseRegion`/`Box`/`Container` for the forwarding
/// chain, and a real `TuiAltScreen`/`TuiMainScreen` over pi's `VirtualTerminal`
/// for the reports a terminal actually sends (pi
/// `test/tui-alt-screen.test.ts:1817,1846,1871,1899,1923`). The C++ replay reads
/// the artifact through `scripts/tui/evidence.py`; nothing here is derived from
/// the C++ side.
export async function captureMouse(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { MouseRegion } = await import(source("src/components/mouse-region.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { Box } = await import(source("src/components/box.ts"));
	const { Container, dispatchMouseEvent, retargetMouseEvent } = await import(source("src/tui.ts"));
	const { getLayoutBoxesAt, renderLayoutFrame } = await import(source("src/layout.ts"));
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { TuiMainScreen } = await import(source("src/tui-main-screen.ts"));
	const { Input } = await import(source("src/components/input.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	/// A component identified by name in the frozen observations.
	const names = new WeakMap<object, string>();
	const named = <T extends object>(component: T, name: string): T => {
		names.set(component, name);
		return component;
	};
	const nameOf = (component: any) => (component === null || component === undefined ? null : names.get(component) ?? "?");

	/// A terminal that records every write while behaving exactly like the
	/// virtual terminal (pi's own `RecordingTerminal` is test-local).
	const recording = (terminal: any) => {
		const writes: string[] = [];
		const proxy = new Proxy(terminal, {
			get(target, property) {
				if (property === "write") {
					return (data: string) => {
						writes.push(data);
						target.write(data);
					};
				}
				const value = Reflect.get(target, property);
				return typeof value === "function" ? value.bind(target) : value;
			},
		});
		return { terminal: proxy, writes };
	};

	/// One normalized report as the alternate-screen root builds it (pi
	/// `tui-alt-screen.ts:803-826`): local coordinates equal the screen cell and
	/// the component bounds are the terminal dimensions.
	const reportEvent = (type: string, button: number, x: number, y: number, columns: number, rows: number) => ({
		type,
		button: type === "wheel" ? "none" : ["left", "middle", "right", "none"][button & 3],
		x,
		y,
		screenX: x,
		screenY: y,
		width: Math.max(1, columns),
		height: Math.max(1, rows),
		shift: (button & 4) !== 0,
		alt: (button & 8) !== 0,
		ctrl: (button & 16) !== 0,
	});

	const scenarios: any[] = [];

	// A cached frame is the hit-test geometry: every box whose clip contains the
	// cell is offered the report deepest first, and the coordinate transform is
	// `screen - box.rect` (pi `layout.ts:415-425`, `tui-alt-screen.ts:832-856`).
	// The padding cell, the two rows of the first region, the second region's row
	// and a cell outside the box distinguish a padding-blind hit test, a
	// row-blind walk and a depth-blind order.
	{
		const observations: any[] = [];
		const alpha = named(
			new MouseRegion(new Text("aa\nbb", 0, 0), (event: any) => {
				observations.push({ region: "alpha", ...event });
				return event.type === "press" ? { handled: true, capture: true, focus: true } : { handled: true };
			}),
			"alpha",
		);
		const beta = named(
			new MouseRegion(new Text("cc", 0, 0), (event: any) => {
				observations.push({ region: "beta", ...event });
				return undefined;
			}),
			"beta",
		);
		const inner = named(new Container(), "inner");
		inner.addChild(alpha);
		inner.addChild(beta);
		const padded = named(new Box(1, 1), "padded");
		padded.addChild(inner);

		const renders = { alpha: 0, beta: 0, inner: 0, padded: 0 };
		const countRender = (component: any, key: keyof typeof renders) => {
			const original = component.render.bind(component);
			component.render = (width: number) => {
				renders[key] += 1;
				return original(width);
			};
		};
		countRender(alpha, "alpha");
		countRender(beta, "beta");
		countRender(inner, "inner");
		countRender(padded, "padded");

		const painted = renderLayoutFrame(padded, 10, 6, () => {});
		const renderedBeforeProbes = { ...renders };
		const probes = [
			{ x: 0, y: 0, cell: "padding-top-left" },
			{ x: 1, y: 1, cell: "alpha-row-0" },
			{ x: 1, y: 2, cell: "alpha-row-1" },
			{ x: 1, y: 3, cell: "beta-row-0" },
			{ x: 9, y: 0, cell: "outside" },
		];
		const cells = probes.map((probe) => {
			const boxes = getLayoutBoxesAt(painted, probe.x, probe.y);
			const visited = new Set<any>();
			let result: any = null;
			const dispatchedTo: string[] = [];
			for (const box of boxes) {
				if (visited.has(box.component)) continue;
				visited.add(box.component);
				dispatchedTo.push(nameOf(box.component));
				const candidate = dispatchMouseEvent(box.component, {
					...reportEvent("press", 0, probe.x, probe.y, 10, 6),
					x: probe.x - box.rect.x,
					y: probe.y - box.rect.y,
					width: box.rect.width,
					height: box.rect.height,
				});
				if (candidate) {
					result = {
						handled: candidate.handled,
						capture: candidate.capture ?? false,
						focus: candidate.focus ?? false,
						render: candidate.render ?? null,
						target: nameOf(candidate.target.component),
						focusTarget: candidate.focusTarget === undefined ? null : nameOf(candidate.focusTarget),
						local: { x: candidate.target.originX, y: candidate.target.originY },
						size: { width: candidate.target.width, height: candidate.target.height },
					};
					break;
				}
			}
			const target = result
				? { component: undefined as any, originX: 0, originY: 0, width: 0, height: 0 }
				: null;
			void target;
			return {
				...probe,
				boxes: boxes.map((box: any) => ({
					component: nameOf(box.component),
					rect: { x: box.rect.x, y: box.rect.y, width: box.rect.width, height: box.rect.height },
				})),
				dispatchedTo,
				result,
			};
		});
		const renderedAfterProbes = { ...renders };
		const lines = painted.lines.map((line: string) => line.trimEnd());
		const retarget = (() => {
			const event = reportEvent("drag", 32, 4, 3, 10, 6);
			const box = getLayoutBoxesAt(painted, 1, 2)[0];
			const target = {
				component: box.component,
				originX: box.rect.x,
				originY: box.rect.y,
				width: box.rect.width,
				height: box.rect.height,
			};
			const retargeted = retargetMouseEvent(event, target);
			return { target: nameOf(target.component), local: { x: retargeted.x, y: retargeted.y }, size: { width: retargeted.width, height: retargeted.height } };
		})();

		scenarios.push({
			name: "cached-frame-hit-test",
			dimensions: { columns: 10, rows: 6 },
			inputs: {
				tree: { box: { paddingX: 1, paddingY: 1 }, children: ["alpha: aa\\nbb", "beta: cc"] },
				probes,
				retarget: { event: { type: "drag", button: 32, x: 4, y: 3 }, cell: { x: 1, y: 2 } },
			},
			expected: { lines, renderedBeforeProbes, renderedAfterProbes, cells, observations, retarget },
		});
	}

	// A press captures drag and release for the component it landed on, wherever
	// the pointer goes, and the gesture gives it keyboard focus (pi
	// `test/tui-alt-screen.test.ts:1846-1870`).
	{
		const terminal = new VirtualTerminal(20, 2);
		const tui = new TuiAltScreen(terminal);
		const events: any[] = [];
		const component = named(
			{
				render: () => ["control"],
				invalidate: () => {},
				handleMouse: (event: any) => {
					events.push({ type: event.type, x: event.x, y: event.y, screenX: event.screenX, screenY: event.screenY, width: event.width, height: event.height });
					return event.type === "press" ? { handled: true, capture: true, focus: true } : { handled: true };
				},
			},
			"control",
		);
		tui.addChild(component);
		tui.start();
		await terminal.waitForRender();

		terminal.sendInput("\x1b[<0;1;1M");
		terminal.sendInput("\x1b[<32;5;2M");
		terminal.sendInput("\x1b[<0;5;2m");
		await terminal.waitForRender();
		const gesture = {
			events,
			focused: nameOf(tui.getFocusedComponent()),
			viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
		};
		tui.stop();

		// A delegating host that handles keyboard input keeps focus when a nested
		// control is clicked (pi `tui.ts:81-89`, `372-396`).
		const hostTerminal = new VirtualTerminal(20, 2);
		const hostTui = new TuiAltScreen(hostTerminal);
		class FocusHost extends Container {
			handleInput() {}
		}
		const host = named(new FocusHost(), "host");
		const nested = named(new MouseRegion(new Text("nested", 0, 0), () => ({ focus: true })), "nested");
		host.addChild(nested);
		hostTui.addChild(host);
		hostTui.start();
		await hostTerminal.waitForRender();
		hostTerminal.sendInput("\x1b[<0;1;1M");
		await hostTerminal.waitForRender();
		const hostFocus = { focused: nameOf(hostTui.getFocusedComponent()) };
		hostTui.stop();

		scenarios.push({
			name: "capture-drag-and-host-focus",
			dimensions: { columns: 20, rows: 2 },
			inputs: {
				reports: ["\x1b[<0;1;1M", "\x1b[<32;5;2M", "\x1b[<0;5;2m"],
				host: { reports: ["\x1b[<0;1;1M"], hostHandlesInput: true },
			},
			expected: { gesture, hostFocus },
		});
	}

	// A wheel report a component consumes leaves the viewport where it is; an
	// unconsumed one scrolls it (pi `test/tui-alt-screen.test.ts:1899-1930`).
	{
		const consumed = new VirtualTerminal(20, 3);
		const consumedTui = new TuiAltScreen(consumed);
		let wheelEvents = 0;
		let wheelDelta: number | null = null;
		consumedTui.addChild(
			new MouseRegion(new Text(Array.from({ length: 8 }, (_, index) => `line ${index + 1}`).join("\n"), 0, 0), (event: any) => {
				if (event.type !== "wheel") return undefined;
				wheelEvents += 1;
				wheelDelta = event.wheelDelta ?? null;
				return { handled: true };
			}),
		);
		consumedTui.start();
		await consumed.waitForRender();
		const consumedTop = consumedTui.viewportTop;
		consumed.sendInput("\x1b[<64;1;1M");
		await consumed.waitForRender();
		const consumedReport = {
			wheelEvents,
			wheelDelta,
			viewportTop: consumedTui.viewportTop,
			viewport: consumed.getViewport().map((line: string) => line.trimEnd()),
		};
		consumedTui.stop();

		const open = new VirtualTerminal(20, 3);
		const openTui = new TuiAltScreen(open);
		openTui.addChild(new Text(Array.from({ length: 8 }, (_, index) => `line ${index + 1}`).join("\n"), 0, 0));
		openTui.start();
		await open.waitForRender();
		const phases: any[] = [{ action: "initial", viewportTop: openTui.viewportTop, viewport: open.getViewport().map((line: string) => line.trimEnd()) }];
		for (const [action, report] of [["wheel-up", "\x1b[<64;1;1M"], ["wheel-down", "\x1b[<65;1;1M"]] as Array<[string, string]>) {
			open.sendInput(report);
			await open.waitForRender();
			phases.push({ action, viewportTop: openTui.viewportTop, viewport: open.getViewport().map((line: string) => line.trimEnd()) });
		}
		openTui.stop();

		scenarios.push({
			name: "wheel-consumed-and-unconsumed",
			dimensions: { columns: 20, rows: 3 },
			inputs: {
				documentLines: 8,
				consumedReport: "\x1b[<64;1;1M",
				unconsumedReports: ["\x1b[<64;1;1M", "\x1b[<65;1;1M"],
			},
			expected: { consumedTop, consumedReport, phases },
		});
	}

	// Handled pointer motion neither re-renders nor writes, and consecutive
	// presses on one cell count up while movement in between restarts the count
	// (pi `test/tui-alt-screen.test.ts:1871-1898,1923-1950`).
	{
		const virtual = new VirtualTerminal(20, 2);
		const recorded = recording(virtual);
		const terminal = recorded.terminal;
		const tui = new TuiAltScreen(terminal as any);
		const renderCount = { value: 0 };
		const events: any[] = [];
		tui.addChild({
			render: () => {
				renderCount.value += 1;
				return ["hover target"];
			},
			invalidate: () => {},
			handleMouse: (event: any) => {
				if (event.type === "move") {
					events.push({ type: event.type, x: event.x, y: event.y });
					return { handled: true };
				}
				if (event.type === "press") {
					events.push({ type: event.type, x: event.x, y: event.y });
					return { handled: true };
				}
				if (event.type === "click") {
					events.push({ type: event.type, x: event.x, y: event.y, clickCount: event.clickCount ?? null });
					return { handled: true };
				}
				return undefined;
			},
		});
		tui.start();
		await terminal.waitForRender();
		const motion = { rendered: renderCount.value, writes: recorded.writes.length };
		virtual.sendInput("\x1b[<35;1;1M");
		await virtual.waitForRender();
		const afterMotion = { rendered: renderCount.value, writes: recorded.writes.length };
		for (let count = 0; count < 3; count++) {
			virtual.sendInput("\x1b[<0;1;1M");
			virtual.sendInput("\x1b[<0;1;1m");
		}
		await virtual.waitForRender();
		const counts = events.filter((event: any) => event.type === "click").map((event: any) => event.clickCount);
		virtual.sendInput("\x1b[<32;4;1M");
		virtual.sendInput("\x1b[<0;1;1M");
		virtual.sendInput("\x1b[<0;1;1m");
		await virtual.waitForRender();
		const afterMove = events.filter((event: any) => event.type === "click").map((event: any) => event.clickCount);
		tui.stop();

		scenarios.push({
			name: "motion-and-click-counts",
			dimensions: { columns: 20, rows: 2 },
			inputs: {
				motionReport: "\x1b[<35;1;1M",
				clickReports: ["\x1b[<0;1;1M", "\x1b[<0;1;1m"],
				repeat: 3,
				moveBetweenReports: ["\x1b[<32;4;1M", "\x1b[<0;1;1M", "\x1b[<0;1;1m"],
			},
			expected: { motion, afterMotion, events, counts, afterMove },
		});
	}

	// The regular screen owns the terminal's mouse behavior: it enables nothing,
	// and a mouse report never becomes typed input for the focused component.
	{
		const virtual = new VirtualTerminal(20, 3);
		const recorded = recording(virtual);
		const tui = new TuiMainScreen(recorded.terminal as any);
		const input = new Input();
		tui.addChild(input);
		// The regular screen routes keys to the focused component; focusing the
		// input makes "no mouse report becomes text" an observable value.
		tui.setFocus(input);
		tui.start();
		await virtual.waitForRender();
		const reports = "\x1b[<0;1;1M\x1b[<0;1;1m";
		virtual.sendInput(reports);
		virtual.sendInput("x");
		await virtual.waitForRender();
		const regular = {
			enabledMouse: recorded.writes.filter((data: string) => /\x1b\[\?100[0-6]h/.test(data)).length,
			value: input.getValue(),
			containsReport: input.getValue().includes("\x1b[<"),
		};
		tui.stop();

		scenarios.push({
			name: "regular-screen-keeps-terminal-mouse",
			dimensions: { columns: 20, rows: 3 },
			inputs: { reports, typed: "x" },
			expected: { regular },
		});
	}

	const sourceEndpoints = [
		"packages/tui/src/tui.ts:dispatchMouseEvent,retargetMouseEvent,Container.handleMouse",
		"packages/tui/src/layout.ts:getLayoutBoxesAt,renderLayoutFrame",
		"packages/tui/src/components/mouse-region.ts:MouseRegion.handleMouse",
		"packages/tui/src/components/box.ts:Box.handleMouse",
		"packages/tui/src/tui-alt-screen.ts:TuiAltScreen.handleMouseEvent,TuiAltScreen.dispatchMouseToLayout,TuiAltScreen.applyMouseDispatchResult,TuiAltScreen.getComponentClickCount,TuiAltScreen.parseWheelEvent",
		"packages/tui/src/tui-main-screen.ts:TuiMainScreen",
		"packages/tui/test/virtual-terminal.ts:VirtualTerminal.sendInput,VirtualTerminal.getViewport",
	];

	return { sourceEndpoints, scenarios };
}
