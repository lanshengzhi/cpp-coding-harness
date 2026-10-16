import { pathToFileURL } from "node:url";
import path from "node:path";
import assert from "node:assert/strict";

/// Independent pi-v1.0.4 observation of fullscreen image placements (#1012).
///
/// Every value below comes from pi's own modules: a real `TuiAltScreen` over pi's
/// `VirtualTerminal`, its own `cropKittyImageLine`/`getKittyImagePlacement`
/// stages, and the frozen `prepareKittyScreen` cache and eviction caps. The
/// write stream is recorded and reduced to the image commands it carries and
/// the screen row each command executes at, so the C++ replay can compare
/// protocol bytes rather than a summary. The source is one 16x64 PNG with four
/// distinguishable 16px bands (red, green, blue, yellow), so a wrong source
/// rectangle selects a visibly wrong band instead of merely a different height.
export async function captureAltScreenImage(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { ScrollView } = await import(source("src/components/scroll-view.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { VStack } = await import(source("src/components/v-stack.ts"));
	const { Image } = await import(source("src/components/image.ts"));
	const { encodeKitty, registerKittyImageMetadata, setCapabilities } = await import(
		source("src/terminal-image.ts")
	);
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	/// The banded 16x64 PNG used by every scenario: bands of 16 rows.
	const PNG =
		"iVBORw0KGgoAAAANSUhEUgAAABAAAABACAIAAACcH2DBAAAALklEQVR42u3TsQkAAAwCQfdf2iyQJm04+PYawTQ5FQB4DMwEAA4BAFs9BgCPwQCIUPs91s6WBgAAAABJRU5ErkJggg==";

	const KITTY = { images: "kitty", trueColor: true, hyperlinks: false } as const;
	const ITERM2 = { images: "iterm2", trueColor: true, hyperlinks: false } as const;

	/// Record every write so the reduced image-command trace is the protocol
	/// bytes pi produced, with the screen row each command ran at.
	const recordWrites = (terminal: any) => {
		const writes: string[] = [];
		const original = terminal.write.bind(terminal);
		terminal.write = (data: string) => {
			writes.push(data);
			original(data);
		};
		return writes;
	};

	/// Every kitty/OSC-1337 command in write order. A row is null when no cursor
	/// position preceded the command inside its write chunk, which is where the
	/// placement cleanup (`d=a`/`d=A`/`d=I`) lands.
	const imageCommands = (writes: readonly string[]) => {
		const trace: Array<{ row: number | null; command: string }> = [];
		for (const chunk of writes) {
			let row: number | null = null;
			let index = 0;
			while (index < chunk.length) {
				const escape = chunk.indexOf("\x1b", index);
				if (escape === -1) break;
				const cursor = /^\x1b\[(\d+);(\d+)H/.exec(chunk.slice(escape));
				if (cursor) {
					row = Number(cursor[1]) - 1;
					index = escape + cursor[0].length;
					continue;
				}
				if (chunk.startsWith("\x1b_G", escape)) {
					const end = chunk.indexOf("\x1b\\", escape + 3);
					if (end === -1) break;
					trace.push({ row, command: chunk.slice(escape, end + 2) });
					index = end + 2;
					continue;
				}
				if (chunk.startsWith("\x1b]1337;File=", escape)) {
					const end = chunk.indexOf("\x07", escape);
					if (end === -1) break;
					trace.push({ row, command: chunk.slice(escape, end + 1) });
					index = end + 1;
					continue;
				}
				index = escape + 1;
			}
		}
		return trace;
	};

	const scenarios: any[] = [];

	// A placement partially scrolled out of a followed viewport: the frozen
	// scroll-top crop paints only the band below the window top and re-places it
	// with `a=p` while the scroll position moves back and forth.
	{
		setCapabilities(KITTY);
		const terminal = new VirtualTerminal(12, 3);
		const writes = recordWrites(terminal);
		const tui = new TuiAltScreen(terminal);
		const imageId = 7001;
		registerKittyImageMetadata({ imageId, columns: 4, rows: 4, widthPx: 16, heightPx: 64 });
		const imageLine = encodeKitty(PNG, { columns: 4, rows: 4, imageId, moveCursor: false });
		const transcript = new ScrollView(
			{ render: () => ["alpha", "beta", imageLine, "", "", "", "gamma", "delta"], invalidate: () => {} },
			{ follow: "end", primary: true },
		);
		tui.setLayoutRoot(transcript);
		tui.start();
		await terminal.waitForRender();

		let cursor = 0;
		const phases: any[] = [];
		const observe = (action: string) => {
			const commands = imageCommands(writes.slice(cursor));
			cursor = writes.length;
			phases.push({
				action,
				scrollTop: transcript.scrollTop,
				viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
				images: commands,
			});
		};
		observe("initial");
		tui.scrollBy(-1);
		await terminal.waitForRender();
		observe("scroll-up");
		tui.scrollBy(1);
		await terminal.waitForRender();
		observe("scroll-back");
		tui.stop();

		scenarios.push({
			name: "image-scroll-crop-round-trip",
			dimensions: { columns: 12, rows: 3 },
			inputs: {
				capabilities: "kitty",
				image: { id: imageId, columns: 4, rows: 4, widthPx: 16, heightPx: 64, mimeType: "image/png", data: PNG },
				content: ["alpha", "beta", null, "", "", "", "gamma", "delta"],
				follow: "end",
				steps: [{ action: "render" }, { action: "scrollBy", lines: -1 }, { action: "scrollBy", lines: 1 }],
			},
			expected: { phases },
		});
	}

	// The same resource placed at a new anchor row: the frozen cache reuses the
	// upload with a placement-only command instead of transmitting again.
	{
		setCapabilities(KITTY);
		const terminal = new VirtualTerminal(10, 8);
		const writes = recordWrites(terminal);
		const tui = new TuiAltScreen(terminal);
		const imageId = 7002;
		registerKittyImageMetadata({ imageId, columns: 4, rows: 4, widthPx: 16, heightPx: 64 });
		const imageLine = encodeKitty(PNG, { columns: 4, rows: 4, imageId, moveCursor: false });
		const header = new Text("head", 0, 0);
		const root = new VStack([
			{ component: header },
			{ component: { render: () => [imageLine, "", "", ""], invalidate: () => {} } },
			{ component: new Text("tail", 0, 0) },
		]);
		tui.setLayoutRoot(root);
		tui.start();
		await terminal.waitForRender();

		let cursor = 0;
		const phases: any[] = [];
		const observe = (action: string) => {
			const commands = imageCommands(writes.slice(cursor));
			cursor = writes.length;
			phases.push({
				action,
				viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
				images: commands,
			});
		};
		observe("initial");
		header.setText("head\ngrown");
		tui.requestRender();
		await terminal.waitForRender();
		observe("header-grows");
		tui.stop();
		assert.equal(phases[0].images.length, 2, "transmission plus the full-redraw clear");
		assert.equal(phases[1].images.length, 2, "placement-only reuse plus the image redraw clear");

		scenarios.push({
			name: "image-anchor-move-reuse",
			dimensions: { columns: 10, rows: 8 },
			inputs: {
				capabilities: "kitty",
				image: { id: imageId, columns: 4, rows: 4, widthPx: 16, heightPx: 64, mimeType: "image/png", data: PNG },
				content: { header: "head", grownHeader: "head\ngrown", tail: "tail" },
				steps: [{ action: "render" }, { action: "growHeader" }],
			},
			expected: { phases },
		});
	}

	// Removing the placement rewrites the rows it covered, and leaving the
	// session deletes every uploaded image.
	{
		setCapabilities(KITTY);
		const terminal = new VirtualTerminal(10, 7);
		const writes = recordWrites(terminal);
		const tui = new TuiAltScreen(terminal);
		const imageId = 7003;
		registerKittyImageMetadata({ imageId, columns: 4, rows: 4, widthPx: 16, heightPx: 64 });
		const imageLine = encodeKitty(PNG, { columns: 4, rows: 4, imageId, moveCursor: false });
		const root = new VStack([{ component: new Text("head", 0, 0) }]);
		const image = { render: () => [imageLine, "", "", ""], invalidate: () => {} };
		root.addChild(image);
		root.addChild(new Text("tail", 0, 0));
		tui.setLayoutRoot(root);
		tui.start();
		await terminal.waitForRender();

		let cursor = 0;
		const phases: any[] = [];
		const observe = (action: string) => {
			const commands = imageCommands(writes.slice(cursor));
			cursor = writes.length;
			phases.push({
				action,
				viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
				images: commands,
			});
		};
		observe("initial");
		root.removeChild(image);
		tui.requestRender();
		await terminal.waitForRender();
		observe("image-removed");
		tui.stop();
		observe("stopped");
		assert.ok(
			phases[1].images.some((entry: any) => entry.command.includes("d=a")),
			"removal clears placements",
		);
		assert.ok(
			phases[2].images.some((entry: any) => entry.command.includes("d=A")),
			"session teardown deletes every image",
		);

		scenarios.push({
			name: "image-removal-session-teardown",
			dimensions: { columns: 10, rows: 7 },
			inputs: {
				capabilities: "kitty",
				image: { id: imageId, columns: 4, rows: 4, widthPx: 16, heightPx: 64, mimeType: "image/png", data: PNG },
				content: { head: "head", tail: "tail" },
				steps: [{ action: "render" }, { action: "removeImage" }, { action: "stop" }],
			},
			expected: { phases },
		});
	}

	// The frozen fullscreen iTerm2 fallback: the session nulls the image
	// capability, so the real component renders its text fallback and no image
	// sequence is written for the whole session.
	{
		setCapabilities(ITERM2);
		const terminal = new VirtualTerminal(24, 6);
		const writes = recordWrites(terminal);
		const tui = new TuiAltScreen(terminal);
		const image = new Image(PNG, "image/png", { fallbackColor: (text: string) => text }, {}, {
			widthPx: 16,
			heightPx: 64,
		});
		tui.setLayoutRoot(image);
		tui.start();
		await terminal.waitForRender();

		let cursor = 0;
		const phases: any[] = [];
		const observe = (action: string) => {
			const commands = imageCommands(writes.slice(cursor));
			cursor = writes.length;
			phases.push({
				action,
				viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
				images: commands,
			});
		};
		observe("initial");
		tui.stop();
		observe("stopped");
		assert.equal(phases[0].images.length, 0, "no image sequence in a fullscreen iTerm2 session");

		scenarios.push({
			name: "iterm2-fullscreen-fallback",
			dimensions: { columns: 24, rows: 6 },
			inputs: {
				capabilities: "iterm2",
				image: { columns: 0, rows: 0, widthPx: 16, heightPx: 64, mimeType: "image/png", data: PNG },
				steps: [{ action: "render" }, { action: "stop" }],
			},
			expected: { phases },
		});
	}

	// A single offscreen upload whose decoded size exceeds the frozen 64 MiB cap
	// is deleted by image id on the first redraw after it leaves the viewport.
	{
		setCapabilities(KITTY);
		const terminal = new VirtualTerminal(10, 4);
		const writes = recordWrites(terminal);
		const tui = new TuiAltScreen(terminal);
		const imageId = 7200;
		registerKittyImageMetadata({ imageId, columns: 4, rows: 1, widthPx: 20000, heightPx: 20000 });
		const imageLine = encodeKitty(PNG, { columns: 4, rows: 1, imageId, moveCursor: false });
		const transcript = new ScrollView(
			{ render: () => [imageLine, "one", "two", "three", "four", "five"], invalidate: () => {} },
			{ primary: true },
		);
		tui.setLayoutRoot(transcript);
		tui.start();
		await terminal.waitForRender();

		let cursor = 0;
		const phases: any[] = [];
		const observe = (action: string) => {
			const commands = imageCommands(writes.slice(cursor));
			cursor = writes.length;
			phases.push({
				action,
				scrollTop: transcript.scrollTop,
				viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
				images: commands,
			});
		};
		observe("initial");
		tui.scrollBy(1);
		await terminal.waitForRender();
		observe("scrolled-off");
		tui.stop();
		assert.ok(
			phases[1].images.some((entry: any) => entry.command === `\x1b_Ga=d,d=I,i=${imageId},q=2\x1b\\`),
			"decoded-size cap evicts by image id",
		);

		scenarios.push({
			name: "image-decoded-bytes-eviction",
			dimensions: { columns: 10, rows: 4 },
			inputs: {
				capabilities: "kitty",
				image: {
					id: imageId,
					columns: 4,
					rows: 1,
					widthPx: 20000,
					heightPx: 20000,
					mimeType: "image/png",
					data: PNG,
				},
				content: [null, "one", "two", "three", "four", "five"],
				steps: [{ action: "render" }, { action: "scrollBy", lines: 1 }],
			},
			expected: { phases },
		});
	}

	// Twenty one-row placements walked past the viewport: the frozen 16-entry
	// offscreen cap deletes the oldest entries by image id in LRU order.
	{
		setCapabilities(KITTY);
		const terminal = new VirtualTerminal(8, 3);
		const writes = recordWrites(terminal);
		const tui = new TuiAltScreen(terminal);
		const imageIds: number[] = [];
		const lines: string[] = [];
		for (let index = 0; index < 20; index++) {
			const imageId = 7100 + index;
			imageIds.push(imageId);
			registerKittyImageMetadata({ imageId, columns: 2, rows: 1, widthPx: 16, heightPx: 16 });
			lines.push(encodeKitty(PNG, { columns: 2, rows: 1, imageId, moveCursor: false }));
		}
		const transcript = new ScrollView({ render: () => lines, invalidate: () => {} }, { primary: true });
		tui.setLayoutRoot(transcript);
		tui.start();
		await terminal.waitForRender();

		let cursor = 0;
		const phases: any[] = [];
		const observe = (action: string) => {
			const commands = imageCommands(writes.slice(cursor));
			cursor = writes.length;
			phases.push({
				action,
				scrollTop: transcript.scrollTop,
				viewport: terminal.getViewport().map((line: string) => line.trimEnd()),
				images: commands,
			});
		};
		observe("initial");
		for (let step = 0; step < 17; step++) {
			tui.scrollBy(1);
			await terminal.waitForRender();
			observe(`scroll-down-${step + 1}`);
		}
		for (let step = 0; step < 17; step++) {
			tui.scrollBy(-1);
			await terminal.waitForRender();
			observe(`scroll-up-${step + 1}`);
		}
		tui.stop();
		const evictions = phases.flatMap((phase: any) =>
			phase.images.filter((entry: any) => entry.command.includes("d=I")).map((entry: any) => entry.command),
		);
		assert.ok(evictions.length >= 2, "the offscreen cap evicts entries in both directions");
		assert.equal(new Set(evictions).size, evictions.length, "each eviction names a distinct image id");

		scenarios.push({
			name: "image-offscreen-count-eviction",
			dimensions: { columns: 8, rows: 3 },
			inputs: {
				capabilities: "kitty",
				image: { columns: 2, rows: 1, widthPx: 16, heightPx: 16, mimeType: "image/png", data: PNG },
				content: { imageCount: 20, imageIds },
				steps: [
					{ action: "render" },
					...Array.from({ length: 17 }, () => ({ action: "scrollBy", lines: 1 })),
					...Array.from({ length: 17 }, () => ({ action: "scrollBy", lines: -1 })),
				],
			},
			expected: { phases },
		});
	}

	return {
		sourceEndpoints: [
			"packages/tui/src/tui-alt-screen.ts:doRender,beforeTerminalStart,beforeTerminalStop,scrollBy,prepareKittyScreen",
			"packages/tui/src/terminal-image.ts:cropKittyImageLine,getKittyImagePlacement,registerKittyImageMetadata,encodeKitty",
			"packages/tui/src/layout.ts:paintBox",
			"packages/tui/src/components/image.ts:render",
		],
		scenarios,
	};
}
