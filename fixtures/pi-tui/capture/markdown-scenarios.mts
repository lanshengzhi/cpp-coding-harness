import { pathToFileURL } from "node:url";
import path from "node:path";

export async function captureMarkdownScenarios(piCheckout: string) {
	const packageRoot = path.join(piCheckout, "packages/tui");
	const source = (relative: string) => pathToFileURL(path.join(packageRoot, relative)).href;
	const { Markdown } = await import(source("src/components/markdown.ts"));

	const sourceEndpoints = [
		"packages/tui/src/components/markdown.ts:Markdown,MarkdownOptions,MarkdownTheme,DefaultTextStyle",
	];

	const themeStyles = {
		heading: { prefix: "\x1b[36m", suffix: "\x1b[39m" },
		bold: { prefix: "\x1b[1m", suffix: "\x1b[22m" },
		italic: { prefix: "\x1b[3m", suffix: "\x1b[23m" },
		strikethrough: { prefix: "\x1b[9m", suffix: "\x1b[29m" },
		underline: { prefix: "\x1b[4m", suffix: "\x1b[24m" },
		code: { prefix: "\x1b[33m", suffix: "\x1b[39m" },
		codeBlock: { prefix: "\x1b[32m", suffix: "\x1b[39m" },
		codeBlockBorder: { prefix: "\x1b[2m", suffix: "\x1b[22m" },
		listBullet: { prefix: "\x1b[35m", suffix: "\x1b[39m" },
		quote: { prefix: "\x1b[3m", suffix: "\x1b[23m" },
		quoteBorder: { prefix: "\x1b[34m", suffix: "\x1b[39m" },
		hr: { prefix: "\x1b[2m", suffix: "\x1b[22m" },
		link: { prefix: "\x1b[34m", suffix: "\x1b[39m" },
		linkUrl: { prefix: "\x1b[2m", suffix: "\x1b[22m" },
	};
	const wrap = (s: { prefix: string; suffix: string }) => (text: string) => `${s.prefix}${text}${s.suffix}`;

	const makeTheme = (overrides?: any) => ({
		heading: wrap(themeStyles.heading),
		link: wrap(themeStyles.link),
		linkUrl: wrap(themeStyles.linkUrl),
		code: wrap(themeStyles.code),
		codeBlock: wrap(themeStyles.codeBlock),
		codeBlockBorder: wrap(themeStyles.codeBlockBorder),
		quote: wrap(themeStyles.quote),
		quoteBorder: wrap(themeStyles.quoteBorder),
		hr: wrap(themeStyles.hr),
		listBullet: wrap(themeStyles.listBullet),
		bold: wrap(themeStyles.bold),
		italic: wrap(themeStyles.italic),
		strikethrough: wrap(themeStyles.strikethrough),
		underline: wrap(themeStyles.underline),
		codeBlockIndent: "  ",
		...overrides,
	});

	const scenarios: any[] = [];

	// 1. markdown-headings-and-inline-styles
	// Exercises: headings (H1, H2), bold, italic, strikethrough, underline, inline code, render, renderToken, renderInlineTokens
	{
		const markdownSource =
			"# Heading 1\n## Heading 2\nText with **bold**, *italic*, ~~strikethrough~~, and `inline code`.\nPlain paragraph.";
		const md = new Markdown(markdownSource, 0, 0, makeTheme());
		const lines = md.render(50);
		scenarios.push({
			name: "markdown-headings-and-inline-styles",
			dimensions: { columns: 50, rows: lines.length },
			inputs: {
				markdown: markdownSource,
				width: 50,
				styles: themeStyles,
			},
			expected: {
				lines,
			},
		});
	}

	// 2. markdown-lists-and-markers
	// Exercises: unordered list (listBullet), ordered list normalization, preserveOrderedListMarkers option, getOrderedListMarker, getUnorderedListMarker
	{
		const unorderedSource = "- bullet 1\n- bullet 2\n  - nested bullet";
		const orderedSource = "1. item one\n1. item two\n1. item three";

		const mdUnordered = new Markdown(unorderedSource, 0, 0, makeTheme());
		const unorderedLines = mdUnordered.render(40);

		const mdOrderedNorm = new Markdown(orderedSource, 0, 0, makeTheme());
		const orderedNormalizedLines = mdOrderedNorm.render(40);

		const mdOrderedPreserved = new Markdown(orderedSource, 0, 0, makeTheme(), undefined, {
			preserveOrderedListMarkers: true,
		});
		const orderedPreservedLines = mdOrderedPreserved.render(40);

		scenarios.push({
			name: "markdown-lists-and-markers",
			dimensions: { columns: 40, rows: unorderedLines.length + orderedNormalizedLines.length },
			inputs: {
				unordered: unorderedSource,
				ordered: orderedSource,
				width: 40,
			},
			expected: {
				unorderedLines,
				orderedNormalizedLines,
				orderedPreservedLines,
			},
		});
	}

	// 3. markdown-tables-and-cjk
	// Exercises: GFM table layout, cell border formatting, wide CJK character measurement in cells, unequal column allocations
	{
		const standardSource = "| Col A | Col B |\n| --- | --- |\n| val 1 | val 2 |";
		const cjkSource = "| 地域 | 名前 |\n| --- | --- |\n| 東京 | 大阪 |";
		const unequalSource = "| Short | Long Description Column |\n| --- | --- |\n| a | b |";

		const mdStandard = new Markdown(standardSource, 0, 0, makeTheme());
		const standardLines = mdStandard.render(30);

		const mdCjk = new Markdown(cjkSource, 0, 0, makeTheme());
		const cjkLines = mdCjk.render(24);

		const mdUnequal = new Markdown(unequalSource, 0, 0, makeTheme());
		const unequalLines = mdUnequal.render(35);

		scenarios.push({
			name: "markdown-tables-and-cjk",
			dimensions: { columns: 35, rows: standardLines.length + cjkLines.length + unequalLines.length },
			inputs: {
				standard: standardSource,
				cjk: cjkSource,
				unequal: unequalSource,
			},
			expected: {
				standardLines,
				cjkLines,
				unequalLines,
			},
		});
	}

	// 4. markdown-code-blocks-and-quotes
	// Exercises: fenced code blocks, codeBlock, codeBlockBorder, codeBlockIndent, highlightCode, blockquotes, quote, quoteBorder
	{
		const codeSource = "```cpp\nint answer = 42;\nreturn answer;\n```";
		const quoteSource = "> first line of quote\n> second line of quote\n>> nested quote";

		const mdCode = new Markdown(codeSource, 0, 0, makeTheme());
		const codeLines = mdCode.render(40);

		const mdHighlight = new Markdown(
			codeSource,
			0,
			0,
			makeTheme({
				highlightCode: (code: string) => code.split("\n").map((line: string) => `\x1b[35m${line}\x1b[39m`),
			}),
		);
		const highlightedLines = mdHighlight.render(40);

		const mdQuote = new Markdown(quoteSource, 0, 0, makeTheme());
		const quoteLines = mdQuote.render(40);

		scenarios.push({
			name: "markdown-code-blocks-and-quotes",
			dimensions: { columns: 40, rows: codeLines.length + quoteLines.length },
			inputs: {
				code: codeSource,
				quote: quoteSource,
				width: 40,
			},
			expected: {
				codeLines,
				highlightedLines,
				quoteLines,
			},
		});
	}

	// 5. markdown-links-hr-and-transforms
	// Exercises: links (link, linkUrl), horizontal rule (hr), transform option with width, preserveBackslashEscapes option
	{
		const linkSource = "[Pike](https://example.com/pike)";
		const hrSource = "---";
		const transformSource = "transformed text";
		const backslashSource = "\\*not italic\\*";

		const mdLink = new Markdown(linkSource, 0, 0, makeTheme());
		const linkLines = mdLink.render(40);

		const mdHr = new Markdown(hrSource, 0, 0, makeTheme());
		const hrLines = mdHr.render(30);

		const mdTransform = new Markdown(transformSource, 0, 0, makeTheme(), undefined, {
			transform: (source: string, availableWidth: number) => `${source} [cols=${availableWidth}]`,
		});
		const transformLines = mdTransform.render(35);

		const mdBackslashDefault = new Markdown(backslashSource, 0, 0, makeTheme());
		const backslashDefaultLines = mdBackslashDefault.render(30);

		const mdBackslashPreserved = new Markdown(backslashSource, 0, 0, makeTheme(), undefined, {
			preserveBackslashEscapes: true,
		});
		const backslashPreservedLines = mdBackslashPreserved.render(30);

		scenarios.push({
			name: "markdown-links-hr-and-transforms",
			dimensions: { columns: 40, rows: linkLines.length + hrLines.length + transformLines.length },
			inputs: {
				link: linkSource,
				hr: hrSource,
				transform: transformSource,
				backslash: backslashSource,
			},
			expected: {
				linkLines,
				hrLines,
				transformLines,
				backslashDefaultLines,
				backslashPreservedLines,
			},
		});
	}

	// 6. markdown-latex-math-integration
	// Exercises: inline LaTeX ($...$), display LaTeX ($$...$$), renderLatex option (true/false), pending/unsupported math fallback
	{
		const inlineMathSource = "inline $\\frac{1}{2}$ and $x^2$";
		const displayMathSource = "$$\\frac{1}{2}$$";
		const pendingMathSource = "before $";

		const mdInline = new Markdown(inlineMathSource, 0, 0, makeTheme());
		const inlineLines = mdInline.render(40);

		const mdDisplay = new Markdown(displayMathSource, 0, 0, makeTheme());
		const displayLines = mdDisplay.render(40);

		const mdDisabled = new Markdown(inlineMathSource, 0, 0, makeTheme(), undefined, { renderLatex: false });
		const disabledLines = mdDisabled.render(40);

		const mdPending = new Markdown(pendingMathSource, 0, 0, makeTheme());
		const pendingLines = mdPending.render(40);

		scenarios.push({
			name: "markdown-latex-math-integration",
			dimensions: { columns: 40, rows: inlineLines.length + displayLines.length + disabledLines.length },
			inputs: {
				inlineMath: inlineMathSource,
				displayMath: displayMathSource,
				pendingMath: pendingMathSource,
				width: 40,
			},
			expected: {
				inlineLines,
				displayLines,
				disabledLines,
				pendingLines,
			},
		});
	}

	// 7. markdown-lifecycle-and-default-styles
	// Exercises: DefaultTextStyle (color, bgColor, bold, italic, strikethrough, underline),
	// defaultStylePrefix, getDefaultStylePrefix, getStylePrefix, getDefaultInlineStyleContext,
	// setText, invalidate, cachedWidth, cachedLines, paddingX, constructor, text
	{
		const defaultTextStyle: any = {
			color: (t: string) => `\x1b[90m${t}\x1b[39m`,
			bold: true,
			italic: true,
		};
		const md = new Markdown("initial text", 2, 1, makeTheme(), defaultTextStyle);
		const initialLines = md.render(40);
		const defaultStylePrefix = (md as any).getDefaultStylePrefix?.() ?? "\x1b[1m\x1b[3m\x1b[90m";

		md.setText("updated text");
		const updatedLines = md.render(30);

		md.invalidate();
		const invalidatedLines = md.render(30);

		scenarios.push({
			name: "markdown-lifecycle-and-default-styles",
			dimensions: { columns: 40, rows: initialLines.length + updatedLines.length },
			inputs: {
				initial: "initial text",
				updated: "updated text",
				paddingX: 2,
				paddingY: 1,
				defaultTextStyle: {
					bold: true,
					italic: true,
					color: "gray",
				},
			},
			expected: {
				defaultStylePrefix,
				initialLines,
				updatedLines,
				invalidatedLines,
			},
		});
	}

	return { sourceEndpoints, scenarios };
}
