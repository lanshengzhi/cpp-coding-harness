import path from "node:path";
import { pathToFileURL } from "node:url";

export interface ColorCaptureObservation {
	sourceEndpoints: string[];
	scenarios: Array<{
		name: string;
		dimensions: { columns: number; rows: number };
		inputs: Record<string, any>;
		expected: Record<string, any>;
	}>;
}

export async function captureColorScenarios(piCheckout: string): Promise<ColorCaptureObservation> {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const {
		indexedColor,
		rgbColor,
		oklchColor,
		okhslColor,
		parseColor,
		colorToRgb,
		colorToOklch,
		colorToOkhsl,
		colorToHex,
		mixColors,
		foregroundAnsi,
		backgroundAnsi,
		styleText,
		styleTextWithAnsi,
	} = await import(source("src/colors.ts"));
	const { oklabToOkhslLightness } = await import(source("src/oklab.ts"));

	const sourceEndpoints = [
		"packages/tui/src/colors.ts:Color,ColorMixSpace,IndexedColor,OkhslChannels,OklchChannels,OklchColorValue,RgbColorValue,TerminalColorMode,TextAttributes,TextStyle,backgroundAnsi,colorToHex,colorToOkhsl,colorToOklch,colorToRgb,foregroundAnsi,indexedColor,mixColors,okhslColor,oklchColor,parseColor,rgbColor,styleText,styleTextWithAnsi",
		"packages/tui/src/oklab.ts:oklabToOkhslLightness",
	];

	// 1. color-values-and-construction
	const indexedInputs = [0, 9, 15, 16, 42, 231, 232, 255];
	const rgbInputs = [
		{ r: 0, g: 0, b: 0 },
		{ r: 255, g: 0, b: 0 },
		{ r: 18, g: 52, b: 86 },
		{ r: 12.5, g: 34.5, b: 56.5 },
		{ r: 255, g: 255, b: 255 },
	];
	const oklchInputs = [
		{ l: 0.627955, c: 0.257683, h: 29.2339 },
		{ l: 0.8, c: 0.15, h: 150 },
		{ l: 1.0, c: 0.3, h: 150 },
		{ l: 0.0, c: 0.3, h: 150 },
	];

	const scenarioValuesAndConstruction = {
		name: "color-values-and-construction",
		dimensions: { columns: 80, rows: 24 },
		inputs: {
			indexed: indexedInputs,
			rgb: rgbInputs,
			oklch: oklchInputs,
		},
		expected: {
			indexed: indexedInputs.map((idx) => indexedColor(idx)),
			rgb: rgbInputs.map((item) => rgbColor(item.r, item.g, item.b)),
			oklch: oklchInputs.map((item) => oklchColor(item.l, item.c, item.h)),
		},
	};

	// 2. color-parsing-and-hex
	const hexParseInputs = ["#abc", "#12aBcD", "#ff0000", "#0000ff", "#808080", "#ffffff", "#000000", "#aB3"];
	const indexParseInputs = [0, 42, 231, 232, 255];
	const oklchParseInputs = ["oklch(62% 0.1 200)", "oklch(80% 0.15 150deg)", "OKLCH(0.8 0.15 510)"];
	const okhslParseInputs = [
		"okhsl(29.23 100% 56.8%)",
		"OKHSL(250deg 60% 55%)",
		"okhsl(150 50% 80%)",
		"okhsl(150deg 0.5 0.8)",
	];

	const parseColorSafe = (val: string | number) => {
		const parsed = parseColor(val);
		return {
			input: val,
			parsed,
			hex: colorToHex(parsed),
		};
	};

	const scenarioParsingAndHex = {
		name: "color-parsing-and-hex",
		dimensions: { columns: 80, rows: 24 },
		inputs: {
			hex: hexParseInputs,
			indexed: indexParseInputs,
			oklch: oklchParseInputs,
			okhsl: okhslParseInputs,
		},
		expected: {
			hex: hexParseInputs.map(parseColorSafe),
			indexed: indexParseInputs.map(parseColorSafe),
			oklch: oklchParseInputs.map(parseColorSafe),
			okhsl: okhslParseInputs.map(parseColorSafe),
		},
	};

	// 3. okhsl-channels-and-conversion
	const okhslInputs = [
		{ h: 29.23, s: 1.0, l: 0.568 },
		{ h: 250, s: 0.6, l: 0.55 },
		{ h: 150, s: 0.5, l: 0.8 },
		{ h: 0, s: 0, l: 0.5 },
		{ h: 180, s: 1.0, l: 0.5 },
	];
	const colorsToOkhslInputs = ["#4f8eb3", "#20242a", "#f8f9fa", "#ff0000", "#00ff00", "#0000ff", 42];

	const scenarioOkhslChannels = {
		name: "okhsl-channels-and-conversion",
		dimensions: { columns: 80, rows: 24 },
		inputs: {
			okhslColors: okhslInputs,
			colorsToOkhsl: colorsToOkhslInputs,
		},
		expected: {
			okhslColors: okhslInputs.map((item) => {
				const rgb = okhslColor(item.h, item.s, item.l);
				const channels = colorToOkhsl(rgb);
				return {
					input: item,
					rgb,
					channels,
				};
			}),
			colorsToOkhsl: colorsToOkhslInputs.map((val) => ({
				input: val,
				channels: colorToOkhsl(parseColor(val)),
			})),
		},
	};

	// 4. color-conversion-and-gamut-mapping
	const toRgbIndexed = [0, 9, 16, 231, 232, 255];
	const toRgbRgb = [
		{ r: 18, g: 52, b: 86 },
		{ r: 255, g: 0, b: 0 },
	];
	const toRgbOklch = [
		{ l: 0.627955, c: 0.257683, h: 29.2339 },
		{ l: 1.0, c: 0.3, h: 150 },
		{ l: 0.0, c: 0.3, h: 150 },
	];

	const toOklchRgb = [
		{ r: 255, g: 0, b: 0 },
		{ r: 0, g: 255, b: 0 },
		{ r: 0, g: 0, b: 255 },
		{ r: 255, g: 255, b: 0 },
	];
	const toOklchIndexed = [9, 42];
	const toOklchOklch = [{ l: 0.8, c: 0.15, h: 150 }];

	const scenarioConversionAndGamut = {
		name: "color-conversion-and-gamut-mapping",
		dimensions: { columns: 80, rows: 24 },
		inputs: {
			toRgb: {
				indexed: toRgbIndexed,
				rgb: toRgbRgb,
				oklch: toRgbOklch,
			},
			toOklch: {
				rgb: toOklchRgb,
				indexed: toOklchIndexed,
				oklch: toOklchOklch,
			},
		},
		expected: {
			toRgb: {
				indexed: toRgbIndexed.map((idx) => ({ input: idx, rgb: colorToRgb(indexedColor(idx)) })),
				rgb: toRgbRgb.map((item) => ({ input: item, rgb: colorToRgb(rgbColor(item.r, item.g, item.b)) })),
				oklch: toRgbOklch.map((item) => ({
					input: item,
					rgb: colorToRgb(oklchColor(item.l, item.c, item.h)),
				})),
			},
			toOklch: {
				rgb: toOklchRgb.map((item) => ({ input: item, oklch: colorToOklch(rgbColor(item.r, item.g, item.b)) })),
				indexed: toOklchIndexed.map((idx) => ({ input: idx, oklch: colorToOklch(indexedColor(idx)) })),
				oklch: toOklchOklch.map((item) => ({
					input: item,
					oklch: colorToOklch(oklchColor(item.l, item.c, item.h)),
				})),
			},
		},
	};

	// 5. color-mixing-and-spaces
	const oklchMixInputs = [
		{ first: "#ff0000", second: "#0000ff", amount: 0.5 },
		{ first: { kind: "oklch", l: 0.7, c: 0.1, h: 350 }, second: { kind: "oklch", l: 0.7, c: 0.1, h: 10 }, amount: 0.5 },
		{ first: "#808080", second: { kind: "oklch", l: 0.7, c: 0.12, h: 217 }, amount: 0.5 },
		{ first: { kind: "oklch", l: 0.7, c: 0.12, h: 217 }, second: "#808080", amount: 0.5 },
	];
	const srgbMixInputs = [
		{ first: "#ff0000", second: "#0000ff", amount: 0.5 },
		{ first: "#000000", second: "#ffffff", amount: 0.25 },
	];

	const toColorObj = (val: any) => {
		if (typeof val === "string" || typeof val === "number") return parseColor(val);
		if (val.kind === "oklch") return oklchColor(val.l, val.c, val.h);
		if (val.kind === "rgb") return rgbColor(val.r, val.g, val.b);
		return indexedColor(val.index);
	};

	const scenarioMixingAndSpaces = {
		name: "color-mixing-and-spaces",
		dimensions: { columns: 80, rows: 24 },
		inputs: {
			oklch: oklchMixInputs,
			srgb: srgbMixInputs,
		},
		expected: {
			oklch: oklchMixInputs.map((item) => {
				const mixed = mixColors(toColorObj(item.first), toColorObj(item.second), item.amount, "oklch");
				return {
					first: item.first,
					second: item.second,
					amount: item.amount,
					mixed,
					hex: colorToHex(mixed),
				};
			}),
			srgb: srgbMixInputs.map((item) => {
				const mixed = mixColors(toColorObj(item.first), toColorObj(item.second), item.amount, "srgb");
				return {
					first: item.first,
					second: item.second,
					amount: item.amount,
					mixed,
					hex: colorToHex(mixed),
				};
			}),
		},
	};

	// 6. oklab-to-okhsl-lightness
	const lightnessInputs = [0.0, 0.1, 0.2, 0.4, 0.5, 0.6279553639214309, 0.75, 0.9, 1.0];
	const scenarioLightness = {
		name: "oklab-to-okhsl-lightness",
		dimensions: { columns: 80, rows: 24 },
		inputs: {
			values: lightnessInputs,
		},
		expected: {
			values: lightnessInputs.map((val) => ({
				input: val,
				lightness: oklabToOkhslLightness(val),
			})),
		},
	};

	// 7. text-style-and-ansi-formatting
	const foregroundInputs = [
		{ color: "#ff0000", mode: "truecolor" as const },
		{ color: "#ff0000", mode: "256color" as const },
		{ color: 9, mode: "truecolor" as const },
		{ color: 9, mode: "256color" as const },
		{ color: { kind: "oklch", l: 0.627955, c: 0.257683, h: 29.2339 }, mode: "truecolor" as const },
		{ color: { kind: "oklch", l: 0.627955, c: 0.257683, h: 29.2339 }, mode: "256color" as const },
	];
	const backgroundInputs = [
		{ color: "#0000ff", mode: "truecolor" as const },
		{ color: "#0000ff", mode: "256color" as const },
		{ color: 42, mode: "truecolor" as const },
		{ color: 42, mode: "256color" as const },
	];

	const styleTextInputs = [
		{
			text: "Ready",
			style: {
				fg: rgbColor(18, 52, 86),
				bg: indexedColor(9),
				bold: true,
				italic: true,
			},
			mode: "truecolor" as const,
		},
		{
			text: "Ready",
			style: {
				fg: rgbColor(18, 52, 86),
			},
			mode: "256color" as const,
		},
		{
			text: "FullAttrs",
			style: {
				fg: rgbColor(18, 52, 86),
				bg: indexedColor(9),
				bold: true,
				dim: true,
				italic: true,
				underline: true,
				inverse: true,
				strikethrough: true,
			},
			mode: "truecolor" as const,
		},
		{
			text: "AttrsOnly",
			style: {
				bold: true,
				dim: true,
				underline: true,
			},
			mode: "256color" as const,
		},
		{
			text: "NoAttrs",
			style: {},
			mode: "truecolor" as const,
		},
	];

	const styleTextWithAnsiInputs = [
		{
			text: "Cached",
			fgAnsi: "\x1b[38;2;18;52;86m",
			bgAnsi: "\x1b[48;5;9m",
			options: { bold: true, italic: true },
		},
		{
			text: "Plain",
			fgAnsi: undefined,
			bgAnsi: undefined,
			options: { underline: true },
		},
	];

	const scenarioTextStyle = {
		name: "text-style-and-ansi-formatting",
		dimensions: { columns: 80, rows: 24 },
		inputs: {
			foregroundAnsi: foregroundInputs,
			backgroundAnsi: backgroundInputs,
			styleText: styleTextInputs,
			styleTextWithAnsi: styleTextWithAnsiInputs,
		},
		expected: {
			foregroundAnsi: foregroundInputs.map((item) => ({
				input: item,
				ansi: foregroundAnsi(toColorObj(item.color), item.mode),
			})),
			backgroundAnsi: backgroundInputs.map((item) => ({
				input: item,
				ansi: backgroundAnsi(toColorObj(item.color), item.mode),
			})),
			styleText: styleTextInputs.map((item) => ({
				input: item,
				output: styleText(item.text, item.style, item.mode),
			})),
			styleTextWithAnsi: styleTextWithAnsiInputs.map((item) => ({
				input: item,
				output: styleTextWithAnsi(item.text, item.fgAnsi, item.bgAnsi, item.options),
			})),
		},
	};

	return {
		sourceEndpoints,
		scenarios: [
			scenarioValuesAndConstruction,
			scenarioParsingAndHex,
			scenarioOkhslChannels,
			scenarioConversionAndGamut,
			scenarioMixingAndSpaces,
			scenarioLightness,
			scenarioTextStyle,
		],
	};
}
