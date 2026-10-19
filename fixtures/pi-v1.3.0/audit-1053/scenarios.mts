/**
 * Scenarios for the `audit-1053` codemode capture. Each function drives frozen pi v1.3.0 through a
 * public seam and returns a scenario record. `capture-1053.mts` writes the artifact envelope and the
 * manifest entry from the same record, so the committed bytes and the pinned scenario list cannot
 * drift apart.
 *
 * Two kinds of record are used deliberately:
 * - `exactScenario` asserts a hand-written expected observation (a known contract value).
 * - `goldenScenario` asserts structural/contract checks and then records the exact frozen output as
 *   the expected value. The observation is the comparison unit for a future C++ scenario; the
 *   checks make a broken capture fail instead of accepting an empty or partial string.
 */

import { deepStrictEqual } from "node:assert/strict";
import { execFile } from "node:child_process";
import { join } from "node:path";
import { promisify } from "node:util";
import {
    CODEMODE_OPTIONS_PREFIX,
    CODEMODE_SOURCE_GRAMMAR,
    CODEMODE_TOOL_NAME,
    CodemodeSandbox,
    CodemodeSourceError,
    DEFAULT_CODEMODE_INLINE_BUDGET,
    DEFAULT_INPUT_SCHEMA_MAX_CHARS,
    DEFAULT_INTERRUPT_BUDGET,
    InlineCodemodeSandbox,
    MAX_OUTPUT_CHARS,
    MAX_OUTPUT_ITEMS,
    MAX_STORE_TOTAL_CHARS,
    MAX_STORE_VALUE_CHARS,
    PRELUDE_SOURCE,
    RemoteCodemodeSandbox,
    codemodeToolSystemPromptContribution,
    createCodemodeDescription,
    createCodemodeExtension,
    getCodemodeCallableTools,
    loadQuickJSWasm,
    parseCodemodeSource,
    renderDeclarations,
    renderToolOutputType,
    renderToolSample,
    renderToolSignature,
    schemaToType,
    serveCodemodeRemote,
    toCodemodeDeclaration,
    toCodemodeIdentifier,
    type CodemodeResultShape,
} from "./pi.mts";

export interface Scenario {
    name: string;
    source: string;
    inputs: unknown;
    expected: unknown;
    observed: unknown;
    normalizations: string[];
    upstreamTest?: { path: string; case: string };
}

const execFileAsync = promisify(execFile);

/** JSON representation of an observed value: the artifact stores JSON, so exact equality is JSON equality. */
export function canonical(value: unknown): unknown {
    return JSON.parse(JSON.stringify(value));
}

function errorRecord(error: unknown): Record<string, unknown> {
    if (!(error instanceof Error)) return { name: "NonError", message: String(error) };
    return { name: error.name, message: error.message };
}

/** Replace host-specific absolute paths that a contract message embeds, keeping the contract suffix. */
function normalizePaths(value: unknown): unknown {
    return JSON.parse(
        JSON.stringify(value).replace(/\/[^"\\]*\/docs\/codemode\.md/g, "<docs>/codemode.md"),
    );
}

export async function exactScenario(
    name: string,
    source: string,
    inputs: unknown,
    expected: unknown,
    run: () => Promise<unknown> | unknown,
    options: { normalizations?: string[]; upstreamTest?: { path: string; case: string } } = {},
): Promise<Scenario> {
    const observed = canonical(await run());
    const expectedJson = canonical(expected);
    try {
        deepStrictEqual(observed, expectedJson);
    } catch (error) {
        throw new Error(
            `${name}: observed value differs from the expected observation\n` +
                `observed=${JSON.stringify(observed)}\nexpected=${JSON.stringify(expectedJson)}\n${String(error)}`,
        );
    }
    return {
        name,
        source,
        inputs,
        expected: expectedJson,
        observed,
        normalizations: options.normalizations ?? [],
        ...(options.upstreamTest ? { upstreamTest: options.upstreamTest } : {}),
    };
}

export async function goldenScenario(
    name: string,
    source: string,
    inputs: unknown,
    run: () => Promise<unknown> | unknown,
    check: (observed: any) => void,
    options: { normalizations?: string[]; upstreamTest?: { path: string; case: string } } = {},
): Promise<Scenario> {
    const observed = normalizePaths(canonical(await run()));
    check(observed);
    return {
        name,
        source,
        inputs,
        expected: observed,
        observed,
        normalizations: options.normalizations ?? [],
        ...(options.upstreamTest ? { upstreamTest: options.upstreamTest } : {}),
    };
}

function assert(condition: unknown, message: string): asserts condition {
    if (!condition) throw new Error(message);
}

const SOURCE_OPTIONS = "packages/codemode/src/source.ts:parseCodemodeSource,CodemodeSourceError,CODEMODE_OPTIONS_PREFIX,CODEMODE_SOURCE_GRAMMAR";

/** Options-parsing observations: public source format, validation and placement. */
export async function sourceScenarios(): Promise<Scenario[]> {
    const invalidInputs = [
        "",
        "  \n",
        "// @options:\ntext(1)",
        "// @options: [1]\ntext(1)",
        '// @options: {"yield": 1}\ntext(1)',
        '// @options: {"max_output_tokens": 1.5}\ntext(1)',
        '// @options: {"timeout_ms": 0}\ntext(1)',
        '// @options: {"timeout_ms": 2147483648}\ntext(1)',
        '// @options: {"timeout_ms": 1}',
        '// @options: {"timeout_ms": 1}\n  \n',
    ];
    const invalidExpected: Array<[string, string]> = [
        ["CodemodeSourceError", 'Expected JavaScript source text (non-empty). Provide JS only, optionally with a first line `// @options: {"max_output_tokens": 1000}`.'],
        ["CodemodeSourceError", 'Expected JavaScript source text (non-empty). Provide JS only, optionally with a first line `// @options: {"max_output_tokens": 1000}`.'],
        ["CodemodeSourceError", "@options must be a JSON object with supported fields `max_output_tokens` and `timeout_ms`"],
        ["CodemodeSourceError", "@options must be a JSON object with supported fields `max_output_tokens` and `timeout_ms`"],
        ["CodemodeSourceError", "@options only supports `max_output_tokens` and `timeout_ms`; got `yield`"],
        ["CodemodeSourceError", "@options field `max_output_tokens` must be a non-negative safe integer"],
        ["CodemodeSourceError", "@options field `timeout_ms` must be a positive integer up to 2147483647"],
        ["CodemodeSourceError", "@options field `timeout_ms` must be a positive integer up to 2147483647"],
        ["CodemodeSourceError", "The @options line must be followed by JavaScript source on subsequent lines"],
        ["CodemodeSourceError", "The @options line must be followed by JavaScript source on subsequent lines"],
    ];

    return [
        await exactScenario(
            "source keeps plain JavaScript and a leading ordinary comment unchanged",
            SOURCE_OPTIONS,
            { inputs: ["text('hi')", "// just a comment\nreturn 1"] },
            [
                { code: "text('hi')", options: {} },
                { code: "// just a comment\nreturn 1", options: {} },
            ],
            () => ["text('hi')", "// just a comment\nreturn 1"].map((input) => parseCodemodeSource(input)),
            { upstreamTest: { path: "packages/codemode/test/source.test.ts", case: "returns plain code unchanged" } },
        ),
        await exactScenario(
            "first-line options are parsed and replaced by an empty line to preserve line numbers",
            SOURCE_OPTIONS,
            { input: '// @options: {"timeout_ms": 10}\nconst a = 1;\ntext(a)' },
            { code: "\nconst a = 1;\ntext(a)", options: { timeoutMs: 10 } },
            () => parseCodemodeSource('// @options: {"timeout_ms": 10}\nconst a = 1;\ntext(a)'),
            { upstreamTest: { path: "packages/codemode/test/source.test.ts", case: "parses the options line and keeps line numbers" } },
        ),
        await exactScenario(
            "options accept leading whitespace, CRLF, an empty object and a zero token budget",
            SOURCE_OPTIONS,
            {
                inputs: [
                    '  // @options:{"max_output_tokens":0,"timeout_ms":1500}\r\ntext(1)',
                    "// @options: {}\ntext(1)",
                ],
            },
            [
                { code: "\ntext(1)", options: { maxOutputTokens: 0, timeoutMs: 1500 } },
                { code: "\ntext(1)", options: {} },
            ],
            () => [
                parseCodemodeSource('  // @options:{"max_output_tokens":0,"timeout_ms":1500}\r\ntext(1)'),
                parseCodemodeSource("// @options: {}\ntext(1)"),
            ],
            { upstreamTest: { path: "packages/codemode/test/source.test.ts", case: "parses the options line and keeps line numbers" } },
        ),
        await exactScenario(
            "only the first line is an options line",
            SOURCE_OPTIONS,
            { inputs: ["// @optionsx {}\ntext(1)", 'text("// @options: {}")'] },
            [
                { code: "// @optionsx {}\ntext(1)", options: {} },
                { code: 'text("// @options: {}")', options: {} },
            ],
            () => [parseCodemodeSource("// @optionsx {}\ntext(1)"), parseCodemodeSource('text("// @options: {}")')],
            { upstreamTest: { path: "packages/codemode/test/source.test.ts", case: "only treats the first line as an options line" } },
        ),
        await exactScenario(
            "a misplaced options line is rejected with its line number",
            SOURCE_OPTIONS,
            {
                inputs: [
                    '// count things\n// @options: {"timeout_ms": 1000}\nwhile (true) {}',
                    '\n  // @options: {"timeout_ms": 1000}\nwhile (true) {}',
                    'text(1)\r\n// @options: {"timeout_ms": 1}',
                    '// @options: {"timeout_ms": 1}\ntext(1)\n// @options: {"timeout_ms": 2}',
                ],
            },
            [2, 2, 2, 3].map((line) => ({
                name: "CodemodeSourceError",
                message: `The // @options: line must be the first line of the input; found one on line ${line}`,
            })),
            () =>
                [
                    '// count things\n// @options: {"timeout_ms": 1000}\nwhile (true) {}',
                    '\n  // @options: {"timeout_ms": 1000}\nwhile (true) {}',
                    'text(1)\r\n// @options: {"timeout_ms": 1}',
                    '// @options: {"timeout_ms": 1}\ntext(1)\n// @options: {"timeout_ms": 2}',
                ].map((input) => {
                    try {
                        parseCodemodeSource(input);
                        return { name: "NoError", message: "" };
                    } catch (error) {
                        return errorRecord(error);
                    }
                }),
            { upstreamTest: { path: "packages/codemode/test/source.test.ts", case: "rejects an options line that is not the first line" } },
        ),
        await goldenScenario(
            "invalid options and empty input are rejected with typed errors",
            SOURCE_OPTIONS,
            { inputs: invalidInputs },
            () =>
                invalidInputs.map((input) => {
                    try {
                        return { input, result: parseCodemodeSource(input) };
                    } catch (error) {
                        return { input, error: errorRecord(error) };
                    }
                }),
            (observed) => {
                assert(observed.length === invalidExpected.length, "every invalid input must be observed");
                observed.forEach((entry: any, index: number) => {
                    assert(entry.error, `input ${JSON.stringify(entry.input)} must be rejected`);
                    assert(entry.error.name === invalidExpected[index]![0], `error name for ${JSON.stringify(entry.input)}`);
                    assert(entry.error.message === invalidExpected[index]![1], `error message for ${JSON.stringify(entry.input)}`);
                    assert(entry.result === undefined, `input ${JSON.stringify(entry.input)} must not parse`);
                });
            },
            { upstreamTest: { path: "packages/codemode/test/source.test.ts", case: "rejects empty input and invalid options" } },
        ),
        await exactScenario(
            "the public options prefix and grammar constants are fixed",
            SOURCE_OPTIONS,
            { exports: ["CODEMODE_OPTIONS_PREFIX", "CODEMODE_SOURCE_GRAMMAR"] },
            {
                prefix: "// @options:",
                grammar:
                    "\nstart: options_source | plain_source\noptions_source: OPTIONS_LINE NEWLINE SOURCE\nplain_source: SOURCE\n\nOPTIONS_LINE: /[ \\t]*\\/\\/ @options:[^\\r\\n]*/\nNEWLINE: /\\r?\\n/\nSOURCE: /[\\s\\S]+/\n",
            },
            () => ({ prefix: CODEMODE_OPTIONS_PREFIX, grammar: CODEMODE_SOURCE_GRAMMAR }),
            { upstreamTest: { path: "packages/codemode/src/source.ts", case: "CODEMODE_SOURCE_GRAMMAR" } },
        ),
    ];
}

const DECLARATIONS = "packages/codemode/src/declarations.ts:schemaToType,renderDeclarations,renderToolSignature,renderToolSample,renderToolOutputType,toCodemodeIdentifier";

/** Declaration rendering: identifier mapping, schema constraints, output types and catalogs. */
export async function declarationScenarios(): Promise<Scenario[]> {
    return [
        await exactScenario(
            "tool identifiers replace invalid characters and preserve MCP names",
            DECLARATIONS,
            { names: ["mcp__docs__search", "my-tool", "9lives", "", "a b", "$ok", "_", "naïve"] },
            ["mcp__docs__search", "my_tool", "_lives", "_", "a_b", "$ok", "_", "na_ve"],
            () => ["mcp__docs__search", "my-tool", "9lives", "", "a b", "$ok", "_", "naïve"].map(toCodemodeIdentifier),
            { upstreamTest: { path: "packages/codemode/src/identifier.ts", case: "toCodemodeIdentifier" } },
        ),
        await exactScenario(
            "schema primitives, literals, unions and references render as TypeScript types",
            DECLARATIONS,
            {
                schemas: [
                    { type: "string" },
                    { type: "integer" },
                    { type: ["string", "null"] },
                    { const: "a" },
                    { enum: ["a", 1, null] },
                    { anyOf: [{ type: "string" }, { type: "number" }] },
                    { anyOf: [{ type: "string" }, {}] },
                    { allOf: [{ anyOf: [{ type: "string" }, { type: "number" }] }, { const: 1 }] },
                    { $ref: "#/defs/x" },
                    true,
                    false,
                    { type: "array", items: { type: "string" } },
                    { type: "array", prefixItems: [{ type: "string" }, { type: "number" }] },
                    { type: "array" },
                ],
            },
            [
                "string",
                "number",
                "string | null",
                '"a"',
                '"a" | 1 | null',
                "string | number",
                "unknown",
                "(string | number) & 1",
                "unknown",
                "unknown",
                "never",
                "Array<string>",
                "[string, number]",
                "unknown[]",
            ],
            () =>
                [
                    { type: "string" },
                    { type: "integer" },
                    { type: ["string", "null"] },
                    { const: "a" },
                    { enum: ["a", 1, null] },
                    { anyOf: [{ type: "string" }, { type: "number" }] },
                    { anyOf: [{ type: "string" }, {}] },
                    { allOf: [{ anyOf: [{ type: "string" }, { type: "number" }] }, { const: 1 }] },
                    { $ref: "#/defs/x" },
                    true,
                    false,
                    { type: "array", items: { type: "string" } },
                    { type: "array", prefixItems: [{ type: "string" }, { type: "number" }] },
                    { type: "array" },
                ].map((schema) => schemaToType(schema as any)),
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "renders primitives, literals, and unions" } },
        ),
        await exactScenario(
            "object properties are sorted on one line with quoted non-identifier keys",
            DECLARATIONS,
            {
                schemas: [
                    {
                        type: "object",
                        properties: { city: { type: "string" }, "max-lines": { type: "number" } },
                        required: ["city"],
                        additionalProperties: false,
                    },
                    { type: "object", additionalProperties: { type: "number" } },
                    { type: "object" },
                    { type: "object", properties: {}, additionalProperties: false },
                ],
            },
            ['{ city: string; "max-lines"?: number; }', "{ [key: string]: number; }", "{ [key: string]: unknown; }", "{}"],
            () =>
                [
                    {
                        type: "object",
                        properties: { city: { type: "string" }, "max-lines": { type: "number" } },
                        required: ["city"],
                        additionalProperties: false,
                    },
                    { type: "object", additionalProperties: { type: "number" } },
                    { type: "object" },
                    { type: "object", properties: {}, additionalProperties: false },
                ].map((schema) => schemaToType(schema as any)),
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "renders objects on one line with sorted properties" } },
        ),
        await exactScenario(
            "property descriptions become comment lines",
            DECLARATIONS,
            {
                schema: {
                    type: "object",
                    properties: {
                        weather: {
                            type: "array",
                            description: "look up weather for a given list of locations",
                            items: { type: "object", properties: { location: { type: "string" } }, required: ["location"] },
                        },
                    },
                    required: ["weather"],
                },
            },
            "{\n  // look up weather for a given list of locations\n  weather: Array<{ location: string; }>;\n}",
            () =>
                schemaToType({
                    type: "object",
                    properties: {
                        weather: {
                            type: "array",
                            description: "look up weather for a given list of locations",
                            items: { type: "object", properties: { location: { type: "string" } }, required: ["location"] },
                        },
                    },
                    required: ["weather"],
                }),
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "puts property descriptions on comment lines" } },
        ),
        await exactScenario(
            "constraint tags render integer bounds, defaults, patterns and deprecated markers",
            DECLARATIONS,
            {
                schema: {
                    type: "object",
                    properties: {
                        timeoutMs: { type: "integer", minimum: 0, maximum: 30000, default: 0, description: "Wait in milliseconds." },
                        id: { type: "string", pattern: "^[a-z]+$", maxLength: 8 },
                        tags: { type: "array", items: { type: "string" }, maxItems: 5, uniqueItems: true, deprecated: true },
                        name: { type: "string" },
                    },
                    required: ["id"],
                },
            },
            [
                "{",
                "  // @maxLength 8 @pattern ^[a-z]+$",
                "  id: string;",
                "  name?: string;",
                "  // @maxItems 5 @uniqueItems @deprecated",
                "  tags?: Array<string>;",
                "  // Wait in milliseconds.",
                "  // @integer @minimum 0 @maximum 30000 @default 0",
                "  timeoutMs?: number;",
                "}",
            ].join("\n"),
            () =>
                schemaToType({
                    type: "object",
                    properties: {
                        timeoutMs: { type: "integer", minimum: 0, maximum: 30000, default: 0, description: "Wait in milliseconds." },
                        id: { type: "string", pattern: "^[a-z]+$", maxLength: 8 },
                        tags: { type: "array", items: { type: "string" }, maxItems: 5, uniqueItems: true, deprecated: true },
                        name: { type: "string" },
                    },
                    required: ["id"],
                }),
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "puts constraints the type cannot express on property comment lines" } },
        ),
        await exactScenario(
            "optional non-null branches keep their constraint tags",
            DECLARATIONS,
            {
                schema: {
                    type: "object",
                    properties: {
                        limit: { anyOf: [{ type: "integer", minimum: 1, maximum: 100 }, { type: "null" }], default: null, title: "Limit" },
                        n: { anyOf: [{ type: "integer", minimum: 10, maximum: 50, format: "int32" }, { type: "null" }], minimum: 0, maximum: 20, format: "port" },
                    },
                },
            },
            "{\n  // @integer @minimum 1 @maximum 100\n  limit?: number | null;\n  // @integer @minimum 10 @maximum 20 @format port @format int32\n  n?: number | null;\n}",
            () =>
                schemaToType({
                    type: "object",
                    properties: {
                        limit: { anyOf: [{ type: "integer", minimum: 1, maximum: 100 }, { type: "null" }], default: null, title: "Limit" },
                        n: { anyOf: [{ type: "integer", minimum: 10, maximum: 50, format: "int32" }, { type: "null" }], minimum: 0, maximum: 20, format: "port" },
                    },
                }),
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "reads constraints of optional fields from the non-null branch" } },
        ),
        await exactScenario(
            "draft-04 boolean exclusive bounds become exclusive tags",
            DECLARATIONS,
            {
                schema: {
                    type: "object",
                    properties: {
                        n: { type: "integer", minimum: 0, exclusiveMinimum: true, maximum: 10, exclusiveMaximum: false },
                        m: { anyOf: [{ type: "number", maximum: 5, exclusiveMaximum: true }, { type: "null" }], maximum: 8 },
                    },
                },
            },
            "{\n  // @maximum 8 @exclusiveMaximum 5\n  m?: number | null;\n  // @integer @exclusiveMinimum 0 @maximum 10\n  n?: number;\n}",
            () =>
                schemaToType({
                    type: "object",
                    properties: {
                        n: { type: "integer", minimum: 0, exclusiveMinimum: true, maximum: 10, exclusiveMaximum: false },
                        m: { anyOf: [{ type: "number", maximum: 5, exclusiveMaximum: true }, { type: "null" }], maximum: 8 },
                    },
                }),
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "reads draft-04 boolean exclusive bounds" } },
        ),
        await exactScenario(
            "long tags are dropped, line terminators are escaped, and line breaks stay in comments",
            DECLARATIONS,
            {
                schemas: [
                    { type: "object", properties: { config: { type: "object", default: { text: "x".repeat(100) } } } },
                    {
                        type: "object",
                        properties: {
                            sep: { type: "string", pattern: "^[\r\n\u2028\u2029]$", default: "\u2028", description: "a\u2028b" },
                        },
                    },
                ],
            },
            [
                "{ config?: { [key: string]: unknown; }; }",
                "{\n  // a\n  // b\n  // @pattern ^[\\u000d\\u000a\\u2028\\u2029]$ @default \"\\u2028\"\n  sep?: string;\n}",
            ],
            () =>
                [
                    { type: "object", properties: { config: { type: "object", default: { text: "x".repeat(100) } } } },
                    {
                        type: "object",
                        properties: {
                            sep: { type: "string", pattern: "^[\r\n\u2028\u2029]$", default: "\u2028", description: "a\u2028b" },
                        },
                    },
                ].map((schema) => schemaToType(schema as any)),
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "leaves out long tags and escapes line terminators" } },
        ),
        await exactScenario(
            "types over the character budget drop tags, then become unknown",
            DECLARATIONS,
            { schema: { type: "object", properties: { n: { type: "integer", minimum: 1 } } }, budgets: [null, 20, 10] },
            ["{\n  // @integer @minimum 1\n  n?: number;\n}", "{ n?: number; }", "unknown"],
            () => {
                const schema = { type: "object", properties: { n: { type: "integer", minimum: 1 } } };
                return [schemaToType(schema), schemaToType(schema, { maxChars: 20 }), schemaToType(schema, { maxChars: 10 })];
            },
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "drops constraint tags before rendering a type over the budget as unknown" } },
        ),
        await exactScenario(
            "local references resolve and recursive or remote references render unknown",
            DECLARATIONS,
            {
                schema: {
                    type: "object",
                    properties: {
                        item: { $ref: "#/$defs/Item" },
                        legacy: { $ref: "#/definitions/Legacy" },
                        remote: { $ref: "https://example.com/schema.json" },
                    },
                    required: ["item"],
                    $defs: {
                        Item: {
                            type: "object",
                            properties: { id: { type: "string" }, parent: { $ref: "#/$defs/Item" } },
                            required: ["id"],
                        },
                    },
                    definitions: { Legacy: { enum: ["a", "b"] } },
                },
            },
            '{ item: { id: string; parent?: unknown; }; legacy?: "a" | "b"; remote?: unknown; }',
            () =>
                schemaToType({
                    type: "object",
                    properties: {
                        item: { $ref: "#/$defs/Item" },
                        legacy: { $ref: "#/definitions/Legacy" },
                        remote: { $ref: "https://example.com/schema.json" },
                    },
                    required: ["item"],
                    $defs: {
                        Item: {
                            type: "object",
                            properties: { id: { type: "string" }, parent: { $ref: "#/$defs/Item" } },
                            required: ["id"],
                        },
                    },
                    definitions: { Legacy: { enum: ["a", "b"] } },
                }),
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "resolves local references and stops at recursive ones" } },
        ),
        await exactScenario(
            "tool signatures and samples use the output schema and input budget",
            DECLARATIONS,
            {
                tool: {
                    name: "mcp__docs__search",
                    description: " Search docs ",
                    inputSchema: { type: "object", properties: { query: { type: "string" } }, required: ["query"] },
                    outputSchema: { type: "object", properties: { results: { type: "array", items: { type: "string" } } }, required: ["results"] },
                },
            },
            {
                signature: "mcp__docs__search(args: { query: string; }): Promise<{ results: Array<string>; }>;",
                unknownOutput: "unknown",
                output: "{ [key: string]: unknown; }",
                sample: "Search docs\n\ncodemode tool declaration:\n```ts\ndeclare const tools: { mcp__docs__search(args: { query: string; }): Promise<{ results: Array<string>; }>; };\n```",
                budgeted: "mcp__docs__search(args: unknown): Promise<{ results: Array<string>; }>;",
            },
            () => {
                const tool = {
                    name: "mcp__docs__search",
                    description: " Search docs ",
                    inputSchema: { type: "object", properties: { query: { type: "string" } }, required: ["query"] },
                    outputSchema: { type: "object", properties: { results: { type: "array", items: { type: "string" } } }, required: ["results"] },
                };
                return {
                    signature: renderToolSignature(tool),
                    unknownOutput: renderToolOutputType(undefined),
                    output: renderToolOutputType({ type: "object", properties: {} }),
                    sample: renderToolSample(tool),
                    budgeted: renderToolSignature(tool, { inputMaxChars: 1 }),
                };
            },
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "renderToolSignature" } },
        ),
        await goldenScenario(
            "renderDeclarations groups tools, globals and namespaces with doc comments",
            DECLARATIONS,
            {
                tools: [
                    { name: "my-tool", description: "Do the thing", inputSchema: { type: "object", properties: { a: { type: "string" } } }, outputSchema: { type: "string" } },
                ],
                globals: [
                    { name: "searchTools", description: "Search tools", signature: "(query: string): Promise<string[]>" },
                    { name: "models.classify", description: "Classify", signature: "(input: string): Promise<string>" },
                    { name: "models.generateImages", signature: "(input: string): Promise<string[]>" },
                ],
            },
            () =>
                renderDeclarations({
                    tools: [
                        { name: "my-tool", description: "Do the thing", inputSchema: { type: "object", properties: { a: { type: "string" } } }, outputSchema: { type: "string" } },
                    ],
                    globals: [
                        { name: "searchTools", description: "Search tools", signature: "(query: string): Promise<string[]>" },
                        { name: "models.classify", description: "Classify", signature: "(input: string): Promise<string>" },
                        { name: "models.generateImages", signature: "(input: string): Promise<string[]>" },
                    ],
                }),
            (observed) => {
                assert(observed.startsWith("declare const tools: {\n"), "tools declaration must start the output");
                assert(observed.includes("/** Do the thing */\n  my_tool(args: { a?: string; }): Promise<string>;"), "tool doc comment and signature");
                assert(observed.includes("declare function searchTools"), "top-level global declaration");
                assert(observed.includes("declare const models: {"), "dotted globals group into one namespace");
                assert(observed.includes("classify(input: string): Promise<string>;"), "classify declaration");
                assert(observed.includes("generateImages(input: string): Promise<string[]>;"), "generateImages declaration");
            },
            { upstreamTest: { path: "packages/codemode/test/declarations.test.ts", case: "renderDeclarations" } },
        ),
        await exactScenario(
            "default and stored limits are public constants",
            DECLARATIONS,
            { exports: ["DEFAULT_INPUT_SCHEMA_MAX_CHARS", "MAX_OUTPUT_CHARS", "MAX_OUTPUT_ITEMS", "MAX_STORE_TOTAL_CHARS", "MAX_STORE_VALUE_CHARS", "DEFAULT_INTERRUPT_BUDGET"] },
            {
                inputSchemaMaxChars: 16000,
                outputChars: 16777216,
                outputItems: 100000,
                storeTotalChars: 1048576,
                storeValueChars: 262144,
                interruptBudget: 100000,
            },
            () => ({
                inputSchemaMaxChars: DEFAULT_INPUT_SCHEMA_MAX_CHARS,
                outputChars: MAX_OUTPUT_CHARS,
                outputItems: MAX_OUTPUT_ITEMS,
                storeTotalChars: MAX_STORE_TOTAL_CHARS,
                storeValueChars: MAX_STORE_VALUE_CHARS,
                interruptBudget: DEFAULT_INTERRUPT_BUDGET,
            }),
            { upstreamTest: { path: "packages/codemode/src/runtime/prelude-source.ts", case: "MAX_OUTPUT_CHARS,MAX_OUTPUT_ITEMS,MAX_STORE_TOTAL_CHARS,MAX_STORE_VALUE_CHARS" } },
        ),
    ];
}

const WASM = await loadQuickJSWasm();
const EXECUTION = "packages/codemode/src/runtime/execution.ts:CodemodeSandboxBase.execute,CodemodeSandboxBase.registerTool,CodemodeSandboxBase.start,CodemodeSandboxBase.close";

/** Remove only noncontractual call durations, keeping presence and status. */
function normalizeCalls(result: CodemodeResultShape): CodemodeResultShape {
    const normalized = canonical(result) as CodemodeResultShape & { calls: Array<Record<string, unknown>> };
    for (const call of normalized.calls) {
        call.durationPresent = typeof call.durationMs === "number";
        delete call.durationMs;
    }
    return normalized;
}

async function withSandbox<T>(
    options: Record<string, unknown>,
    run: (sandbox: any) => Promise<T>,
): Promise<T> {
    const sandbox = new CodemodeSandbox({ wasm: WASM, ...options });
    try {
        return await run(sandbox);
    } finally {
        await sandbox.close();
    }
}

const PNG_1X1 =
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mP8z8BQDwAFgwJ/lYQ5WQAAAABJRU5ErkJggg==";

/** Script execution: return/output/error channels, tool routing, globals, store and limits. */
export async function executionScenarios(): Promise<Scenario[]> {
    const TOOL_SOURCE = "packages/codemode/src/runtime/execution.ts:CodemodeSandboxBase.execute,Execution.handleCall";
    return [
        await exactScenario(
            "a script returns a JSON value after top-level await and collects output in order",
            TOOL_SOURCE,
            { script: "text('plain'); console.log('log', 2); console.error('err'); image(dataUrl); return { a: 1 }" },
            {
                ok: true,
                value: { a: 1 },
                output: [
                    { type: "text", text: "plain" },
                    { type: "text", text: "log 2", console: true },
                    { type: "text", text: "err", console: true },
                    { type: "image", data: PNG_1X1, mimeType: "image/png" },
                ],
                calls: [],
                storeWrites: { set: {}, delete: [] },
            },
            async () => {
                const result = await withSandbox({}, (sandbox) =>
                    sandbox.execute(`text("plain"); console.log("log", 2); console.error("err"); image("data:image/png;base64,${PNG_1X1}"); return {a: 1}`),
                );
                return normalizeCalls(result);
            },
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "collects text(), image(), and console output in order" } },
        ),
        await exactScenario(
            "text values are stringified and image accepts data URLs and image blocks",
            TOOL_SOURCE,
            {
                script:
                    "text(['a', 1]); text({x: true}); text(undefined); image({type:'image', data: b64, mimeType:'image/png'}); return 1",
            },
            {
                ok: true,
                value: 1,
                output: [
                    { type: "text", text: '["a",1]' },
                    { type: "text", text: '{"x":true}' },
                    { type: "text", text: "undefined" },
                    { type: "image", data: PNG_1X1, mimeType: "image/png" },
                ],
                calls: [],
                storeWrites: { set: {}, delete: [] },
            },
            async () => {
                const result = await withSandbox({}, (sandbox) =>
                    sandbox.execute(
                        `text(["a", 1]); text({x: true}); text(undefined); image({type:"image", data:"${PNG_1X1}", mimeType:"image/png"}); return 1`,
                    ),
                );
                return normalizeCalls(result);
            },
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "collects text(), image(), and console output in order" } },
        ),
        await exactScenario(
            "invalid image arguments throw a catchable TypeError inside the script",
            TOOL_SOURCE,
            { script: "try { image(42); } catch (error) { return [error.name, error.message]; }" },
            {
                ok: true,
                value: [
                    "TypeError",
                    'image expects a non-empty image URL string, an object with image_url, or an image block { type: "image", data, mimeType }',
                ],
                output: [],
                calls: [],
                storeWrites: { set: {}, delete: [] },
            },
            async () => normalizeCalls(await withSandbox({}, (sandbox) => sandbox.execute('try { image(42); } catch (error) { return [error.name, error.message]; }'))),
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "rejects invalid text() and image() arguments" } },
        ),
        await exactScenario(
            "exit() succeeds with prior output and store writes and stops later output",
            TOOL_SOURCE,
            { script: 'text("before"); store("a", {x: 1}); store("b", 2); store("b", undefined); exit(); text("after")' },
            {
                ok: true,
                output: [{ type: "text", text: "before" }],
                calls: [],
                storeWrites: { set: { a: { x: 1 } }, delete: ["b"] },
            },
            async () => normalizeCalls(await withSandbox({}, (sandbox) => sandbox.execute('text("before"); store("a", {x: 1}); store("b", 2); store("b", undefined); exit(); text("after")'))),
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "ends the script successfully on exit(), keeping output and store writes" } },
        ),
        await exactScenario(
            "a script failure keeps prior output and reports the script error with its line",
            TOOL_SOURCE,
            { script: 'text("before");\nthrow new Error("boom")' },
            {
                ok: false,
                error: { kind: "script", name: "Error", message: "boom", stack: "Error: boom\n    at <anonymous> (codemode.js:2:11)" },
                output: [{ type: "text", text: "before" }],
                calls: [],
            },
            async () => normalizeCalls(await withSandbox({}, (sandbox) => sandbox.execute('text("before");\nthrow new Error("boom")'))),
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "keeps output produced before a failure" } },
        ),
        await exactScenario(
            "non-Error throws, syntax errors and non-serializable returns are script errors",
            TOOL_SOURCE,
            { scripts: ['throw "plain"', "return 1n", "text("] },
            [
                { ok: false, error: { kind: "script", message: "plain" }, output: [], calls: [] },
                {
                    ok: false,
                    error: { kind: "script", name: "TypeError", message: "BigInt are forbidden in JSON.stringify", stack: "TypeError: BigInt are forbidden in JSON.stringify\n    at stringify (native)" },
                    output: [],
                    calls: [],
                },
                {
                    ok: false,
                    error: {
                        kind: "script",
                        name: "SyntaxError",
                        message: "unexpected token in expression: '}'",
                        stack: "SyntaxError: unexpected token in expression: '}'\n    at codemode.js:2:1",
                    },
                    output: [],
                    calls: [],
                },
            ],
            async () => {
                const results = [];
                for (const script of ['throw "plain"', "return 1n", "text("]) {
                    results.push(normalizeCalls(await withSandbox({}, (sandbox) => sandbox.execute(script))));
                }
                return results;
            },
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "reports non-Error throws,reports a non-serializable return value as a script error" } },
        ),
        await exactScenario(
            "tools are async functions under normalized identifiers and record status and duration presence",
            TOOL_SOURCE,
            { tools: ["read", "my-tool"] },
            {
                ok: true,
                value: ["function", "function", ["read", "my_tool"], { got: { path: "x" } }, "y"],
                output: [],
                calls: [
                    { name: "read", status: "ok", durationPresent: true },
                    { name: "my-tool", status: "ok", durationPresent: true },
                ],
                storeWrites: { set: {}, delete: [] },
            },
            async () =>
                normalizeCalls(
                    await withSandbox(
                        {
                            tools: [
                                { name: "read", description: "Read", execute: (args: unknown) => ({ got: args }) },
                                { name: "my-tool", execute: (args: unknown) => args },
                            ],
                        },
                        (sandbox) =>
                            sandbox.execute(
                                'const a=typeof tools.read, b=typeof tools["my-tool"], c=ALL_TOOLS.map(t=>t.name); const one=await tools.read({path:"x"}); const two=await tools.my_tool("y"); return [a,b,c,one,two];',
                            ),
                    ),
                ),
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "exposes tools as async functions and records calls" } },
        ),
        await exactScenario(
            "unknown tools and thrown tool errors are catchable, with error call status",
            TOOL_SOURCE,
            { tools: ["fail"] },
            {
                ok: true,
                value: [
                    "TypeError",
                    'tools.nope does not exist. Available: fail. ALL_TOOLS lists every tool; searchTools(query) finds tools by topic. Check for a member with "nope" in tools.',
                    "Error",
                    "boom",
                ],
                output: [],
                calls: [{ name: "fail", status: "error", durationPresent: true }],
                storeWrites: { set: {}, delete: [] },
            },
            async () =>
                normalizeCalls(
                    await withSandbox({ tools: [{ name: "fail", execute: () => { throw new Error("boom"); } }] }, (sandbox) =>
                        sandbox.execute(
                            "let unknown; try { await tools.nope(1); unknown = ['none']; } catch (e) { unknown = [e.name, e.message]; } try { await tools.fail(); } catch (e) { return [...unknown, e.name, e.message]; }",
                        ),
                    ),
                ),
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "turns tool errors into catchable Errors in the script" } },
        ),
        await exactScenario(
            "an unawaited tool call is aborted when the script returns and its record is cancelled",
            TOOL_SOURCE,
            { script: 'tools.hang(); return "done"' },
            {
                ok: true,
                value: "done",
                output: [],
                calls: [{ name: "hang", status: "cancelled", durationPresent: true }],
                storeWrites: { set: {}, delete: [] },
            },
            async () => {
                let aborted = false;
                const result = await withSandbox(
                    {
                        tools: [
                            {
                                name: "hang",
                                execute: (_args: unknown, context: { signal: AbortSignal }) =>
                                    new Promise((_resolve, reject) => {
                                        context.signal.addEventListener("abort", () => {
                                            aborted = true;
                                            reject(new Error("aborted"));
                                        });
                                    }),
                            },
                        ],
                    },
                    (sandbox) => sandbox.execute('tools.hang(); return "done"'),
                );
                assert(aborted, "the tool call signal must abort when the script returns");
                return normalizeCalls(result);
            },
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "aborts unawaited calls when the script returns" } },
        ),
        await exactScenario(
            "globals support top-level spread calls and dotted frozen namespaces",
            TOOL_SOURCE,
            { globals: ["search", "ns.member"] },
            {
                ok: true,
                value: [["a", 2], { x: 1 }],
                output: [],
                calls: [],
                storeWrites: { set: {}, delete: [] },
            },
            async () =>
                normalizeCalls(
                    await withSandbox(
                        {
                            globals: [
                                { name: "search", spread: true, execute: (args: unknown) => args },
                                { name: "ns.member", execute: (args: unknown) => args },
                            ],
                        },
                        (sandbox) => sandbox.execute('return [await search("a", 2), await ns.member({x: 1})]'),
                    ),
                ),
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "numbers calls to globals with tools, and answers errors as rejections" } },
        ),
        await exactScenario(
            "invalid global names, duplicates and namespace conflicts are rejected at construction",
            TOOL_SOURCE,
            { globals: ["tools", "a-b", "x,x", "ns,ns.m"] },
            [
                'Invalid global name "tools"',
                'Invalid global name "a-b"',
                'Global "x" is already registered',
                'Global "ns" conflicts with the namespace "ns"',
            ],
            async () => {
                const cases = [
                    [{ name: "tools", execute: () => 1 }],
                    [{ name: "a-b", execute: () => 1 }],
                    [{ name: "x", execute: () => 1 }, { name: "x", execute: () => 2 }],
                    [{ name: "ns", execute: () => 1 }, { name: "ns.m", execute: () => 2 }],
                ];
                return cases.map((globals) => {
                    try {
                        new CodemodeSandbox({ wasm: WASM, globals });
                        return { message: "no error" };
                    } catch (error) {
                        return { message: (error as Error).message };
                    }
                }).map((entry) => entry.message);
            },
            { upstreamTest: { path: "packages/codemode/src/runtime/execution.ts", case: "checkGlobals" } },
        ),
        await exactScenario(
            "store and load use a JSON snapshot and only successful executions report writes",
            TOOL_SOURCE,
            { store: { existing: { keep: true } }, scripts: ["success", "failure"] },
            [
                {
                    ok: true,
                    value: { keep: true },
                    output: [],
                    calls: [],
                    storeWrites: { set: { added: { x: 1 } }, delete: ["obsolete"] },
                },
                {
                    ok: false,
                    error: { kind: "script", name: "Error", message: "x", stack: "Error: x\n    at <anonymous> (codemode.js:1:57)" },
                    output: [],
                    calls: [],
                },
            ],
            async () => {
                return withSandbox({}, async (sandbox) => {
                    const success = await sandbox.execute(
                        'store("added", {x: 1}); store("obsolete", undefined); return load("existing")',
                        { store: { existing: { keep: true }, obsolete: 1 } },
                    );
                    const failure = await sandbox.execute('store("lost", 1); throw new Error("x")');
                    return [normalizeCalls(success), normalizeCalls(failure)];
                });
            },
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "store" } },
        ),
        await exactScenario(
            "the public tool table can be registered, listed and unregistered; close rejects later runs",
            TOOL_SOURCE,
            { tools: ["a-b", "a_b"] },
            {
                duplicateConstructorAllowed: true,
                duplicateRegister: 'Tool "a-b" is already registered',
                listed: ["a-b", "a_b"],
                removedExisting: true,
                removedMissing: false,
                listedAfter: ["a_b"],
                closed: "Sandbox is closed",
            },
            async () => {
                const sandbox = new CodemodeSandbox({
                    wasm: WASM,
                    tools: [
                        { name: "a-b", execute: () => 1 },
                        { name: "a_b", execute: () => 2 },
                    ],
                });
                const duplicateConstructorAllowed = true;
                let duplicateRegister = "";
                try {
                    sandbox.registerTool({ name: "a-b", execute: () => 1 });
                } catch (error) {
                    duplicateRegister = (error as Error).message;
                }
                const listed = sandbox.tools.map((tool: any) => tool.name);
                const removedExisting = sandbox.unregisterTool("a-b");
                const removedMissing = sandbox.unregisterTool("missing");
                const listedAfter = sandbox.tools.map((tool: any) => tool.name);
                await sandbox.close();
                let closed = "";
                try {
                    await sandbox.execute("return 1");
                } catch (error) {
                    closed = (error as Error).message;
                }
                return { duplicateConstructorAllowed, duplicateRegister, listed, removedExisting, removedMissing, listedAfter, closed };
            },
            { upstreamTest: { path: "packages/codemode/src/runtime/execution.ts", case: "registerTool,unregisterTool,close" } },
        ),
        await exactScenario(
            "a caller abort ends a waiting script, aborts its tool and keeps partial output",
            TOOL_SOURCE,
            { timeoutMs: "disabled", cancellation: "AbortController" },
            {
                ok: false,
                error: { kind: "aborted", message: "cancelled" },
                output: [{ type: "text", text: "partial" }],
                calls: [{ name: "hang", status: "cancelled", durationPresent: true }],
            },
            async () => {
                let started: () => void = () => {};
                const called = new Promise<void>((resolve) => {
                    started = resolve;
                });
                let toolAborted = false;
                const sandbox = new CodemodeSandbox({
                    wasm: WASM,
                    tools: [
                        {
                            name: "hang",
                            execute: (_args: unknown, context: { signal: AbortSignal }) => {
                                started();
                                return new Promise((_resolve, reject) => {
                                    context.signal.addEventListener("abort", () => {
                                        toolAborted = true;
                                        reject(new Error("tool aborted"));
                                    });
                                });
                            },
                        },
                    ],
                });
                try {
                    const controller = new AbortController();
                    const pending = sandbox.execute("text('partial'); await tools.hang()", { signal: controller.signal });
                    await called;
                    controller.abort(new Error("cancelled"));
                    const result = normalizeCalls(await pending);
                    assert(toolAborted, "the in-flight tool signal must abort");
                    return result;
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/worker-lifetime.test.ts", case: "settles abort without waiting for worker exit" } },
        ),
        await exactScenario(
            "a script that computes past its deadline is a timeout",
            TOOL_SOURCE,
            { script: "while (true) {}", timeoutMs: 50 },
            {
                ok: false,
                error: { kind: "timeout", message: "Execution timed out after 50 ms" },
                output: [],
                calls: [],
            },
            async () => normalizeCalls(await withSandbox({}, (sandbox) => sandbox.execute("while (true) {}", { timeoutMs: 50 }))),
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "times out" } },
        ),
        await exactScenario(
            "a memory-limited VM reports out of memory as a script error",
            TOOL_SOURCE,
            { memoryLimitBytes: 8388608 },
            {
                ok: false,
                error: { kind: "script", name: "InternalError", message: "out of memory", stack: "InternalError: out of memory\n    at <anonymous> (codemode.js:1:82)" },
                output: [],
                calls: [],
            },
            async () =>
                normalizeCalls(
                    await withSandbox({ memoryLimitBytes: 8 * 1024 * 1024 }, (sandbox) =>
                        sandbox.execute("const parts = []; for (;;) parts.push('x'.repeat(1 << 20) + parts.length);"),
                    ),
                ),
            { upstreamTest: { path: "packages/codemode/test/portable.test.ts", case: "caps the interrupt budget and memory at the remote's limits" } },
        ),
        await exactScenario(
            "a pre-aborted execution never starts the guest and reports the caller reason",
            TOOL_SOURCE,
            { cancellation: "already-aborted AbortSignal" },
            { ok: false, error: { kind: "aborted", message: "early" }, output: [], calls: [] },
            async () =>
                normalizeCalls(
                    await withSandbox({}, (sandbox) => {
                        const controller = new AbortController();
                        controller.abort(new Error("early"));
                        return sandbox.execute("return 1", { signal: controller.signal });
                    }),
                ),
            { upstreamTest: { path: "packages/codemode/test/sandbox.test.ts", case: "cancellation" } },
        ),
        await goldenScenario(
            "the guest prelude is the frozen sandbox prelude with output and store limits",
            "packages/codemode/src/runtime/prelude-source.ts:PRELUDE_SOURCE",
            { export: "PRELUDE_SOURCE" },
            () => PRELUDE_SOURCE,
            (observed) => {
                assert(typeof observed === "string" && observed.length > 10_000, "the prelude text must be present");
                assert(observed.includes('"use strict"'), "the prelude must be strict");
                assert(observed.includes("Object.freeze"), "the prelude must freeze built-ins");
                assert(observed.includes("16777216") && observed.includes("100000"), "prelude carries the output limits");
                assert(observed.includes("262144") && observed.includes("1048576"), "prelude carries the store limits");
            },
            {
                normalizations: ["the prelude text is an exact golden; no environment variation is recorded"],
                upstreamTest: { path: "packages/codemode/src/runtime/prelude-source.ts", case: "PRELUDE_SOURCE" },
            },
        ),
    ];
}

const WORKER_SOURCE = "packages/codemode/src/runtime/host.ts:workerLauncher,CodemodeSandbox,CodemodeSandboxOptions.acquireWorker,CodemodeSandboxOptions.onWorkerStarted";

/** Host records for a real worker: admission, start hooks and actual exits. */
interface WorkerStats {
    acquired: number;
    released: number;
    started: number;
    exits: Array<Promise<void>>;
}

function workerStats(): WorkerStats {
    return { acquired: 0, released: 0, started: 0, exits: [] };
}

function workerOptions(stats: WorkerStats, tools: unknown[] = []) {
    return {
        wasm: WASM,
        tools,
        acquireWorker: () => {
            stats.acquired++;
            return () => {
                stats.released++;
            };
        },
        onWorkerStarted: (exited: Promise<void>) => {
            stats.started++;
            stats.exits.push(exited);
        },
    };
}

const hangingTool = {
    name: "hang",
    execute: (_args: unknown, context: { signal: AbortSignal }) =>
        new Promise((_resolve, reject) => {
            context.signal.addEventListener("abort", () => reject(new Error("tool aborted")));
        }),
};

async function waitFor(check: () => boolean): Promise<void> {
    const deadline = Date.now() + 5000;
    while (!check()) {
        if (Date.now() > deadline) throw new Error("condition was not reached");
        await new Promise((resolve) => setTimeout(resolve, 5));
    }
}

/** Keep the event loop referenced while an unref'd terminated worker reports its exit. */
async function waitForExits(stats: WorkerStats): Promise<void> {
    await new Promise((resolve) => setTimeout(resolve, 800));
    await Promise.all(stats.exits);
}

/** Worker lifetime: admission, construction failure, settlement before exit and release accounting. */
export async function workerLifetimeScenarios(): Promise<Scenario[]> {
    return [
        await exactScenario(
            "admission denial rejects before a worker starts and keeps the start hook uncalled",
            WORKER_SOURCE,
            { acquireWorker: "throws worker capacity exhausted" },
            {
                ok: false,
                errorKind: "sandbox",
                errorMessage: "worker capacity exhausted",
                acquired: 1,
                started: 0,
            },
            async () => {
                const stats = workerStats();
                const sandbox = new CodemodeSandbox({
                    wasm: WASM,
                    acquireWorker: () => {
                        stats.acquired++;
                        throw new Error("worker capacity exhausted");
                    },
                    onWorkerStarted: () => {
                        stats.started++;
                    },
                });
                try {
                    const result = await sandbox.execute("while (true) {}");
                    return {
                        ok: result.ok,
                        errorKind: result.error?.kind,
                        errorMessage: result.error?.message,
                        acquired: stats.acquired,
                        started: stats.started,
                    };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/worker-lifetime.test.ts", case: "does not start a worker when admission is denied" } },
        ),
        await exactScenario(
            "a wasm loading failure does not reserve admission",
            WORKER_SOURCE,
            { wasm: "load rejects" },
            { ok: false, errorKind: "sandbox", errorMessage: "Failed to load QuickJS: wasm unavailable", acquired: 0 },
            async () => {
                const stats = workerStats();
                const sandbox = new CodemodeSandbox({
                    wasm: Promise.reject(new Error("wasm unavailable")),
                    acquireWorker: () => {
                        stats.acquired++;
                        return () => {};
                    },
                });
                try {
                    const result = await sandbox.execute("return 1");
                    return { ok: result.ok, errorKind: result.error?.kind, errorMessage: result.error?.message, acquired: stats.acquired };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/worker-lifetime.test.ts", case: "does not acquire admission when wasm loading fails" } },
        ),
        await exactScenario(
            "a worker construction failure returns its reserved admission exactly once",
            WORKER_SOURCE,
            { workerUrl: "https://invalid.example/worker.js" },
            {
                ok: false,
                errorKind: "sandbox",
                errorMessage: "Failed to start worker: The URL must be of scheme file",
                acquired: 1,
                released: 1,
            },
            async () => {
                const stats = workerStats();
                const sandbox = new CodemodeSandbox({
                    wasm: WASM,
                    workerUrl: new URL("https://invalid.example/worker.js"),
                    acquireWorker: () => {
                        stats.acquired++;
                        return () => {
                            stats.released++;
                        };
                    },
                });
                try {
                    const result = await sandbox.execute("return 1");
                    return {
                        ok: result.ok,
                        errorKind: result.error?.kind,
                        errorMessage: result.error?.message,
                        acquired: stats.acquired,
                        released: stats.released,
                    };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/worker-lifetime.test.ts", case: "releases admission when worker construction fails" } },
        ),
        await goldenScenario(
            "repeated abort results settle while cancelled workers remain unreleased; exits release every admission",
            WORKER_SOURCE,
            { cancellations: 3, tool: "hang" },
            async () => {
                const stats = workerStats();
                const kinds: string[] = [];
                const releasedAtResult: number[] = [];
                const acquiredAtResult: number[] = [];
                for (let index = 0; index < 3; index++) {
                    let started: () => void = () => {};
                    const called = new Promise<void>((resolve) => {
                        started = resolve;
                    });
                    const tool = {
                        name: "hang",
                        execute: (_args: unknown, context: { signal: AbortSignal }) => {
                            started();
                            return new Promise((_resolve, reject) => {
                                context.signal.addEventListener("abort", () => reject(new Error("tool aborted")));
                            });
                        },
                    };
                    const sandbox = new CodemodeSandbox(workerOptions(stats, [tool]));
                    const controller = new AbortController();
                    const pending = sandbox.execute("text('partial'); await tools.hang()", { signal: controller.signal });
                    await called;
                    controller.abort(new Error("cancelled"));
                    const result = await pending;
                    kinds.push(result.error?.kind ?? "none");
                    releasedAtResult.push(stats.released);
                    acquiredAtResult.push(stats.acquired);
                    await sandbox.close();
                }
                await waitForExits(stats);
                return {
                    kinds,
                    releasedAtResult,
                    acquiredAtResult,
                    acquired: stats.acquired,
                    started: stats.started,
                    exitCount: stats.exits.length,
                    releasedAfterExit: stats.released,
                };
            },
            (observed) => {
                assert(observed.kinds.length === 3 && observed.kinds.every((kind: string) => kind === "aborted"), "every cancellation is aborted");
                assert(observed.acquired === 3 && observed.started === 3, "each cancellation starts one worker");
                assert(observed.exitCount === 3 && observed.releasedAfterExit === 3, "every worker exits and releases once");
                observed.releasedAtResult.forEach((released: number, index: number) => {
                    assert(released < observed.acquiredAtResult[index]!, "a result-only counter would still see a live worker");
                });
                assert(observed.releasedAtResult[2]! < observed.acquired, "the third result settles before its worker exits");
            },
            {
                normalizations: [
                    "the observation is the immediate release counter after each result and after all exits; wall-clock delay is not compared",
                ],
                upstreamTest: { path: "packages/codemode/test/worker-lifetime.test.ts", case: "settles %s without waiting for worker exit" },
            },
        ),
        await exactScenario(
            "close settles an in-flight run before the worker exit releases admission",
            WORKER_SOURCE,
            { closeWhileToolHangs: true },
            {
                kind: "aborted",
                message: "Sandbox closed",
                releasedAtResult: 0,
                acquired: 1,
                releasedAfterExit: 1,
                started: 1,
                exitCount: 1,
            },
            async () => {
                const stats = workerStats();
                let started: () => void = () => {};
                const called = new Promise<void>((resolve) => {
                    started = resolve;
                });
                const tool = {
                    name: "hang",
                    execute: (_args: unknown, context: { signal: AbortSignal }) => {
                        started();
                        return new Promise((_resolve, reject) => {
                            context.signal.addEventListener("abort", () => reject(new Error("tool aborted")));
                        });
                    },
                };
                const sandbox = new CodemodeSandbox(workerOptions(stats, [tool]));
                const pending = sandbox.execute("await tools.hang()");
                await called;
                void sandbox.close();
                const result = await pending;
                const releasedAtResult = stats.released;
                await waitForExits(stats);
                return {
                    kind: result.error?.kind,
                    message: result.error?.message,
                    releasedAtResult,
                    acquired: stats.acquired,
                    releasedAfterExit: stats.released,
                    started: stats.started,
                    exitCount: stats.exits.length,
                };
            },
            {
                normalizations: ["the observation is the release counter immediately after the result and after all exits"],
                upstreamTest: { path: "packages/codemode/test/worker-lifetime.test.ts", case: "settles close without waiting for worker exit" },
            },
        ),
        await exactScenario(
            "a throwing lifetime hook requests termination and still releases the worker",
            WORKER_SOURCE,
            { onWorkerStarted: "throws lifetime hook failed" },
            { ok: false, errorKind: "sandbox", errorMessage: "lifetime hook failed", acquired: 1, released: 1, exitDefined: true },
            async () => {
                const stats = workerStats();
                let exit: Promise<void> | undefined;
                const sandbox = new CodemodeSandbox({
                    wasm: WASM,
                    acquireWorker: () => {
                        stats.acquired++;
                        return () => {
                            stats.released++;
                        };
                    },
                    onWorkerStarted: (exited: Promise<void>) => {
                        exit = exited;
                        throw new Error("lifetime hook failed");
                    },
                });
                try {
                    const result = await sandbox.execute("while (true) {}");
                    await waitForExits(stats);
                    return {
                        ok: result.ok,
                        errorKind: result.error?.kind,
                        errorMessage: result.error?.message,
                        acquired: stats.acquired,
                        released: stats.released,
                        exitDefined: exit !== undefined,
                    };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/worker-lifetime.test.ts", case: "requests termination when onWorkerStarted throws" } },
        ),
        await exactScenario(
            "execute and start each start one worker and release it only after actual exit",
            WORKER_SOURCE,
            { runs: ["execute", "start"] },
            { executeValue: 1, startValue: 2, started: 2, acquired: 2, exitCount: 2, releasedAfterExit: 2, finished: true },
            async () => {
                const stats = workerStats();
                const sandbox = new CodemodeSandbox(workerOptions(stats));
                try {
                    const executeResult = await sandbox.execute("return 1");
                    const run = sandbox.start("return 2", { startedAt: 0, seed: 0, onCall: () => {} });
                    const startResult = await run.result;
                    await waitForExits(stats);
                    return {
                        executeValue: executeResult.ok ? executeResult.value : undefined,
                        startValue: startResult.ok ? startResult.value : undefined,
                        started: stats.started,
                        acquired: stats.acquired,
                        exitCount: stats.exits.length,
                        releasedAfterExit: stats.released,
                        finished: run.finished,
                    };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/worker-lifetime.test.ts", case: "reports the actual exit of execute() and start() workers" } },
        ),
        await exactScenario(
            "a real Node child reports the result before its worker exits and exits promptly for abort, timeout and close",
            "packages/codemode/test/fixtures/worker-process-exit.ts:worker-process-exit",
            { runtime: "node", modes: ["abort", "timeout", "close"] },
            [
                { mode: "abort", kind: "aborted", workerExitedAtResult: false, resultUnderOneSecond: true, exitGapUnderOneSecond: true },
                { mode: "timeout", kind: "timeout", workerExitedAtResult: false, resultUnderOneSecond: true, exitGapUnderOneSecond: true },
                { mode: "close", kind: "aborted", workerExitedAtResult: false, resultUnderOneSecond: true, exitGapUnderOneSecond: true },
            ],
            async () => {
                const observations = [];
                for (const mode of ["abort", "timeout", "close"] as const) {
                    const { stdout } = await execFileAsync(
                        process.execPath,
                        [join(process.env.PI_CHECKOUT!, "packages/codemode/test/fixtures/worker-process-exit.ts"), mode],
                        { cwd: process.env.PI_CHECKOUT!, timeout: 20000 },
                    );
                    const lines = stdout.trim().split(/\r?\n/);
                    assert(lines.length === 2, `${mode}: expected a result line and an exit line`);
                    const result = JSON.parse(lines[0]!) as { kind: string; elapsedMs: number; workerExited: boolean };
                    const exit = JSON.parse(lines[1]!) as { elapsedMs: number };
                    observations.push({
                        mode,
                        kind: result.kind,
                        workerExitedAtResult: result.workerExited,
                        resultUnderOneSecond: result.elapsedMs < 1000,
                        exitGapUnderOneSecond: exit.elapsedMs - result.elapsedMs < 1000,
                    });
                }
                return observations;
            },
            {
                normalizations: ["raw elapsed milliseconds are replaced by the contract's under-one-second relations"],
                upstreamTest: { path: "packages/codemode/test/worker-lifetime.test.ts", case: "exits promptly after %s with a real worker" },
            },
        ),
    ];
}

const DETERMINISTIC = "packages/codemode/src/runtime/execution.ts:CodemodeSandboxBase.start,CodemodeRun.deliver,Execution.deliver";
const STARTED_AT = Date.UTC(2026, 0, 15, 10, 0, 0);
const PINNED_SEED_ONE = 0.34227450605335275;

interface RecordedRun {
    run: any;
    calls: any[];
    idles: Array<[number, number]>;
    events: string[];
    fingerprint: () => string | undefined;
}

function startRecording(sandbox: any, code: string, options: Record<string, any> = {}): RecordedRun {
    const calls: any[] = [];
    const idles: Array<[number, number]> = [];
    const events: string[] = [];
    let fingerprint: string | undefined;
    let seq = 0;
    const answer = options.answer ?? ((call: any) => ({ ok: true, value: call.args }));
    const run = sandbox.start(code, {
        startedAt: STARTED_AT,
        seed: 1,
        tools: [{ name: "echo" }, { name: "wait" }],
        timeoutMs: 20_000,
        ...options,
        onStarted: (value: string) => {
            fingerprint = value;
            events.push("started");
        },
        onCall: (call: any) => {
            calls.push(call);
            events.push(`call ${call.id}`);
            if (call.name === "wait") return;
            queueMicrotask(() => {
                seq++;
                run.deliver(seq, STARTED_AT + 1000 * seq, call.id, answer(call));
            });
        },
        onIdle: (value: number, issued: number) => {
            idles.push([value, issued]);
            events.push(`idle ${value}`);
        },
    });
    return { run, calls, idles, events, fingerprint: () => fingerprint };
}

/** Deterministic host-delivery, replay, fingerprint and mismatch observations. */
export async function deterministicScenarios(): Promise<Scenario[]> {
    const DETERMINISTIC_SCRIPT =
        'const order = []; await Promise.all([tools.wait("a").then(() => order.push("a")), tools.wait("b").then(() => order.push("b"))]); return order;';
    return [
        await exactScenario(
            "the same script, start time, seed and answers make the same calls and output",
            DETERMINISTIC,
            { startedAt: STARTED_AT, seed: 1, script: "clock/random/echo" },
            {
                sameCalls: true,
                sameValue: true,
                sameOutput: true,
                calls: [
                    { id: 1, target: "tool", name: "echo", args: [STARTED_AT, 0, PINNED_SEED_ONE, 0.4620546081390098, "2026-01-15T10:00:00.000Z"] },
                    { id: 2, target: "tool", name: "echo", args: [STARTED_AT, 0, PINNED_SEED_ONE, 0.4620546081390098, "2026-01-15T10:00:00.000Z", 0.056110785474501323] },
                ],
                output: [{ type: "text", text: "[1768471201000,1000,0.5807179129204683]" }],
                idles: [[0, 1], [1, 2]],
                during: { seq: 2, issued: 2 },
            },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const code =
                        'const values = [Date.now(), performance.now(), Math.random(), Math.random(), new Date().toISOString()]; const first = await tools.echo(values); text(JSON.stringify([Date.now(), performance.now(), Math.random()])); return await tools.echo(first.concat(Math.random()));';
                    const one = startRecording(sandbox, code);
                    const two = startRecording(sandbox, code);
                    const [a, b] = await Promise.all([one.run.result, two.run.result]);
                    const withoutDurations = (result: any) => ({
                        ...result,
                        calls: result.calls.map(({ name, status }: any) => ({ name, status })),
                    });
                    return {
                        sameCalls: JSON.stringify(one.calls) === JSON.stringify(two.calls),
                        sameValue: JSON.stringify((a as any).value) === JSON.stringify((b as any).value),
                        sameOutput: JSON.stringify(withoutDurations(a)) === JSON.stringify(withoutDurations(b)),
                        calls: one.calls,
                        output: (a as any).output,
                        idles: one.idles,
                        during: (a as any).during,
                    };
                } finally {
                    await sandbox.close();
                }
            },
            {
                normalizations: ["call durationMs is wall-clock and is not part of the replay equality; presence and status remain"],
                upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "makes the same calls and output for the same start, seed, and results" },
            },
        ),
        await exactScenario(
            "deterministic runs use a virtual UTC clock and seeded randomness without WeakRef",
            DETERMINISTIC,
            { startedAt: STARTED_AT, seed: 1, seeds: [0, 1] },
            {
                clock: [STARTED_AT, 0, PINNED_SEED_ONE, 0, 0, "undefined", "undefined"],
                seedZero: [0.6711372530266764, 0.7258461452833667],
            },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const clock = startRecording(
                        sandbox,
                        "return [Date.now(), performance.now(), Math.random(), new Date(0).getHours(), new Date(0).getTimezoneOffset(), typeof WeakRef, typeof FinalizationRegistry]",
                    );
                    const zero = startRecording(sandbox, "return [Math.random(), Math.random()]", { seed: 0 });
                    return {
                        clock: (await clock.run.result).value,
                        seedZero: (await zero.run.result).value,
                    };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "runs in UTC whatever the host's timezone, and without WeakRef or FinalizationRegistry" } },
        ),
        await exactScenario(
            "results are applied in seq order even when delivered in reverse order",
            DETERMINISTIC,
            { script: DETERMINISTIC_SCRIPT, delivery: "seq 2 then seq 1" },
            {
                value: ["b", "a"],
                idles: [[0, 2], [1, 2]],
                calls: [
                    { id: 1, target: "tool", name: "wait", args: "a" },
                    { id: 2, target: "tool", name: "wait", args: "b" },
                ],
            },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const recorded = startRecording(sandbox, DETERMINISTIC_SCRIPT);
                    await waitFor(() => recorded.calls.length === 2);
                    const [a, b] = recorded.calls;
                    recorded.run.deliver(2, STARTED_AT + 2000, a.id, { ok: true, value: null });
                    recorded.run.deliver(1, STARTED_AT + 1000, b.id, { ok: true, value: null });
                    const result = await recorded.run.result;
                    return { value: result.value, idles: recorded.idles, calls: recorded.calls };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "applies results in seq order, whatever order they are delivered in" } },
        ),
        await goldenScenario(
            "the VM fingerprint arrives before calls, idles report issued calls, and a different heap changes the fingerprint",
            DETERMINISTIC,
            { memoryLimitBytes: [64 * 1024 * 1024, 32 * 1024 * 1024] },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM, memoryLimitBytes: 64 * 1024 * 1024 });
                const other = new CodemodeSandbox({ wasm: WASM, memoryLimitBytes: 32 * 1024 * 1024 });
                try {
                    const recorded = startRecording(sandbox, "await tools.echo(1); await Promise.all([tools.echo(2), tools.echo(3)]);");
                    const result = await recorded.run.result;
                    const same = startRecording(sandbox, "return 1");
                    const different = startRecording(other, "return 1");
                    await Promise.all([same.run.result, different.run.result]);
                    return {
                        events: recorded.events,
                        idles: recorded.idles,
                        calls: recorded.calls,
                        fingerprint: recorded.fingerprint(),
                        sameFingerprint: same.fingerprint(),
                        differentFingerprint: different.fingerprint(),
                        finished: recorded.run.finished,
                        resultOk: result.ok,
                    };
                } finally {
                    await sandbox.close();
                    await other.close();
                }
            },
            (observed) => {
                assert(observed.events.slice(0, 3).join(",") === "started,call 1,idle 0", "started precedes the first call and its idle");
                assert(JSON.stringify(observed.idles) === "[[0,1],[1,3],[2,3]]", "idle progress reports issued calls");
                assert(observed.calls.map((call: any) => call.id).join(",") === "1,2,3", "calls are numbered in issue order");
                assert(/^quickjs-wasi \S+, quickjs \S+, prelude [0-9a-f]{8}, deterministic \d+, stack \d+, heap 67108864, budget none$/.test(observed.fingerprint), "fingerprint names the VM, prelude, heap and budget");
                assert(observed.sameFingerprint === observed.fingerprint, "same VM options keep the fingerprint");
                assert(observed.differentFingerprint !== observed.fingerprint, "a different heap is a different VM fingerprint");
                assert(observed.finished === true && observed.resultOk === true, "a settled run reports finished");
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "reports the fingerprint before any call and the calls issued after each result" } },
        ),
        await exactScenario(
            "during reports the result the VM was processing when the script ended",
            DETERMINISTIC,
            { scripts: ["return 1", "await tools.echo(1); return await tools.echo(2)", "await tools.echo(1); await Promise.all([tools.echo(2), tools.echo(3)]); throw new Error('x')"] },
            [
                { ok: true, during: { seq: 0, issued: 0 } },
                { ok: true, during: { seq: 2, issued: 2 } },
                { ok: false, during: { seq: 3, issued: 3 } },
                { executeHasDuring: false },
            ],
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const results = [];
                    for (const code of ["return 1", "await tools.echo(1); return await tools.echo(2)", "await tools.echo(1); await Promise.all([tools.echo(2), tools.echo(3)]); throw new Error('x')"]) {
                        const result = await startRecording(sandbox, code).run.result;
                        results.push({ ok: result.ok, during: result.during });
                    }
                    const execute = await sandbox.execute("return 1");
                    return [...results, { executeHasDuring: "during" in execute }];
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "reports the result the VM was processing when the script ended" } },
        ),
        await goldenScenario(
            "deliver rejects a wrong call, a reused result number and a non-finite clock without settling the run",
            DETERMINISTIC,
            { delivered: ["Clock NaN", "wrong id 99", "valid"] },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const recorded = startRecording(sandbox, "return await tools.wait(1)");
                    await waitFor(() => recorded.calls.length === 1);
                    const errors: string[] = [];
                    for (const [seq, now, id] of [[1, Number.NaN, 1], [1, STARTED_AT, 99]] as const) {
                        try {
                            recorded.run.deliver(seq as number, now as number, id, { ok: true, value: null });
                        } catch (error) {
                            errors.push((error as Error).message);
                        }
                    }
                    recorded.run.deliver(1, STARTED_AT, 1, { ok: true, value: "fine" });
                    return { errors, result: (await recorded.run.result).value };
                } finally {
                    await sandbox.close();
                }
            },
            (observed) => {
                assert(observed.errors[0] === "The clock value of result 1 is not a finite number", "non-finite clock is rejected");
                assert(observed.errors[1] === "Call 99 is not waiting for a result", "unknown call is rejected");
                assert(observed.result === "fine", "the run stays alive after rejected deliveries");
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "keeps the clock monotonic and rejects a clock value that is not finite" } },
        ),
        await exactScenario(
            "a reused result number is rejected",
            DETERMINISTIC,
            { delivery: "seq 1 twice" },
            { error: "Result number 1 is not a positive integer used once", value: ["a", "b"] },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const recorded = startRecording(sandbox, DETERMINISTIC_SCRIPT);
                    await waitFor(() => recorded.calls.length === 2);
                    const [a, b] = recorded.calls;
                    recorded.run.deliver(1, STARTED_AT, a.id, { ok: true, value: null });
                    let error = "";
                    try {
                        recorded.run.deliver(1, STARTED_AT, b.id, { ok: true, value: null });
                    } catch (caught) {
                        error = (caught as Error).message;
                    }
                    recorded.run.deliver(2, STARTED_AT, b.id, { ok: true, value: null });
                    return { error, value: (await recorded.run.result).value };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "rejects a call that is not waiting and a result number used twice" } },
        ),
        await exactScenario(
            "a changed seed is a replay mismatch and changes randomness without changing the start time",
            DETERMINISTIC,
            { seeds: [1, 2, 0], sameStartedAt: true },
            { sameSeedSame: true, reseededDifferent: true, seedZeroDiffersFromSeedOne: true, sameStartTime: [STARTED_AT, STARTED_AT] },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const code = "return [Date.now(), Math.random()]";
                    const one = startRecording(sandbox, code);
                    const again = startRecording(sandbox, code);
                    const two = startRecording(sandbox, code, { seed: 2 });
                    const zero = startRecording(sandbox, code, { seed: 0 });
                    const [a, b, c, d] = await Promise.all([one.run.result, again.run.result, two.run.result, zero.run.result]);
                    const values = [a.value, b.value, c.value, d.value] as number[][];
                    return {
                        sameSeedSame: values[0]![1] === values[1]![1],
                        reseededDifferent: values[0]![1] !== values[2]![1],
                        seedZeroDiffersFromSeedOne: values[3]![1] !== values[0]![1],
                        sameStartTime: [values[0]![0], values[1]![0]],
                    };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "the seed decides the random values, not the start time" } },
        ),
        await exactScenario(
            "abort in a deterministic run is an uncatchable aborted error and finished becomes true",
            DETERMINISTIC,
            { cancellation: "after start, while waiting" },
            {
                ok: false,
                error: { kind: "aborted", message: "diverged" },
                output: [],
                calls: [{ name: "wait", status: "cancelled", durationPresent: true }],
            },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const controller = new AbortController();
                    const recorded = startRecording(
                        sandbox,
                        'try { await tools.wait(1); } catch { text("caught"); } text("after");',
                        { signal: controller.signal },
                    );
                    await waitFor(() => recorded.calls.length === 1);
                    controller.abort(new Error("diverged"));
                    return normalizeCalls(await recorded.run.result) as any;
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "ends with an error the script cannot catch when the caller aborts" } },
        ),
        await exactScenario(
            "a nonserializable answer leaves the call waiting and a later valid answer settles it",
            DETERMINISTIC,
            { answers: ["1n", "'fine'"] },
            { error: "Do not know how to serialize a BigInt", value: "fine" },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const recorded = startRecording(sandbox, "return await tools.wait(1)");
                    await waitFor(() => recorded.calls.length === 1);
                    let error = "";
                    try {
                        recorded.run.deliver(1, STARTED_AT, 1, { ok: true, value: 1n });
                    } catch (caught) {
                        error = (caught as Error).message;
                    }
                    recorded.run.deliver(1, STARTED_AT, 1, { ok: true, value: "fine" });
                    return { error, value: (await recorded.run.result).value };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "leaves a call waiting when its answer cannot be serialized" } },
        ),
        await exactScenario(
            "start rejects invalid start times, seeds and duplicate script names",
            DETERMINISTIC,
            { invalid: ["startedAt -1", "seed 1.5", "tools a-b and a_b", "same name twice"] },
            [
                "startedAt must be a non-negative number of milliseconds since the epoch",
                "seed must be a 32-bit unsigned integer",
                "Tool script names must be unique",
                "Tool names must be unique",
            ],
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const errors: string[] = [];
                    for (const invoke of [
                        () => sandbox.start("", { startedAt: -1, seed: 1, onCall: () => {} }),
                        () => sandbox.start("", { startedAt: 0, seed: 1.5, onCall: () => {} }),
                        () => sandbox.start("", { startedAt: 0, seed: 1, tools: [{ name: "a-b" }, { name: "a_b" }], onCall: () => {} }),
                        () => sandbox.start("", { startedAt: 0, seed: 1, tools: [{ name: "x" }, { name: "x" }], onCall: () => {} }),
                    ]) {
                        try {
                            invoke();
                            errors.push("no error");
                        } catch (error) {
                            errors.push((error as Error).message);
                        }
                    }
                    return errors;
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/src/runtime/execution.ts", case: "start validation" } },
        ),
        await exactScenario(
            "a run-supplied jsName overrides the derived script name",
            DETERMINISTIC,
            { tools: [{ name: "my-tool", jsName: "renamed" }] },
            { value: ["function", ["renamed", "my-tool"], ["renamed"]] },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const recorded = startRecording(
                        sandbox,
                        "return [typeof tools.renamed, Object.keys(tools), ALL_TOOLS.map((tool) => tool.name)]",
                        { tools: [{ name: "my-tool", jsName: "renamed" }] },
                    );
                    return { value: (await recorded.run.result).value };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "uses the script names the run passes" } },
        ),
        await exactScenario(
            "globals count as issued calls and rejection properties reach the script Error",
            DETERMINISTIC,
            { globals: ["search (spread)"], answer: "error with structuredOutput" },
            {
                firstCall: { id: 1, target: "global", name: "search", args: ["x", 2] },
                secondCall: { id: 2, target: "tool", name: "echo", args: "fail" },
                idles: [[0, 1], [1, 2]],
                calls: [{ name: "echo", status: "error", durationPresent: true }],
                output: [{ type: "text", text: "no" }],
                value: ["found"],
            },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const recorded = startRecording(
                        sandbox,
                        'const found = await search("x", 2); try { await tools.echo("fail"); } catch (error) { text(error.message); } return found;',
                        {
                            globals: [{ name: "search", spread: true }],
                            answer: (call: any) =>
                                call.args === "fail" ? { ok: false, message: "no" } : { ok: true, value: call.name === "search" ? ["found"] : call.args },
                        },
                    );
                    const result = normalizeCalls(await recorded.run.result) as any;
                    return {
                        firstCall: recorded.calls[0],
                        secondCall: recorded.calls[1],
                        idles: recorded.idles.slice(0, 2),
                        calls: result.calls,
                        output: result.output,
                        value: result.value,
                    };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "numbers calls to globals with tools, and answers errors as rejections" } },
        ),
        await goldenScenario(
            "execute() is not deterministic while start() is",
            DETERMINISTIC,
            { executes: 2, starts: 2 },
            async () => {
                const sandbox = new CodemodeSandbox({ wasm: WASM });
                try {
                    const before = Date.now();
                    const first = await sandbox.execute("return [Date.now(), Math.random()]");
                    const second = await sandbox.execute("return [Date.now(), Math.random()]");
                    const after = Date.now();
                    return {
                        nowWithinRun: (first as any).value[0] >= before && (first as any).value[0] <= after,
                        randomDiffers: (first as any).value[1] !== (second as any).value[1],
                    };
                } finally {
                    await sandbox.close();
                }
            },
            (observed) => {
                assert(observed.nowWithinRun === true, "execute reads the real wall clock");
                assert(observed.randomDiffers === true, "execute reads real randomness");
            },
            { upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "execute() is not deterministic" } },
        ),
    ];
}

const INLINE = "packages/codemode/src/runtime/inline.ts:InlineCodemodeSandbox,inlineLauncher";

/** Inline execution: interrupt-budget behavior, caller abort and deadlines while a tool waits. */
export async function inlineScenarios(): Promise<Scenario[]> {
    return [
        await exactScenario(
            "an inline script that computes without awaiting is stopped by its interrupt budget, not the deadline",
            INLINE,
            { interruptBudget: 1000, scripts: ["while (true) {}", "while (true) await null"], timeoutMs: [null, 50] },
            [
                { kind: "timeout", message: "Execution exceeded its interrupt budget of 1000" },
                { kind: "timeout", message: "Execution exceeded its interrupt budget of 1000" },
                { kind: "timeout", message: "Execution exceeded its interrupt budget of 1000" },
            ],
            async () => {
                const sandbox = new InlineCodemodeSandbox({ wasm: WASM, interruptBudget: 1000 });
                try {
                    const results = [];
                    results.push(await sandbox.execute("while (true) {}", { timeoutMs: Number.POSITIVE_INFINITY }));
                    results.push(await sandbox.execute("while (true) await null", { timeoutMs: Number.POSITIVE_INFINITY }));
                    results.push(await sandbox.execute("while (true) {}", { timeoutMs: 50 }));
                    return results.map((result) => result.error);
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/portable.test.ts", case: "fails a script that computes past its interrupt budget as a timeout" } },
        ),
        await goldenScenario(
            "the interrupt budget counts across awaits on tools",
            INLINE,
            { interruptBudget: 1000, loop: "1e5 iterations then await tools.tick()" },
            async () => {
                const sandbox = new InlineCodemodeSandbox({
                    wasm: WASM,
                    interruptBudget: 1000,
                    tools: [{ name: "tick", execute: () => 1 }],
                });
                try {
                    const result = await sandbox.execute("for (;;) { for (let i = 0; i < 1e5; i++); await tools.tick(); }");
                    return { error: result.error, calls: result.calls.length };
                } finally {
                    await sandbox.close();
                }
            },
            (observed) => {
                assert(observed.error?.kind === "timeout", "the shared budget produces a timeout");
                assert(observed.error?.message === "Execution exceeded its interrupt budget of 1000", "the budget message is exact");
                assert(observed.calls > 1, "the script waited on more than one tool before the budget was exhausted");
            },
            { upstreamTest: { path: "packages/codemode/test/portable.test.ts", case: "counts the budget across waits on tools" } },
        ),
        await exactScenario(
            "caller abort during an inline tool wait aborts the tool and keeps partial output",
            INLINE,
            { cancellation: "AbortController while tool waits" },
            {
                ok: false,
                error: { kind: "aborted", message: "cancelled" },
                output: [{ type: "text", text: "partial" }],
                calls: [{ name: "hang", status: "cancelled", durationPresent: true }],
            },
            async () => {
                let started: () => void = () => {};
                const called = new Promise<void>((resolve) => {
                    started = resolve;
                });
                const sandbox = new InlineCodemodeSandbox({
                    wasm: WASM,
                    interruptBudget: 10000,
                    tools: [
                        {
                            name: "hang",
                            execute: (_args: unknown, context: { signal: AbortSignal }) => {
                                started();
                                return new Promise((_resolve, reject) => {
                                    context.signal.addEventListener("abort", () => reject(new Error("tool aborted")));
                                });
                            },
                        },
                    ],
                });
                try {
                    const controller = new AbortController();
                    const pending = sandbox.execute("text('partial'); await tools.hang()", { signal: controller.signal });
                    await called;
                    controller.abort(new Error("cancelled"));
                    return normalizeCalls(await pending);
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/src/runtime/inline.ts", case: "abort signal while a tool waits" } },
        ),
        await exactScenario(
            "an inline deadline while a tool waits reports the deadline, not the interrupt budget",
            INLINE,
            { timeoutMs: 40, tool: "hang" },
            {
                ok: false,
                error: { kind: "timeout", message: "Execution timed out after 40 ms" },
                output: [{ type: "text", text: "partial" }],
                calls: [{ name: "hang", status: "cancelled", durationPresent: true }],
            },
            async () => {
                let started: () => void = () => {};
                const called = new Promise<void>((resolve) => {
                    started = resolve;
                });
                const sandbox = new InlineCodemodeSandbox({
                    wasm: WASM,
                    interruptBudget: 10000,
                    tools: [
                        {
                            name: "hang",
                            execute: (_args: unknown, context: { signal: AbortSignal }) => {
                                started();
                                return new Promise((_resolve, reject) => {
                                    context.signal.addEventListener("abort", () => reject(new Error("tool aborted")));
                                });
                            },
                        },
                    ],
                });
                try {
                    const pending = sandbox.execute("text('partial'); await tools.hang()", { timeoutMs: 40 });
                    await called;
                    return normalizeCalls(await pending);
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/src/runtime/inline.ts", case: "deadline while a tool waits" } },
        ),
        await exactScenario(
            "an inline execution with an already-aborted signal never creates a VM",
            INLINE,
            { cancellation: "pre-aborted AbortSignal" },
            { ok: false, error: { kind: "aborted", message: "early" }, output: [], calls: [] },
            async () => {
                const sandbox = new InlineCodemodeSandbox({ wasm: WASM, interruptBudget: 1000 });
                try {
                    const controller = new AbortController();
                    controller.abort(new Error("early"));
                    return normalizeCalls(await sandbox.execute("return 1", { signal: controller.signal }));
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/src/runtime/execution.ts", case: "signal already aborted" } },
        ),
        await exactScenario(
            "a successful inline execution returns its value and text output",
            INLINE,
            { script: "text('inline'); return 7", interruptBudget: 1000 },
            { ok: true, value: 7, output: [{ type: "text", text: "inline" }], calls: [], storeWrites: { set: {}, delete: [] } },
            async () => {
                const sandbox = new InlineCodemodeSandbox({ wasm: WASM, interruptBudget: 1000 });
                try {
                    return normalizeCalls(await sandbox.execute("text('inline'); return 7"));
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/portable.test.ts", case: "InlineCodemodeSandbox" } },
        ),
    ];
}

const REMOTE = "packages/codemode/src/runtime/remote.ts:RemoteCodemodeSandbox,remoteLauncher,serveCodemodeRemote";

interface RecordingRemote {
    remote: { execute(request: any, exchange: any): Promise<void> };
    exchanges: string[][];
    replies: string[];
    served: Promise<void>[];
}

function recordingRemote(options: Record<string, unknown> = {}): RecordingRemote {
    const exchanges: string[][] = [];
    const replies: string[] = [];
    const served: Promise<void>[] = [];
    return {
        remote: {
            execute(request: any, exchange: any) {
                const done = serveCodemodeRemote(
                    structuredClone(request),
                    async (messages: any[]) => {
                        exchanges.push(messages.map((message) => (message.type === "call" ? `call:${message.name}` : message.type)));
                        const reply = await exchange(structuredClone(messages));
                        replies.push(reply === null ? "null" : reply.type === "result" ? `result:${reply.ok}` : reply.type);
                        return structuredClone(reply);
                    },
                    { wasm: WASM, ...options },
                );
                served.push(done);
                return done;
            },
        },
        exchanges,
        replies,
        served,
    };
}

/** Generic Remote exchange: transport trace, caller-side tools, caps, stop and failure. */
export async function remoteScenarios(): Promise<Scenario[]> {
    return [
        await exactScenario(
            "a generic remote runs the script and calls the caller's tools across the exchange",
            REMOTE,
            { script: "text('hi'); return await tools.add({a: 1})", store: { k: 1 } },
            {
                ok: true,
                value: 2,
                output: [{ type: "text", text: "hi" }],
                calls: [{ name: "add", status: "ok", durationPresent: true }],
                storeWrites: { set: {}, delete: [] },
                seen: [{ a: 1 }],
                exchanges: [["output", "call:add"], ["done"]],
                replies: ["result:true", "null"],
            },
            async () => {
                const recording = recordingRemote();
                const seen: unknown[] = [];
                const sandbox = new RemoteCodemodeSandbox({
                    remote: () => recording.remote,
                    tools: [
                        {
                            name: "add",
                            execute: (args: any) => {
                                seen.push(args);
                                return args.a + 1;
                            },
                        },
                    ],
                });
                try {
                    const result = normalizeCalls(await sandbox.execute("text('hi'); return await tools.add({a: 1})", { store: { k: 1 } }));
                    await Promise.all(recording.served);
                    return { ...result, seen, exchanges: recording.exchanges, replies: recording.replies };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/portable.test.ts", case: "runs tools in the caller and the script on the remote" } },
        ),
        await goldenScenario(
            "remote output is buffered instead of one exchange per output item",
            REMOTE,
            { outputItems: 5000 },
            async () => {
                const recording = recordingRemote();
                const sandbox = new RemoteCodemodeSandbox({ remote: () => recording.remote });
                try {
                    const result = await sandbox.execute("for (let i = 0; i < 5000; i++) console.log(i); return 'ok'");
                    await Promise.all(recording.served);
                    return {
                        ok: result.ok,
                        outputCount: result.output.length,
                        last: result.output.at(-1),
                        exchanges: recording.exchanges.length,
                        exchangeSizes: recording.exchanges.map((exchange) => exchange.length),
                    };
                } finally {
                    await sandbox.close();
                }
            },
            (observed) => {
                assert(observed.ok === true && observed.outputCount === 5000, "all console output arrives");
                assert(observed.last.console === true && observed.last.text === "4999", "the last item is console output");
                assert(observed.exchanges <= 6, "output is batched, not one exchange per line");
            },
            { upstreamTest: { path: "packages/codemode/test/portable.test.ts", case: "batches output instead of making one exchange per line" } },
        ),
        await exactScenario(
            "the remote caps the requested interrupt budget and memory limit",
            REMOTE,
            { maxInterruptBudget: 1000, maxMemoryLimitBytes: 8388608, requestedInterruptBudget: 1000000000 },
            {
                timeout: { kind: "timeout", message: "Execution exceeded its interrupt budget of 1000" },
                memory: { kind: "script", name: "InternalError", message: "out of memory" },
            },
            async () => {
                const recording = recordingRemote({ maxInterruptBudget: 1000, maxMemoryLimitBytes: 8 * 1024 * 1024 });
                const sandbox = new RemoteCodemodeSandbox({ remote: () => recording.remote, interruptBudget: 1e9 });
                try {
                    const timeout = await sandbox.execute("while (true) {}");
                    const memory = await sandbox.execute("const parts = []; for (;;) parts.push('x'.repeat(1 << 20) + parts.length);");
                    await Promise.all(recording.served);
                    return {
                        timeout: { kind: timeout.error?.kind, message: timeout.error?.message },
                        memory: { kind: memory.error?.kind, name: memory.error?.name, message: memory.error?.message },
                    };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/portable.test.ts", case: "caps the interrupt budget and memory at the remote's limits" } },
        ),
        await exactScenario(
            "caller abort answers the remote's exchange with stop and settles without waiting for remote compute",
            REMOTE,
            { cancellation: "AbortController while tool hangs" },
            {
                ok: false,
                error: { kind: "aborted", message: "This operation was aborted" },
                output: [],
                calls: [{ name: "hang", status: "cancelled", durationPresent: true }],
                exchanges: [["call:hang"]],
                replies: ["stop"],
            },
            async () => {
                const recording = recordingRemote();
                let started: () => void = () => {};
                const called = new Promise<void>((resolve) => {
                    started = resolve;
                });
                const sandbox = new RemoteCodemodeSandbox({
                    remote: () => recording.remote,
                    tools: [
                        {
                            name: "hang",
                            execute: () => {
                                started();
                                return new Promise(() => {});
                            },
                        },
                    ],
                });
                try {
                    const controller = new AbortController();
                    const pending = sandbox.execute("await tools.hang(); return 'never'", { signal: controller.signal });
                    await called;
                    controller.abort();
                    const result = normalizeCalls(await pending);
                    await Promise.all(recording.served);
                    return { ...result, exchanges: recording.exchanges, replies: recording.replies };
                } finally {
                    await sandbox.close();
                }
            },
            { upstreamTest: { path: "packages/codemode/test/portable.test.ts", case: "tells the remote to stop when the caller aborts" } },
        ),
        await exactScenario(
            "a failing, silent or unreachable remote is a sandbox error with the transport reason",
            REMOTE,
            { remotes: ["reject", "silent", "throw"] },
            [
                { kind: "sandbox", message: "Remote sandbox failed: object reset" },
                { kind: "sandbox", message: "Remote sandbox returned before the script settled" },
                { kind: "sandbox", message: "Failed to reach the remote sandbox: no binding" },
            ],
            async () => {
                const results = [];
                for (const remote of [
                    () => ({ execute: () => Promise.reject(new Error("object reset")) }),
                    () => ({ execute: () => Promise.resolve() }),
                    () => {
                        throw new Error("no binding");
                    },
                ]) {
                    const sandbox = new RemoteCodemodeSandbox({ remote: remote as any });
                    try {
                        results.push((await sandbox.execute("return 1")).error);
                    } finally {
                        await sandbox.close();
                    }
                }
                return results;
            },
            { upstreamTest: { path: "packages/codemode/test/portable.test.ts", case: "reports a failing or vanishing remote as a sandbox error" } },
        ),
        await exactScenario(
            "a deterministic start over a transport that reorders replies still applies results in seq order",
            REMOTE,
            { startedAt: STARTED_AT, seed: 1, transportDelayMs: 30 },
            {
                value: ["a", "b"],
                idles: [[0, 2], [1, 2]],
                exchanges: [["started", "call:wait"], ["call:wait"], ["idle"], ["idle"], ["done"]],
            },
            async () => {
                const recording = recordingRemote();
                const delayed: RecordingRemote = {
                    ...recording,
                    remote: {
                        execute(request: any, exchange: any) {
                            const served = serveCodemodeRemote(
                                structuredClone(request),
                                async (messages: any[]) => {
                                    recording.exchanges.push(messages.map((message) => (message.type === "call" ? `call:${message.name}` : message.type)));
                                    const reply = await exchange(structuredClone(messages));
                                    if (reply?.type === "result" && reply.seq === 1) {
                                        await new Promise((resolve) => setTimeout(resolve, 30));
                                    }
                                    return structuredClone(reply);
                                },
                                { wasm: WASM, maxInterruptBudget: 50000 },
                            );
                            recording.served.push(served);
                            return served;
                        },
                    },
                };
                const sandbox = new RemoteCodemodeSandbox({ remote: () => delayed.remote, interruptBudget: 50000 });
                try {
                    const calls: any[] = [];
                    const idles: Array<[number, number]> = [];
                    const code =
                        'const order = []; await Promise.all([tools.wait("a").then(() => order.push("a")), tools.wait("b").then(() => order.push("b"))]); return order;';
                    const run = sandbox.start(code, {
                        startedAt: STARTED_AT,
                        seed: 1,
                        tools: [{ name: "wait" }],
                        timeoutMs: 20_000,
                        onStarted: () => {},
                        onCall: (call: any) => {
                            calls.push(call);
                        },
                        onIdle: (seq: number, issued: number) => {
                            idles.push([seq, issued]);
                        },
                    });
                    await waitFor(() => calls.length === 2);
                    const [a, b] = calls;
                    run.deliver(1, STARTED_AT + 1000, a.id, { ok: true, value: null });
                    run.deliver(2, STARTED_AT + 2000, b.id, { ok: true, value: null });
                    const result = await run.result;
                    await Promise.all(recording.served);
                    return { value: result.value, idles, exchanges: recording.exchanges };
                } finally {
                    await sandbox.close();
                }
            },
            {
                normalizations: ["the VM fingerprint is recorded in the deterministic artifact; this remote scenario compares ordering only"],
                upstreamTest: { path: "packages/codemode/test/deterministic.test.ts", case: "applies results in seq order when the transport reorders replies sent in order" },
            },
        ),
    ];
}

const APPLICATION = "packages/coding-agent/src/extensions/codemode/tool.ts:createCodemodeDescription,toCodemodeDeclaration,getCodemodeCallableTools,createCodemodeToolDefinition";

function applicationTool(name: string, description: string, outputSchema?: unknown, parameters: unknown = { type: "object", properties: { query: { type: "string" } } }) {
    return { name, description, parameters, ...(outputSchema === undefined ? {} : { outputSchema }) };
}

function fakeLoadout(entries: Array<{ tool: any; exposure: string; namespace?: unknown; guidelines?: string[] }>) {
    return {
        declared: entries.map((entry) => entry.tool),
        callable: entries.map((entry) => entry.tool),
        registered: entries.map((entry) => entry.tool),
        getExposure: (name: string) => entries.find((entry) => entry.tool.name === name)!.exposure,
        getNamespace: (name: string) => entries.find((entry) => entry.tool.name === name)!.namespace,
        getPromptGuidelines: (name: string) => entries.find((entry) => entry.tool.name === name)!.guidelines ?? [],
    };
}

/** Application consumer: dynamic description, catalog grouping, loadout modes and extension identity. */
export async function applicationScenarios(): Promise<Scenario[]> {
    return [
        await goldenScenario(
            "the empty codemode catalog still declares globals, discovery and the options grammar",
            APPLICATION,
            { tools: [] },
            () => createCodemodeDescription([]),
            (observed) => {
                assert(observed.startsWith("Run JavaScript that calls other tools."), "intro is present");
                assert(observed.includes("Globals:\n"), "globals section is present");
                assert(observed.includes("store(key, value)") && observed.includes("load(key)"), "store/load are declared");
                assert(observed.includes("ALL_TOOLS") && observed.includes("searchTools"), "discovery globals are declared");
                assert(observed.includes("Optional first line: `// @options:"), "options line is documented");
                assert(!observed.includes("Nested tools:"), "an empty catalog has no nested-tool section");
                assert(!observed.includes("`models`"), "models is absent unless enabled");
            },
            { upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/tool.ts", case: "createCodemodeDescription" } },
        ),
        await exactScenario(
            "a tool declaration defaults its output schema to string and appends prompt guidelines",
            APPLICATION,
            { tool: "read", guidelines: ["Read before editing."] },
            {
                name: "read",
                description: "Read a file\n\n- Read before editing.",
                inputSchema: { type: "object", properties: { query: { type: "string" } } },
                outputSchema: { type: "string" },
            },
            () => toCodemodeDeclaration(applicationTool("read", "Read a file"), ["Read before editing.", "  "]),
            { upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/tool.ts", case: "toCodemodeDeclaration" } },
        ),
        await goldenScenario(
            "a listed tool section carries its declaration, output type and guidelines",
            APPLICATION,
            { tool: "mcp__docs__search", outputSchema: "object" },
            () => createCodemodeDescription([applicationTool("mcp__docs__search", "Search docs", { type: "object", properties: { results: { type: "array", items: { type: "string" } } }, required: ["results"] })], { guidelines: new Map([["mcp__docs__search", ["Search before reading."]]]) }),
            (observed) => {
                assert(observed.includes("Nested tools:"), "the catalog has a nested-tool section");
                assert(observed.includes("### `mcp__docs__search`"), "the identifier heading is present");
                assert(observed.includes("Search docs\n\n- Search before reading."), "the description and guideline are present");
                assert(observed.includes("codemode tool declaration:"), "the declaration block is present");
                assert(observed.includes("Promise<{ results: Array<string>; }>;"), "the output schema is rendered in the signature");
            },
            { upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/tool.ts", case: "renderToolSection" } },
        ),
        await goldenScenario(
            "namespaces group sections and the inline budget omits tools with an explicit marker",
            APPLICATION,
            { tools: ["read", "mcp__docs__search"], namespace: "docs", inlineBudget: 40 },
            () =>
                createCodemodeDescription(
                    [
                        applicationTool("read", "Read a file"),
                        applicationTool("mcp__docs__search", "Search docs"),
                    ],
                    {
                        namespaces: new Map([["mcp__docs__search", { name: "docs", description: "Documentation tools" }]]),
                        inlineBudget: 40,
                    },
                ),
            (observed) => {
                assert(observed.includes("## docs (tools not listed)\nDocumentation tools"), "the namespace heading, omission marker and description are present");
                assert(observed.includes("## docs (tools not listed)"), "a namespace whose tool does not fit is marked");
                assert(observed.includes("### `read`"), "a tool that fits the budget is listed");
                assert(!observed.includes("### `mcp__docs__search`"), "a tool that does not fit the budget is omitted");
            },
            { upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/tool.ts", case: "selectCatalog" } },
        ),
        await exactScenario(
            "deferred tools are never listed and do not change the description",
            APPLICATION,
            { tools: ["alpha", "beta"], deferred: ["beta"] },
            { deferredOmitted: true, sameAsSingleTool: true, emptyDeferredIsEmptyDescription: true },
            () => {
                const alpha = applicationTool("alpha", "Alpha");
                const beta = applicationTool("beta", "Beta");
                const withDeferred = createCodemodeDescription([alpha, beta], { deferred: new Set(["beta"]) });
                return {
                    deferredOmitted: !withDeferred.includes("### `beta`"),
                    sameAsSingleTool: withDeferred === createCodemodeDescription([alpha]),
                    emptyDeferredIsEmptyDescription: createCodemodeDescription([alpha], { deferred: new Set(["alpha"]) }) === createCodemodeDescription([]),
                };
            },
            { upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/tool.ts", case: "createCodemodeDescription deferred" } },
        ),
        await exactScenario(
            "the models declaration line is present only when models are enabled",
            APPLICATION,
            { models: [false, true] },
            { withoutModels: false, withModels: true, mentionsClassifiersAndImages: true },
            () => {
                const without = createCodemodeDescription([], { models: false });
                const withModels = createCodemodeDescription([], { models: true });
                return {
                    withoutModels: without.includes("`models`"),
                    withModels: withModels.includes("`models`"),
                    mentionsClassifiersAndImages: withModels.includes("classifiers and image generation"),
                };
            },
            {
                normalizations: ["the docs path in the models line is host-specific and is not part of this comparison"],
                upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/tool.ts", case: "describeGlobals" },
            },
        ),
        await exactScenario(
            "the callable tool set excludes codemode itself",
            APPLICATION,
            { tools: ["read", "codemode", "mcp__docs__search"] },
            ["read", "mcp__docs__search"],
            () =>
                getCodemodeCallableTools([
                    applicationTool("read", "Read"),
                    applicationTool("codemode", "Self"),
                    applicationTool("mcp__docs__search", "Search"),
                ]).map((tool: any) => tool.name),
            { upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/tool.ts", case: "getCodemodeCallableTools" } },
        ),
        await exactScenario(
            "the extension registers an inactive model-only codemode tool with the source grammar",
            "packages/coding-agent/src/extensions/codemode/index.ts:createCodemodeExtension",
            { models: false },
            {
                name: "codemode",
                label: "codemode",
                exposure: "model-only",
                defaultActive: false,
                grammarEqualsSource: true,
                snippet: "Run JavaScript that calls other tools",
                guidelines: ["Use codemode to batch independent tool calls (Promise.allSettled), chain them, or filter large output, instead of many separate calls."],
                inlineBudget: 3000,
            },
            () => {
                let registered: any;
                const pi = {
                    getSettings: () => ({}),
                    getAllTools: () => [],
                    appendEntry: () => {},
                    registerTool: (definition: any) => {
                        registered = definition;
                    },
                };
                createCodemodeExtension({ models: false })(pi as any);
                return {
                    name: registered.name,
                    label: registered.label,
                    exposure: registered.exposure,
                    defaultActive: registered.defaultActive,
                    grammarEqualsSource: registered.constrainedSampling?.variants?.openai_lark === CODEMODE_SOURCE_GRAMMAR,
                    snippet: registered.promptSnippet,
                    guidelines: registered.promptGuidelines,
                    inlineBudget: DEFAULT_CODEMODE_INLINE_BUDGET,
                };
            },
            { upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/index.ts", case: "createCodemodeExtension" } },
        ),
        await goldenScenario(
            "the on loadout lists only non-direct callable tools and adds script-call descriptions",
            APPLICATION,
            { mode: "on", tools: ["read (direct)", "mcp__docs__search (codemode)"] },
            () => {
                let registered: any;
                const pi = {
                    getSettings: () => ({ codemode: { mode: "on" } }),
                    getAllTools: () => [],
                    appendEntry: () => {},
                    registerTool: (definition: any) => {
                        registered = definition;
                    },
                };
                createCodemodeExtension({ models: false })(pi as any);
                const changes = registered.prepareLoadout(
                    fakeLoadout([
                        { tool: applicationTool("read", "Read a file"), exposure: "direct" },
                        { tool: applicationTool("mcp__docs__search", "Search docs", { type: "object", properties: { results: { type: "array", items: { type: "string" } } }, required: ["results"] }), exposure: "codemode", namespace: { name: "docs", description: "Documentation tools" }, guidelines: ["Use docs."] },
                    ]),
                );
                return {
                    hiddenDeclarations: changes.hiddenDeclarations,
                    codemodeDescription: changes.descriptions.codemode,
                    readDescription: changes.descriptions.read,
                    searchDescription: changes.descriptions.mcp__docs__search,
                };
            },
            (observed) => {
                assert(observed.hiddenDeclarations.length === 0, "on mode hides no declarations");
                assert(observed.readDescription === "Read a file\n\nCodemode: `tools.read(args)` resolves to a string.", "direct tool gets a script-call sentence");
                assert(observed.searchDescription.includes("resolves to `{ results }`"), "output object fields are described");
                assert(observed.codemodeDescription.includes("## docs"), "the non-direct tool is listed in the codemode catalog with its namespace");
                assert(!observed.codemodeDescription.includes("### `read`"), "the direct tool is not repeated in the codemode catalog");
            },
            { upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/tool.ts", case: "prepareCodemodeLoadout mode on" } },
        ),
        await goldenScenario(
            "the only loadout lists every callable tool and hides direct declarations",
            APPLICATION,
            { mode: "only", tools: ["read (direct)", "mcp__docs__search (codemode)"] },
            () => {
                let registered: any;
                const pi = {
                    getSettings: () => ({ codemode: { mode: "only" } }),
                    getAllTools: () => [],
                    appendEntry: () => {},
                    registerTool: (definition: any) => {
                        registered = definition;
                    },
                };
                createCodemodeExtension({ models: false })(pi as any);
                const changes = registered.prepareLoadout(
                    fakeLoadout([
                        { tool: applicationTool("read", "Read a file"), exposure: "direct" },
                        { tool: applicationTool("mcp__docs__search", "Search docs"), exposure: "codemode", namespace: { name: "docs" } },
                    ]),
                );
                return {
                    hiddenDeclarations: changes.hiddenDeclarations,
                    codemodeDescription: changes.descriptions.codemode,
                    descriptions: Object.keys(changes.descriptions).sort(),
                };
            },
            (observed) => {
                assert(JSON.stringify(observed.hiddenDeclarations) === '["read"]', "only mode hides the direct declaration");
                assert(observed.descriptions.join(",") === "codemode", "only mode emits no per-direct-tool description");
                assert(observed.codemodeDescription.includes("### `read`"), "only mode lists the direct tool in the codemode catalog");
                assert(observed.codemodeDescription.includes("### `mcp__docs__search`"), "only mode lists the non-direct tool too");
            },
            { upstreamTest: { path: "packages/coding-agent/src/extensions/codemode/tool.ts", case: "prepareCodemodeLoadout mode only" } },
        ),
    ];
}
