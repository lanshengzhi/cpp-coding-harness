import path from "node:path";
import { pathToFileURL } from "node:url";

export interface ComponentCaptureObservation {
	sourceEndpoints: string[];
	scenarios: Array<{
		name: string;
		dimensions: { columns: number; rows: number };
		inputs: Record<string, any>;
		expected: Record<string, any>;
	}>;
}

export async function captureComponents(piCheckout: string): Promise<ComponentCaptureObservation> {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { Text } = await import(source("src/components/text.ts"));
	const { Box } = await import(source("src/components/box.ts"));
	const { Spacer } = await import(source("src/components/spacer.ts"));
	const { TruncatedText } = await import(source("src/components/truncated-text.ts"));

	const sourceEndpoints = [
		"packages/tui/src/components/text.ts:Text,render,setText,setCustomBgFn,setPaddingX,invalidate",
		"packages/tui/src/components/box.ts:Box,addChild,removeChild,clear,setBgFn,setPaddingX,invalidate,handleMouse,render",
		"packages/tui/src/components/spacer.ts:Spacer,render,setLines,invalidate",
		"packages/tui/src/components/truncated-text.ts:TruncatedText,render,invalidate",
	];

	const scenarios: any[] = [];

	// 1. styled-text-mutation
	{
		const textInputs = {
			text: "\x1b[31mhello\x1b[0m world\n\x1b[32msecond line\x1b[0m\twith tab",
			paddingX: 1,
			paddingY: 1,
			changedText: "changed\nlines\x1b[34m with ansi\x1b[0m",
		};
		const text = new Text(textInputs.text, textInputs.paddingX, textInputs.paddingY);
		const first = [...text.render(12)];
		text.setText(textInputs.changedText);
		const changed = [...text.render(12)];
		scenarios.push({
			name: "styled-text-mutation",
			dimensions: { columns: 12, rows: 6 },
			inputs: textInputs,
			expected: { first, changed },
		});
	}

	// 2. text-render-cache-and-bg
	{
		const text = new Text("alpha beta gamma\titem", 1, 1);
		const initial = [...text.render(20)];
		const cached = [...text.render(20)];
		const narrow = [...text.render(10)];
		text.setCustomBgFn((s: string) => `\x1b[44m${s}\x1b[49m`);
		const withBg = [...text.render(20)];
		text.setPaddingX(2);
		const padded2 = [...text.render(20)];
		text.invalidate();
		const invalidated = [...text.render(20)];
		scenarios.push({
			name: "text-render-cache-and-bg",
			dimensions: { columns: 20, rows: 5 },
			inputs: { text: "alpha beta gamma\titem", paddingX: 1, paddingY: 1 },
			expected: { initial, cached, narrow, withBg, padded2, invalidated },
		});
	}

	// 3. box-rendering-and-cache
	{
		const box = new Box(1, 1);
		const child1 = new Text("first child", 0, 0);
		const child2 = new Text("second child", 0, 0);
		box.addChild(child1);
		box.addChild(child2);
		const initial = [...box.render(20)];
		const cached = [...box.render(20)];
		box.setBgFn((s: string) => `\x1b[42m${s}\x1b[49m`);
		const withBg = [...box.render(20)];
		box.setPaddingX(2);
		const padded2 = [...box.render(20)];
		box.removeChild(child2);
		const removedChild2 = [...box.render(20)];
		box.invalidate();
		const invalidated = [...box.render(20)];
		box.clear();
		const cleared = [...box.render(20)];
		scenarios.push({
			name: "box-rendering-and-cache",
			dimensions: { columns: 20, rows: 6 },
			inputs: { paddingX: 1, paddingY: 1, child1: "first child", child2: "second child" },
			expected: { initial, cached, withBg, padded2, removedChild2, invalidated, cleared },
		});
	}

	// 4. box-mouse-routing
	{
		class ClickableChild {
			height = 2;
			lastEvent: any = null;
			render(_w: number) {
				return ["row 0", "row 1"];
			}
			handleMouse(event: any) {
				this.lastEvent = event;
				return { handled: true };
			}
		}
		const box = new Box(2, 1);
		const child = new ClickableChild();
		box.addChild(child as any);
		// Pre-render so mouseLayout is populated at width 20 (content width = 16)
		box.render(20);

		// Inside child: x=3, y=2 -> contentX = 3-2 = 1, contentY = 2-1 = 1
		const eventInside = { type: "mouse" as const, action: "down" as const, button: "left" as const, x: 3, y: 2, width: 20, height: 4 };
		const resultInside = box.handleMouse(eventInside);

		// In left padding: x=1, y=2 -> contentX = 1-2 = -1 -> undefined
		const eventLeftPad = { type: "mouse" as const, action: "down" as const, button: "left" as const, x: 1, y: 2, width: 20, height: 4 };
		const resultLeftPad = box.handleMouse(eventLeftPad);

		// In top padding: x=3, y=0 -> contentY = 0-1 = -1 -> undefined
		const eventTopPad = { type: "mouse" as const, action: "down" as const, button: "left" as const, x: 3, y: 0, width: 20, height: 4 };
		const resultTopPad = box.handleMouse(eventTopPad);

		scenarios.push({
			name: "box-mouse-routing",
			dimensions: { columns: 20, rows: 4 },
			inputs: { paddingX: 2, paddingY: 1 },
			expected: {
				resultInsideHandled: resultInside?.handled ?? false,
				childReceivedCoords: { x: child.lastEvent?.x, y: child.lastEvent?.y },
				resultLeftPadHandled: resultLeftPad !== undefined,
				resultTopPadHandled: resultTopPad !== undefined,
			},
		});
	}

	// 5. spacer-rendering
	{
		const spacer = new Spacer(3);
		const render3 = [...spacer.render(15)];
		spacer.setLines(1);
		const render1 = [...spacer.render(15)];
		spacer.setLines(0);
		const render0 = [...spacer.render(15)];
		spacer.invalidate();
		scenarios.push({
			name: "spacer-rendering",
			dimensions: { columns: 15, rows: 5 },
			inputs: { lines: 3 },
			expected: { render3, render1, render0 },
		});
	}

	// 6. truncated-text-rendering
	{
		const trunc = new TruncatedText("first line text\nsecond line is ignored", 1, 1);
		const wide = [...trunc.render(25)];
		const narrow = [...trunc.render(12)];
		const ansiTrunc = new TruncatedText("\x1b[31mhello\x1b[0m world", 0, 0);
		const ansiRender = [...ansiTrunc.render(8)];
		const zeroWidth = [...ansiTrunc.render(0)];
		trunc.invalidate();
		scenarios.push({
			name: "truncated-text-rendering",
			dimensions: { columns: 25, rows: 3 },
			inputs: { text: "first line text\nsecond line is ignored", paddingX: 1, paddingY: 1 },
			expected: { wide, narrow, ansiRender, zeroWidth },
		});
	}

	return { sourceEndpoints, scenarios };
}
