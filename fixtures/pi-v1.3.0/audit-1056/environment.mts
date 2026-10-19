/**
 * `audit-1056` Linux environment and integration surface: the real local ExecutionEnv, the built-in
 * coding tools over a real injected environment, Photon image preparation, and the read tool's
 * image-capability-absent outcomes.
 */

import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import path from "node:path";
import { deflateSync } from "node:zlib";
import {
    BACKGROUND_CONTEXT,
    DEFAULT_IMAGE_LIMITS,
    MemoryStorage,
    NodeExecutionEnv,
    Type,
    createCodingTools,
    createNodePhotonImages,
    createReadTool,
    defineExtension,
    defineTool,
} from "./pi.mts";
import { answer, closeChat, entriesOf, openChat, toolCall, toolResults, type Scenario } from "./support.mts";

function crc32(bytes: Uint8Array): number {
    let crc = 0xffffffff;
    for (const byte of bytes) {
        crc ^= byte;
        for (let bit = 0; bit < 8; bit++) crc = crc & 1 ? (crc >>> 1) ^ 0xedb88320 : crc >>> 1;
    }
    return (crc ^ 0xffffffff) >>> 0;
}

function chunk(type: string, data: Uint8Array): Buffer {
    const length = Buffer.alloc(4);
    length.writeUInt32BE(data.length);
    const typeBytes = Buffer.from(type, "ascii");
    const crc = Buffer.alloc(4);
    crc.writeUInt32BE(crc32(Buffer.concat([typeBytes, Buffer.from(data)])));
    return Buffer.concat([length, typeBytes, Buffer.from(data), crc]);
}

/** A deterministic opaque RGBA PNG of the requested size. */
function png(width: number, height: number): Buffer {
    const header = Buffer.alloc(13);
    header.writeUInt32BE(width, 0);
    header.writeUInt32BE(height, 4);
    header[8] = 8;
    header[9] = 6;
    const rows: Buffer[] = [];
    for (let y = 0; y < height; y++) {
        const row = Buffer.alloc(1 + width * 4);
        for (let x = 0; x < width; x++) {
            const at = 1 + x * 4;
            row[at] = (x * 7) % 256;
            row[at + 1] = (y * 11) % 256;
            row[at + 2] = 40;
            row[at + 3] = 255;
        }
        rows.push(row);
    }
    return Buffer.concat([
        Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
        chunk("IHDR", header),
        chunk("IDAT", deflateSync(Buffer.concat(rows))),
        chunk("IEND", Buffer.alloc(0)),
    ]);
}

/** A valid 1x1, 24-bit BMP header and pixel row. */
function bmp(): Buffer {
    const bytes = Buffer.alloc(58);
    bytes.write("BM", 0, "ascii");
    bytes.writeUInt32LE(58, 2);
    bytes.writeUInt32LE(54, 10);
    bytes.writeUInt32LE(40, 14);
    bytes.writeUInt32LE(1, 18);
    bytes.writeUInt32LE(1, 22);
    bytes.writeUInt16LE(1, 26);
    bytes.writeUInt16LE(24, 28);
    bytes.writeUInt32LE(0, 30);
    bytes.writeUInt32LE(4, 34);
    bytes[54] = 0;
    bytes[55] = 0;
    bytes[56] = 255;
    bytes[57] = 0;
    return bytes;
}

function unwrap<T>(result: { ok: true; value: T } | { ok: false; error: any }): T {
    if (!result.ok) throw new Error(`expected ok, got ${JSON.stringify(result.error)}`);
    return result.value;
}

function errorOf(result: { ok: boolean; error?: any }): Record<string, unknown> {
    return result.ok ? { ok: true } : { ok: false, code: result.error?.code, message: result.error?.message };
}

export async function nodeEnvironment(): Promise<Scenario> {
    const directory = mkdtempSync(path.join(tmpdir(), "pi-1056-env-"));
    try {
        const env = new NodeExecutionEnv({ cwd: directory });
        unwrap(await env.createDir("nested/child", { recursive: true }, BACKGROUND_CONTEXT));
        unwrap(await env.writeFile("nested/child/file.txt", "hel", BACKGROUND_CONTEXT));
        unwrap(await env.appendFile("nested/child/file.txt", "lo", BACKGROUND_CONTEXT));
        const text = unwrap(await env.readTextFile("nested/child/file.txt", BACKGROUND_CONTEXT));
        const lines = unwrap(await env.readTextLines("nested/child/file.txt", undefined, BACKGROUND_CONTEXT));
        const info = unwrap(await env.fileInfo("nested/child/file.txt", BACKGROUND_CONTEXT));
        const entries = unwrap(await env.listDir("nested/child", BACKGROUND_CONTEXT));
        const missing = await env.readTextFile("missing.txt", BACKGROUND_CONTEXT);
        const chunks: Array<{ stream: string; text: string }> = [];
        const shell = unwrap(
            await env.exec(
                "printf 'out\\n'; printf 'err\\n' >&2; exit 3",
                {
                    onOutput: (chunk: string, _context: any, info: any) => chunks.push({ stream: info.stream, text: chunk }),
                },
                BACKGROUND_CONTEXT,
            ),
        );
        const direct = unwrap(await env.exec(["printf", "direct"], undefined, BACKGROUND_CONTEXT));
        const timeout = await env.exec("sleep 5", { timeout: 0.1 }, BACKGROUND_CONTEXT);
        const absolute = unwrap(await env.absolutePath("nested/child/file.txt", BACKGROUND_CONTEXT));
        const expected = {
            absolutePathIsUnderRoot: absolute.startsWith(directory),
            absoluteBasename: path.basename(absolute),
            text,
            lines,
            file: { kind: info.kind, size: info.size },
            dir: entries.map((entry: any) => [entry.name, entry.kind]).sort(),
            missing: {
                ok: missing.ok,
                code: missing.ok ? null : missing.error.code,
                message: missing.ok ? null : missing.error.message.replace(directory, "<root>"),
            },
            shell: {
                exitCode: shell.exitCode,
                stdout: chunks.filter((chunk) => chunk.stream === "stdout").map((chunk) => chunk.text).join(""),
                stderr: chunks.filter((chunk) => chunk.stream === "stderr").map((chunk) => chunk.text).join(""),
            },
            direct: direct.exitCode,
            timeout: {
                ok: timeout.ok,
                code: timeout.ok ? null : timeout.error.code,
                message: timeout.ok ? null : timeout.error.message,
            },
            tempDirPrefix: path.basename(unwrap(await env.createTempDir("audit", BACKGROUND_CONTEXT))).startsWith("audit"),
        };
        await env.cleanup(BACKGROUND_CONTEXT);
        return {
            name: "the real Node ExecutionEnv reads, appends, lists, executes split streams and reports timeout",
            source: "packages/durable/src/env/node.ts:NodeExecutionEnv;packages/durable/src/env/index.ts:ExecutionEnv",
            inputs: { root: "temporary directory", shell: "printf stdout/stderr then exit 3", timeoutSeconds: 0.1 },
            expected,
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

export async function codingToolsWorkflow(): Promise<Scenario> {
    const directory = mkdtempSync(path.join(tmpdir(), "pi-1056-tools-"));
    try {
        const env = new NodeExecutionEnv({ cwd: directory });
        const chat = await openChat(new MemoryStorage(), {
            env: () => env,
            settings: { toolExecution: "sequential" },
        });
        chat.registry.install(createCodingTools());
        chat.faux.setResponses([
            toolCall("write", { path: "notes.txt", content: "alpha\n" }, "w1"),
            toolCall("read", { path: "notes.txt" }, "r1"),
            toolCall("edit", { path: "notes.txt", edits: [{ oldText: "alpha", newText: "omega" }] }, "e1"),
            toolCall("read", { path: "notes.txt" }, "r2"),
            toolCall("bash", { command: "cat notes.txt" }, "b1"),
            answer("tools done"),
        ]);
        const settled = await (
            await chat.root.submit({ type: "input", content: "exercise the coding tools" }, BACKGROUND_CONTEXT)
        ).wait(BACKGROUND_CONTEXT);
        const results = toolResults(await entriesOf(chat.root));
        const expected = {
            settled: settled.status,
            tools: results.map((result: any) => ({
                callId: result.toolCallId,
                toolName: result.toolName,
                isError: result.isError,
                text: result.content
                    .map((item: any) => (item.type === "text" ? item.text : `[${item.type}]`))
                    .join("|"),
            })),
            fileOnDisk: unwrap(await env.readTextFile("notes.txt", BACKGROUND_CONTEXT)),
        };
        await closeChat(chat);
        return {
            name: "the built-in read, write, edit and bash tools run over a real injected local environment",
            source: "packages/durable/src/tools/index.ts:createCodingTools;packages/durable/src/tools/bash.ts:createBashTool;packages/durable/src/harness/types.ts:ToolExecutionApi.env",
            inputs: {
                toolRound: ["write notes.txt", "read notes.txt", "edit alpha->omega", "read notes.txt", "bash cat notes.txt"],
                environment: "NodeExecutionEnv over a temporary directory",
            },
            expected,
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

export async function imageProcessor(): Promise<Scenario> {
    const images = await createNodePhotonImages({ worker: false });
    const source = png(40, 20);
    const asIs = await images.prepare(source, "image/png", DEFAULT_IMAGE_LIMITS);
    const shrunk = await images.prepare(source, "image/png", { maxWidth: 10, maxHeight: 10, maxBytes: 1024 * 1024 });
    const invalid = await images.prepare(new Uint8Array([1, 2, 3, 4]), "image/png", DEFAULT_IMAGE_LIMITS);
    const expected = {
        sourceBytes: source.length,
        asIs: {
            mimeType: asIs?.mimeType,
            base64Length: asIs?.data.length,
            sameAsSource: asIs?.data === Buffer.from(source).toString("base64"),
            resized: asIs?.resized ?? null,
            convertedFrom: asIs?.convertedFrom ?? null,
        },
        shrunk: {
            mimeType: shrunk?.mimeType,
            base64Length: shrunk?.data.length,
            resized: shrunk?.resized ?? null,
        },
        invalid: invalid ?? null,
    };
    return {
        name: "the Photon image processor passes an upright inline image through and resizes one over the limits",
        source: "packages/durable/src/images/node.ts:createNodePhotonImages;packages/durable/src/images/index.ts:createPhotonImages",
        inputs: { source: "40x20 RGBA PNG", limits: ["default 2000x2000", "10x10 with 1MiB base64"], invalidBytes: 4 },
        expected,
    };
}

export async function readImageWithProcessor(): Promise<Scenario> {
    const directory = mkdtempSync(path.join(tmpdir(), "pi-1056-image-"));
    try {
        const env = new NodeExecutionEnv({ cwd: directory });
        writeFileSync(path.join(directory, "photo.png"), png(40, 20));
        const images = await createNodePhotonImages({ worker: false });
        const chat = await openChat(new MemoryStorage(), {
            env: () => env,
            fauxOptions: {
                models: [
                    {
                        id: "faux-1",
                        input: ["text", "image"],
                        inputLimits: { images: { resize: { maxWidth: 10, maxHeight: 10, maxBytes: 1024 * 1024 } } },
                    },
                ],
            },
        });
        chat.registry.install(defineExtension({ name: "read-image", tools: [createReadTool({ images })] }));
        chat.faux.setResponses([toolCall("read", { path: "photo.png" }, "i1"), answer("seen")]);
        await (await chat.root.submit({ type: "input", content: "read the image" }, BACKGROUND_CONTEXT)).wait(
            BACKGROUND_CONTEXT,
        );
        const result = toolResults(await entriesOf(chat.root))[0];
        const expected = {
            isError: result.isError,
            content: result.content.map((item: any) =>
                item.type === "image"
                    ? { type: item.type, mimeType: item.mimeType, base64Length: item.data.length }
                    : { type: item.type, text: item.text },
            ),
            diagnostics: result.diagnostics ?? null,
        };
        await closeChat(chat);
        return {
            name: "the read tool returns a resized image content block with its coordinate-mapping diagnostic",
            source: "packages/durable/src/tools/read.ts:createReadTool,readImage;packages/durable/src/tools/image-processor.ts:ImageProcessor",
            inputs: { file: "40x20 PNG", modelImageLimit: "10x10", processor: "Photon, calling thread" },
            expected,
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

export async function readBmpWithoutProcessor(): Promise<Scenario> {
    const directory = mkdtempSync(path.join(tmpdir(), "pi-1056-bmp-"));
    try {
        const env = new NodeExecutionEnv({ cwd: directory });
        writeFileSync(path.join(directory, "image.bmp"), bmp());
        const chat = await openChat(new MemoryStorage(), { env: () => env });
        chat.registry.install(defineExtension({ name: "read-plain", tools: [createReadTool()] }));
        chat.faux.setResponses([toolCall("read", { path: "image.bmp" }, "b1"), answer("done")]);
        await (await chat.root.submit({ type: "input", content: "read bmp" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
        const result = toolResults(await entriesOf(chat.root))[0];
        const expected = {
            isError: result.isError,
            content: result.content,
            diagnostics: result.diagnostics ?? null,
        };
        await closeChat(chat);
        return {
            name: "reading a BMP without an image processor is an explicit unsupported-image result",
            source: "packages/durable/src/tools/read.ts:createReadTool,readImage;packages/durable/src/tools/image.ts:detectSupportedImageMimeType",
            inputs: { file: "1x1 24-bit BMP", processor: "none" },
            expected,
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

export async function readPngWithoutProcessor(): Promise<Scenario> {
    const directory = mkdtempSync(path.join(tmpdir(), "pi-1056-png-"));
    try {
        const env = new NodeExecutionEnv({ cwd: directory });
        const bytes = png(2, 2);
        writeFileSync(path.join(directory, "plain.png"), bytes);
        const chat = await openChat(new MemoryStorage(), {
            env: () => env,
            fauxOptions: { models: [{ id: "faux-1", input: ["text", "image"] }] },
        });
        chat.registry.install(defineExtension({ name: "read-plain-png", tools: [createReadTool()] }));
        chat.faux.setResponses([toolCall("read", { path: "plain.png" }, "p1"), answer("done")]);
        await (await chat.root.submit({ type: "input", content: "read png" }, BACKGROUND_CONTEXT)).wait(BACKGROUND_CONTEXT);
        const result = toolResults(await entriesOf(chat.root))[0];
        const expected = {
            isError: result.isError,
            content: result.content.map((item: any) =>
                item.type === "image"
                    ? { type: item.type, mimeType: item.mimeType, base64Length: item.data.length }
                    : { type: item.type, text: item.text },
            ),
            originalBase64Length: Math.ceil(bytes.length / 3) * 4,
            diagnostics: result.diagnostics ?? null,
        };
        await closeChat(chat);
        return {
            name: "an inline image within the limits passes through unchanged without a processor",
            source: "packages/durable/src/tools/read.ts:readImage;packages/durable/src/tools/image.ts:imageDimensions",
            inputs: { file: "2x2 PNG", processor: "none" },
            expected,
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

export async function allEnvironmentScenarios(): Promise<Scenario[]> {
    return [
        await nodeEnvironment(),
        await codingToolsWorkflow(),
        await imageProcessor(),
        await readImageWithProcessor(),
        await readBmpWithoutProcessor(),
        await readPngWithoutProcessor(),
    ];
}
