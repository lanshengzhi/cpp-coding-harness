import path from "node:path";
import { pathToFileURL } from "node:url";

export interface UtilsWidthCaptureObservation {
	sourceEndpoints: string[];
	scenarios: Array<{
		name: string;
		dimensions: { columns: number; rows: number };
		inputs: Record<string, any>;
		expected: Record<string, any>;
	}>;
}

export async function captureUtilsWidth(piCheckout: string): Promise<UtilsWidthCaptureObservation> {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const {
		visibleWidth,
		wrapTextWithAnsi,
		truncateToWidth,
		sliceByColumn,
		getOsc8LinkAtColumn,
		stripTerminalSequences,
	} = await import(source("src/utils.ts"));
	const { Text } = await import(source("src/components/text.ts"));
	const { Input } = await import(source("src/components/input.ts"));

	const sourceEndpoints = [
		"packages/tui/src/utils.ts:visibleWidth,wrapTextWithAnsi,truncateToWidth,sliceByColumn,getOsc8LinkAtColumn,stripTerminalSequences",
		"packages/tui/src/components/text.ts:Text,render",
		"packages/tui/src/components/input.ts:Input,render,setValue",
	];

	const scenarios: any[] = [];

	// 1. visible-width-and-narrow-placement
	{
		const narrowText = new Text("काक्ष", 0, 0);
		const narrowInput = new Input();
		narrowInput.setValue("काक्ष");

		const visibleWidthCases = [
			{ name: "ka", input: "का", output: visibleWidth("का") },
			{ name: "ksha", input: "क्ष", output: visibleWidth("क्ष") },
			{ name: "ka-newline-ksha", input: "का\nक्ष", output: visibleWidth("का\nक्ष") },
			{ name: "hello-newline-world", input: "hello\nworld", output: visibleWidth("hello\nworld") },
			{
				name: "ansi-newline-ansi",
				input: "\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m",
				output: visibleWidth("\x1b[31mred\x1b[0m\n\x1b[32mtail\x1b[0m"),
			},
			{ name: "emoji-combining-flag", input: "a😀b", output: visibleWidth("a😀b") },
		];

		scenarios.push({
			name: "visible-width-and-narrow-placement",
			dimensions: { columns: 6, rows: 4 },
			inputs: {
				visibleWidth: visibleWidthCases,
				narrowText: { text: "काक्ष", width: 2 },
				narrowInput: { value: "काक्ष", width: 6 },
			},
			expected: {
				visibleWidth: visibleWidthCases,
				narrowText: { width2: [...narrowText.render(2)], width4: [...narrowText.render(4)] },
				narrowInput: { width6: [...narrowInput.render(6)] },
			},
		});
	}

	// 2. ansi-boundary-order-and-hyperlinks
	{
		const belLink = (url: string) => `\x1b]8;;${url}\x07`;
		const stLink = (url: string) => `\x1b]8;;${url}\x1b\\`;
		const wrapCases = [
			{ name: "staged-control-after-content", input: "中文\x1b[31mABCDEFGHIJ", width: 4 },
			{ name: "staged-underline-after-content", input: "中文\x1b[4mABCDEFGH", width: 4 },
			{ name: "staged-hyperlink-after-content", input: `中文${belLink("u")}ABCDEFGH`, width: 4 },
			{ name: "st-terminated-link-across-break", input: `${stLink("u")}hello world${stLink("")}`, width: 7 },
			{ name: "link-cjk-styled-span", input: `ab ${belLink("u")}\x1b[31m中文\x1b[0m${belLink("")} cd`, width: 4 },
			{ name: "control-before-logical-newline", input: "abc\x1b[31m\ndef", width: 20 },
		];
		const truncateCases = [
			{ name: "fits-open-underline-pads-inside", input: "\x1b[4mabc", width: 8, ellipsis: "", pad: true },
			{ name: "pending-control-styles-dropped-text", input: "\x1b[4ma\x1b[31mbcdef", width: 4, ellipsis: "...", pad: false },
			{ name: "pending-link-styles-dropped-text", input: `\x1b[4ma${belLink("u")}bcdef`, width: 4, ellipsis: "...", pad: false },
			{ name: "st-terminated-link-closes-before-reset", input: `${stLink("u")}abcdefgh`, width: 4, ellipsis: "...", pad: false },
			{ name: "ellipsis-does-not-fit", input: "abcdef", width: 2, ellipsis: "中", pad: false },
			{ name: "ellipsis-clipped-to-fit", input: "abcdef", width: 2, ellipsis: "abc", pad: false },
			{ name: "ellipsis-clipped-away", input: "abcdef", width: 1, ellipsis: "中中", pad: false },
		];
		const sliceCases = [
			{ name: "slice-start-code-order", input: "\x1b[32mfoo\x1b[39m bar", start: 3, length: 4, strict: true },
			{ name: "slice-inside-open-link", input: `${belLink("u")}abcdefgh${belLink("")}`, start: 2, length: 3, strict: false },
			{ name: "slice-spanning-cjk-link", input: `${belLink("u")}a你bcd${belLink("")}`, start: 1, length: 3, strict: false },
		];
		const linkColumns = [
			{ name: "cjk-link-first-cell", input: `a ${belLink("https://x")}你b${belLink("")} c`, column: 2 },
			{ name: "cjk-link-second-cell", input: `a ${belLink("https://x")}你b${belLink("")} c`, column: 3 },
			{ name: "styled-link-cell", input: `\x1b[31m${belLink("https://x")}ab${belLink("")}\x1b[0m`, column: 0 },
			{ name: "cell-after-link-close", input: `${belLink("https://x")}ab${belLink("")}`, column: 3 },
		];
		const stripCases = [
			{ name: "plain", input: "hello world" },
			{ name: "sgr-colors", input: "\x1b[31mred\x1b[0m text \x1b[1mbold\x1b[22m" },
			{ name: "osc8-link", input: `prefix ${belLink("https://example.com")}link text${belLink("")} suffix` },
			{ name: "osc-title", input: "\x1b]0;terminal title\x07visible text" },
		];

		scenarios.push({
			name: "ansi-boundary-order-and-hyperlinks",
			dimensions: { columns: 20, rows: 4 },
			inputs: { wrap: wrapCases, truncate: truncateCases, slice: sliceCases, linkColumns, strip: stripCases },
			expected: {
				wrap: wrapCases.map((entry) => ({ ...entry, output: wrapTextWithAnsi(entry.input, entry.width) })),
				truncate: truncateCases.map((entry) => ({
					...entry, output: truncateToWidth(entry.input, entry.width, entry.ellipsis, entry.pad),
				})),
				slice: sliceCases.map((entry) => ({
					...entry, output: sliceByColumn(entry.input, entry.start, entry.length, entry.strict),
				})),
				linkColumns: linkColumns.map((entry) => ({
					...entry, link: getOsc8LinkAtColumn(entry.input, entry.column) ?? null,
				})),
				strip: stripCases.map((entry) => ({
					...entry, output: stripTerminalSequences(entry.input),
				})),
			},
		});
	}

	return { sourceEndpoints, scenarios };
}
