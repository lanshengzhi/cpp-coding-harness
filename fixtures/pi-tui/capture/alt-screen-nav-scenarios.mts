import assert from "node:assert/strict";
import path from "node:path";
import { pathToFileURL } from "node:url";

/// Independent fullscreen prompt navigation, flash, and jump-to-end observations from frozen pi-v1.0.4.
export async function captureAltScreenNav(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { ScrollView } = await import(source("src/components/scroll-view.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { TuiAltScreen } = await import(source("src/tui-alt-screen.ts"));
	const { VirtualTerminal } = await import(source("test/virtual-terminal.ts"));

	const promptTerminal = new VirtualTerminal(20, 3);
	const promptTui = new TuiAltScreen(promptTerminal);
	promptTui.addChild(new Text(
		[1, 2, 3, 4].flatMap((message) => [`\x1b]133;A\x07message ${message}`, "detail"]).join("\n"),
		0,
		0,
	));
	promptTui.start();
	await promptTerminal.waitForRender();
	const promptNavigation: Array<{ action: string; viewportTop: number; screen: string[] }> = [];
	const recordPromptNavigation = (action: string) => {
		promptNavigation.push({
			action,
			viewportTop: promptTui.viewportTop,
			screen: promptTerminal.getViewport().map((line) => line.trimEnd()),
		});
	};
	recordPromptNavigation("initial");
	promptTerminal.sendInput("\x1b[57419;6u");
	promptTerminal.sendInput("\x1b[57419;6:3u");
	await promptTerminal.waitForRender();
	recordPromptNavigation("previous-press-and-release");
	promptTerminal.sendInput("\x1b[1;6A");
	await promptTerminal.waitForRender();
	recordPromptNavigation("previous");
	promptTerminal.sendInput("\x1b[1;6A");
	await promptTerminal.waitForRender();
	recordPromptNavigation("previous-again");
	promptTerminal.sendInput("\x1b[57420;6u");
	promptTerminal.sendInput("\x1b[57420;6:3u");
	await promptTerminal.waitForRender();
	recordPromptNavigation("next-press-and-release");
	promptTerminal.sendInput("\x1b[1;6B");
	await promptTerminal.waitForRender();
	recordPromptNavigation("next");
	promptTui.stop();

	const noPromptTerminal = new VirtualTerminal(20, 3);
	const noPromptTui = new TuiAltScreen(noPromptTerminal);
	noPromptTui.addChild(new Text(
		"\x1b]133;B\x07message output\n\x1b]133;C\x07assistant output\nplain three\nplain four\nplain five",
		0,
		0,
	));
	noPromptTui.start();
	await noPromptTerminal.waitForRender();
	const noPromptNavigation: Array<{ action: string; viewportTop: number; screen: string[] }> = [];
	const recordNoPromptNavigation = (action: string) => {
		noPromptNavigation.push({
			action,
			viewportTop: noPromptTui.viewportTop,
			screen: noPromptTerminal.getViewport().map((line) => line.trimEnd()),
		});
	};
	recordNoPromptNavigation("initial");
	noPromptTerminal.sendInput("\x1b[1;6A");
	await noPromptTerminal.waitForRender();
	recordNoPromptNavigation("previous-without-prompt");
	noPromptTerminal.sendInput("\x1b[1;6B");
	await noPromptTerminal.waitForRender();
	recordNoPromptNavigation("next-without-prompt");
	noPromptTui.stop();

	const terminal = new VirtualTerminal(20, 4);
	const tui = new TuiAltScreen(terminal);
	tui.addChild(new Text("one\ntwo\nthree\nfour", 0, 0));
	tui.start();
	await terminal.waitForRender();
	tui.flash("First", 5000);
	tui.flash("Second", 5000);
	await terminal.waitForRender();
	const flashScreen = terminal.getViewport().map((line) => line.trimEnd());
	assert.ok(flashScreen[0]?.endsWith("First"));
	assert.ok(flashScreen[1]?.endsWith("Second"));
	assert.ok(flashScreen[2]?.includes("three"));
	tui.stop();

	const overflowTerminal = new VirtualTerminal(20, 4);
	const overflowTui = new TuiAltScreen(overflowTerminal);
	overflowTui.addChild(new Text("one\ntwo\nthree\nfour", 0, 0));
	overflowTui.start();
	await overflowTerminal.waitForRender();
	const overflowFlashes = ["Flash 1", "Flash 2", "Flash 3", "Flash 4", "Flash 5", "Flash 6"];
	for (const message of overflowFlashes) overflowTui.flash(message, 5000);
	await overflowTerminal.waitForRender();
	const overflowScreen = overflowTerminal.getViewport().map((line) => line.trimEnd());
	assert.deepEqual(overflowScreen.map((line) => line.split("Flash")[1]?.trim()), ["3", "4", "5", "6"]);
	overflowTui.stop();

	const indicatorTerminal = new VirtualTerminal(30, 3);
	const indicatorTui = new TuiAltScreen(indicatorTerminal, undefined, undefined, {
		scrollToEndIndicator: () => " ↓ Jump to end ",
	});
	const followTranscript = new ScrollView(
		new Text("one\ntwo\nthree\nfour\nfive", 0, 0),
		{ follow: "end", primary: true },
	);
	indicatorTui.setLayoutRoot(followTranscript);
	indicatorTui.start();
	await indicatorTerminal.waitForRender();
	indicatorTerminal.sendInput("\x1b[<64;1;1M");
	await indicatorTerminal.waitForRender();
	const indicatorScreen = indicatorTerminal.getViewport().map((line) => line.trimEnd());
	const indicatorShown = indicatorScreen.some((line) => line.includes("Jump to end"));
	indicatorTerminal.sendInput("\x1b[<0;15;3M");
	indicatorTerminal.sendInput("\x1b[<0;15;3m");
	await indicatorTerminal.waitForRender();
	const clickFollowingEnd = followTranscript.isFollowingEnd;
	indicatorTui.stop();

	const noFollowTerminal = new VirtualTerminal(30, 3);
	const noFollowTui = new TuiAltScreen(noFollowTerminal, undefined, undefined, {
		scrollToEndIndicator: () => " ↓ Jump to end ",
	});
	noFollowTui.setLayoutRoot(new ScrollView(
		new Text("one\ntwo\nthree\nfour\nfive", 0, 0),
		{ primary: true },
	));
	noFollowTui.start();
	await noFollowTerminal.waitForRender();
	noFollowTerminal.sendInput("\x1b[<64;1;1M");
	await noFollowTerminal.waitForRender();
	const noFollowHasIndicator = noFollowTerminal.getViewport().some((line) => line.includes("Jump to end"));
	noFollowTui.stop();
	assert.ok(indicatorShown);
	assert.equal(clickFollowingEnd, true);
	assert.equal(noFollowHasIndicator, false);

	return {
		sourceEndpoints: [
			"packages/tui/src/components/alt-screen-flash.ts:AltScreenFlashContainer.flash,AltScreenFlashContainer.render",
			"packages/tui/src/tui-alt-screen.ts:TuiAltScreen.flash,TuiAltScreen.scrollToPrompt,TuiAltScreen.compositeFlashes,TuiAltScreen.compositeScrollToEndIndicator,TuiAltScreen.handleScrollToEndIndicatorMouseEvent,TuiAltScreen.doRender",
			"packages/tui/test/virtual-terminal.ts:VirtualTerminal.sendInput,VirtualTerminal.waitForRender,VirtualTerminal.getViewport",
		],
		scenarios: [
			{
				name: "prompt-navigation-uses-raw-osc-133-and-ignores-releases",
				dimensions: { columns: 20, rows: 3 },
				inputs: {
					transcript: [1, 2, 3, 4].flatMap((message) => [`\x1b]133;A\x07message ${message}`, "detail"]),
					keys: ["previous-press", "previous-release", "previous", "previous", "next-press", "next-release", "next"],
				},
				expected: { navigation: promptNavigation },
			},
			{
				name: "prompt-navigation-leaves-view-when-no-prompt-matches",
				dimensions: { columns: 20, rows: 3 },
				inputs: { transcript: ["\x1b]133;B\x07message output", "\x1b]133;C\x07assistant output", "plain three", "plain four", "plain five"], keys: ["previous", "next"] },
				expected: { navigation: noPromptNavigation },
			},
			{
				name: "flash-replaces-top-rows-oldest-first",
				dimensions: { columns: 20, rows: 4 },
				inputs: { transcript: ["one", "two", "three", "four"], flashes: ["First", "Second"] },
				expected: { screen: flashScreen },
			},
			{
				name: "flash-overflow-keeps-newest-rows-in-original-order",
				dimensions: { columns: 20, rows: 4 },
				inputs: { transcript: ["one", "two", "three", "four"], flashes: overflowFlashes },
				expected: { screen: overflowScreen },
			},
			{
				name: "indicator-shown-when-scrolled",
				dimensions: { columns: 30, rows: 3 },
				inputs: { transcript: ["one", "two", "three", "four", "five"], wheel: "\x1b[<64;1;1M" },
				expected: { indicator: "Jump to end", screen: indicatorScreen, clickFollowingEnd },
			},
			{
				name: "indicator-hidden-without-follow-end",
				dimensions: { columns: 30, rows: 3 },
				inputs: { transcript: ["one", "two", "three", "four", "five"], wheel: "\x1b[<64;1;1M", followEnd: false },
				expected: { indicatorShown: noFollowHasIndicator },
			},
		],
	};
}
