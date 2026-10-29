/**
 * `audit-1090` capture: durable image preparation and read outcomes against the private frozen
 * pi v1.3.0 checkout named by `PI_CHECKOUT`.
 *
 * The script refuses to write evidence unless the checkout is the exact baseline revision and its
 * tracked source is clean. It records the Photon processor's orientation/conversion metadata, the
 * public read tool's image content and diagnostics, the vision-absent note, the unsupported-image
 * results without a processor, and the frozen MIME-sniffing rejections.
 *
 * Run from the Pike repository:
 *   NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> \
 *     <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1090/capture-1090.mts
 */

import { execFileSync } from "node:child_process";
import { createHash } from "node:crypto";
import { mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, relative } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import { deflateSync } from "node:zlib";

const script = fileURLToPath(import.meta.url);
const repo = join(dirname(script), "../../..");
const checkout = process.env.PI_CHECKOUT!;
const baselines = JSON.parse(readFileSync(join(repo, "fixtures/pi-v1.3.0/baselines.json"), "utf8"));
const baseline = baselines.baselines[baselines.default];
const revision = execFileSync("git", ["rev-parse", "HEAD"], { cwd: checkout, encoding: "utf8" }).trim();
if (revision !== baseline.revision || execFileSync("git", ["status", "--porcelain"], { cwd: checkout, encoding: "utf8" }).trim()) {
    throw new Error("frozen checkout is not clean at baseline");
}
const load = (path: string) => import(pathToFileURL(join(checkout, path)).href);

const durable = await load("packages/durable/src/index.ts");
const envNode = await load("packages/durable/src/env/node.ts");
const tools = await load("packages/durable/src/tools/index.ts");
const image = await load("packages/durable/src/tools/image.ts");
const imagesNode = await load("packages/durable/src/images/node.ts");
const ai = await load("packages/ai/src/index.ts");
const chordContext = await load("packages/chord/src/context/index.ts");

const { Harness, MemoryStorage, createRegistry, defineExtension } = durable;
const { NodeExecutionEnv } = envNode;
const { createReadTool, DEFAULT_IMAGE_LIMITS } = tools;
const { detectSupportedImageMimeType, imageDimensions } = image;
const { createNodePhotonImages } = imagesNode;
const { PhotonImage } = await import(
    pathToFileURL(join(checkout, "node_modules/@silvia-odwyer/photon/photon_rs.js")).href
);
const { createModels, fauxProvider, fauxAssistantMessage, fauxText, fauxToolCall } = ai;
const { BACKGROUND_CONTEXT: context } = chordContext;

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

/** A valid 2x2, 24-bit BMP. */
function bmp(): Buffer {
    const bytes = Buffer.alloc(70);
    bytes.write("BM", 0, "ascii");
    bytes.writeUInt32LE(70, 2);
    bytes.writeUInt32LE(54, 10);
    bytes.writeUInt32LE(40, 14);
    bytes.writeUInt32LE(2, 18);
    bytes.writeUInt32LE(2, 22);
    bytes.writeUInt16LE(1, 26);
    bytes.writeUInt16LE(24, 28);
    bytes.writeUInt32LE(16, 34);
    bytes.set([0, 0, 255, 0, 255, 0, 255, 0, 0, 0, 0, 255, 0, 255, 0, 255], 54);
    return bytes;
}

const ORIENTED_JPEG = Buffer.from(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/4QAiRXhpZgAATU0AKgAAAAgAAQESAAMAAAABAAYAAAAAAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAAIABADAREAAhEBAxEB/8QAHwAAAQUBAQEBAQEAAAAAAAAAAAECAwQFBgcICQoL/8QAtRAAAgEDAwIEAwUFBAQAAAF9AQIDAAQRBRIhMUEGE1FhByJxFDKBkaEII0KxwRVS0fAkM2JyggkKFhcYGRolJicoKSo0NTY3ODk6Q0RFRkdISUpTVFVWV1hZWmNkZWZnaGlqc3R1dnd4eXqDhIWGh4iJipKTlJWWl5iZmqKjpKWmp6ipqrKztLW2t7i5usLDxMXGx8jJytLT1NXW19jZ2uHi4+Tl5ufo6erx8vP09fb3+Pn6/8QAHwEAAwEBAQEBAQEBAQAAAAAAAAECAwQFBgcICQoL/8QAtREAAgECBAQDBAcFBAQAAQJ3AAECAxEEBSExBhJBUQdhcRMiMoEIFEKRobHBCSMzUvAVYnLRChYkNOEl8RcYGRomJygpKjU2Nzg5OkNERUZHSElKU1RVVldYWVpjZGVmZ2hpanN0dXZ3eHl6goOEhYaHiImKkpOUlZaXmJmaoqOkpaanqKmqsrO0tba3uLm6wsPExcbHyMnK0tPU1dbX2Nna4uPk5ebn6Onq8vP09fb3+Pn6/9oADAMBAAIRAxEAPwD42/4Wr/1C/wDyY/8Asa/cP+JZf+px/wCUP/ux/Rv/ABMN/wBSr/yt/wDcj9af+Gdf+pg/8kv/ALZX+On+tH/Tn/yb/wC1P8//APiBf/Uy/wDKX/3U/9k=",
    "base64",
);
const ORIENTED_WEBP = Buffer.from(
    "UklGRnAAAABXRUJQVlA4WAoAAAAIAAAAAgAAAQAAVlA4IDAAAADQAQCdASoDAAIAAUAmJaACdLoB+AADsAD+8ut//NgVzXPv9//S4P0uD9Lg/9KQAABFWElGGgAAAE1NACoAAAAIAAEBEgADAAAAAQAGAAAAAAAA",
    "base64",
);
const TINY_PNG = Buffer.from(
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACAQMAAABIeJ9nAAAAIGNIUk0AAHomAACAhAAA+gAAAIDoAAB1MAAA6mAAADqYAAAXcJy6UTwAAAAGUExURf8AAP///0EdNBEAAAABYktHRAH/Ai3eAAAAB3RJTUUH6gEOADM5Ddoh/wAAAAxJREFUCNdjYGBgAAAABAABJzQnCgAAACV0RVh0ZGF0ZTpjcmVhdGUAMjAyNi0wMS0xNFQwMDo1MTo1NyswMDowMOnKzHgAAAAldEVYdGRhdGU6bW9kaWZ5ADIwMjYtMDEtMTRUMDA6NTE6NTcrMDA6MDCYl3TEAAAAKHRFWHRkYXRlOnRpbWVzdGFtcAAyMDI2LTAxLTE0VDAwOjUxOjU3KzAwOjAwz4JVGwAAAABJRU5ErkJggg==",
    "base64",
);
const TINY_JPEG = Buffer.from(
    "/9j/4AAQSkZJRgABAQAAAQABAAD/2wBDAAMCAgMCAgMDAwMEAwMEBQgFBQQEBQoHBwYIDAoMDAsKCwsNDhIQDQ4RDgsLEBYQERMUFRUVDA8XGBYUGBIUFRT/2wBDAQMEBAUEBQkFBQkUDQsNFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBQUFBT/wAARCAACAAIDAREAAhEBAxEB/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/xAAVAQEBAAAAAAAAAAAAAAAAAAAGCf/EABQRAQAAAAAAAAAAAAAAAAAAAAD/2gAMAwEAAhEDEQA/AD3VTB3/2Q==",
    "base64",
);
const TINY_GIF = Buffer.from("R0lGODlhAQABAIAAAAAAAP///ywAAAAAAQABAAACAUwAOw==", "base64");
const TINY_WEBP = Buffer.from("UklGRhwAAABXRUJQVlA4TA8AAAAvAUAAAAcQ/Y/+ByKi/wEA", "base64");
const alpha = (value: any) => (value === undefined ? null : value);

async function openChat(storage: any, options: Record<string, any> = {}): Promise<any> {
    const faux = fauxProvider(options.fauxOptions ?? {});
    const models = createModels();
    models.setProvider(faux.provider);
    const registry = options.registry ?? createRegistry();
    const harness = await Harness.open(
        storage,
        {
            models,
            registry,
            ...(options.env === undefined ? {} : { env: options.env }),
        },
        context,
    );
    const root = await harness.root(context, { agent: { model: { provider: "faux", modelId: "faux-1" } } });
    return { harness, root, faux, registry };
}

async function entriesOf(conversation: any): Promise<any[]> {
    const page = await conversation.entries({}, 1000, undefined, context);
    return [...page.items].reverse();
}

async function toolResult(chat: any): Promise<any> {
    const entries = await entriesOf(chat.root);
    return entries.filter((entry) => entry.kind === "pi.tool-result").map((entry) => entry.model![0])[0];
}

/** The RGBA pixels Photon decodes from an encoded image, as base64. */
function pixelBase64(encoded: Buffer): string {
    const image = PhotonImage.new_from_byteslice(encoded);
    try {
        return Buffer.from(image.get_raw_pixels()).toString("base64");
    } finally {
        image.free();
    }
}

/** One RGBA sample at each quadrant centre of a prepared image. */
function quadrantSamples(encoded: Buffer): Array<{ x: number; y: number; rgba: number[] }> {
    const image = PhotonImage.new_from_byteslice(encoded);
    try {
        const width = image.get_width();
        const height = image.get_height();
        const pixels = image.get_raw_pixels();
        const at = (x: number, y: number) => Array.from(pixels.subarray((y * width + x) * 4, (y * width + x) * 4 + 4));
        const xs = [Math.floor(width / 4), Math.floor((3 * width) / 4)];
        const ys = [Math.floor(height / 4), Math.floor((3 * height) / 4)];
        return ys.flatMap((y) => xs.map((x) => ({ x, y, rgba: at(x, y) })));
    } finally {
        image.free();
    }
}

const contentShape = (result: any) =>
    result.content.map((item: any) =>
        item.type === "image"
            ? {
                  type: item.type,
                  mimeType: item.mimeType,
                  data: item.data,
                  base64Length: item.data.length,
                  dimensions: imageDimensions(Buffer.from(item.data, "base64"), item.mimeType),
                  ...(item.mimeType === "image/png"
                      ? { pixelsBase64: pixelBase64(Buffer.from(item.data, "base64")) }
                      : {}),
              }
            : { type: item.type, text: item.text },
    );

async function readWithProcessorScenario(name: string, file: string, bytes: Buffer, limit: Record<string, number>) {
    const directory = mkdtempSync(join(tmpdir(), "pi-1090-"));
    try {
        writeFileSync(join(directory, file), bytes);
        const images = await createNodePhotonImages({ worker: false });
        const chat = await openChat(new MemoryStorage(), {
            env: () => new NodeExecutionEnv({ cwd: directory }),
            fauxOptions: {
                models: [{ id: "faux-1", input: ["text", "image"], inputLimits: { images: { resize: limit } } }],
            },
        });
        chat.registry.install(defineExtension({ name: "read-image", tools: [createReadTool({ images })] }));
        chat.faux.setResponses([fauxAssistantMessage([fauxToolCall("read", { path: file }, { id: "i1" })], { stopReason: "toolUse" }), fauxAssistantMessage([fauxText("seen")])]);
        await (await chat.root.submit({ type: "input", content: "read the image" }, context)).wait(context);
        const result = await toolResult(chat);
        const expected = { isError: result.isError, content: contentShape(result) };
        await chat.harness.close(context);
        return { name, source: "packages/durable/src/tools/read.ts:createReadTool,readImage", inputs: { file, limit }, expected };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

async function readWithoutProcessorScenario(name: string, file: string, bytes: Buffer) {
    const directory = mkdtempSync(join(tmpdir(), "pi-1090-"));
    try {
        writeFileSync(join(directory, file), bytes);
        const chat = await openChat(new MemoryStorage(), {
            env: () => new NodeExecutionEnv({ cwd: directory }),
            fauxOptions: { models: [{ id: "faux-1", input: ["text", "image"] }] },
        });
        chat.registry.install(defineExtension({ name: "read-plain", tools: [createReadTool()] }));
        chat.faux.setResponses([fauxAssistantMessage([fauxToolCall("read", { path: file }, { id: "i1" })], { stopReason: "toolUse" }), fauxAssistantMessage([fauxText("done")])]);
        await (await chat.root.submit({ type: "input", content: "read the image" }, context)).wait(context);
        const result = await toolResult(chat);
        const expected = { isError: result.isError, content: contentShape(result) };
        await chat.harness.close(context);
        return { name, source: "packages/durable/src/tools/read.ts:createReadTool,readImage;packages/durable/src/tools/image.ts:imageDimensions", inputs: { file, processor: "none" }, expected };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

async function visionAbsentScenario() {
    const directory = mkdtempSync(join(tmpdir(), "pi-1090-"));
    try {
        const bytes = TINY_PNG;
        writeFileSync(join(directory, "plain.png"), bytes);
        const chat = await openChat(new MemoryStorage(), {
            env: () => new NodeExecutionEnv({ cwd: directory }),
            fauxOptions: { models: [{ id: "faux-1", input: ["text"] }] },
        });
        chat.registry.install(defineExtension({ name: "read-plain", tools: [createReadTool()] }));
        chat.faux.setResponses([fauxAssistantMessage([fauxToolCall("read", { path: "plain.png" }, { id: "v1" })], { stopReason: "toolUse" }), fauxAssistantMessage([fauxText("done")])]);
        await (await chat.root.submit({ type: "input", content: "read the image" }, context)).wait(context);
        const result = await toolResult(chat);
        const expected = {
            isError: result.isError,
            content: contentShape(result),
            originalBase64Length: Math.ceil(bytes.length / 3) * 4,
        };
        await chat.harness.close(context);
        return {
            name: "a model without image input gets the image and the frozen placeholder note",
            source: "packages/durable/src/tools/read.ts:readImage,imageModel",
            inputs: { file: "2x2 PNG", modelInput: ["text"], processor: "none" },
            expected,
        };
    } finally {
        rmSync(directory, { recursive: true, force: true });
    }
}

async function processorScenario() {
    const images = await createNodePhotonImages({ worker: false });
    const describe = async (bytes: Buffer, mimeType: string, limits: any) => {
        const prepared = await images.prepare(bytes, mimeType, limits);
        if (prepared === undefined) return null;
        const encoded = Buffer.from(prepared.data, "base64");
        return {
            mimeType: prepared.mimeType,
            convertedFrom: alpha(prepared.convertedFrom),
            resized: alpha(prepared.resized),
            dimensions: imageDimensions(encoded, prepared.mimeType),
            ...(mimeType === "image/bmp" ? { pixelsBase64: pixelBase64(encoded) } : {}),
            ...(mimeType === "image/jpeg" ? { quadrantSamples: quadrantSamples(encoded) } : {}),
        };
    };
    return {
        name: "the Photon processor orients JPEG and WebP, converts BMP, and reports its metadata",
        source: "packages/durable/src/images/index.ts:createPhotonImages;packages/durable/src/tools/image.ts:imageDimensions",
        inputs: { jpeg: "16x8 EXIF orientation 6", webp: "3x2 EXIF orientation 6", bmp: "2x2 24-bit" },
        expected: {
            jpeg: await describe(ORIENTED_JPEG, "image/jpeg", DEFAULT_IMAGE_LIMITS),
            webp: await describe(ORIENTED_WEBP, "image/webp", DEFAULT_IMAGE_LIMITS),
            bmp: await describe(bmp(), "image/bmp", DEFAULT_IMAGE_LIMITS),
        },
    };
}

function sniffScenario() {
    const source = Buffer.from(png(2, 2));
    const animationChunk = Buffer.alloc(12);
    animationChunk.write("acTL", 4, "ascii");
    const animated = Buffer.concat([source.subarray(0, 33), animationChunk, source.subarray(33)]);
    const rejectedJpeg = Buffer.from([0xff, 0xd8, 0xff, 0xf7, 0x00]);
    return {
        name: "frozen image sniffing rejects animated PNG and JPEG byte 3 0xf7",
        source: "packages/durable/src/tools/image.ts:detectSupportedImageMimeType",
        inputs: {
            animatedPng: "2x2 PNG with acTL before IDAT",
            jpeg: "SOI SOI 0xf7",
            staticPng: "2x2 RGBA PNG",
            tinyJpeg: "2x2 baseline JPEG",
            gif: "1x1 GIF89a",
            webp: "2x2 lossless WebP",
            bmp: "2x2 24-bit BMP",
        },
        expected: {
            animatedPng: alpha(detectSupportedImageMimeType(animated)),
            jpeg: alpha(detectSupportedImageMimeType(rejectedJpeg)),
            staticPng: alpha(detectSupportedImageMimeType(png(2, 2))),
            tinyJpeg: alpha(detectSupportedImageMimeType(TINY_JPEG)),
            gif: alpha(detectSupportedImageMimeType(TINY_GIF)),
            webp: alpha(detectSupportedImageMimeType(TINY_WEBP)),
            bmp: alpha(detectSupportedImageMimeType(bmp())),
            staticPngDimensions: alpha(imageDimensions(png(2, 2), "image/png")),
            tinyJpegDimensions: alpha(imageDimensions(TINY_JPEG, "image/jpeg")),
            gifDimensions: alpha(imageDimensions(TINY_GIF, "image/gif")),
            webpDimensions: alpha(imageDimensions(TINY_WEBP, "image/webp")),
            bmpDimensions: alpha(imageDimensions(bmp(), "image/bmp")),
        },
    };
}

const hugePng = Buffer.concat([png(2, 2), Buffer.alloc(3_670_016 - png(2, 2).length)]);
const scenarios = [
    await processorScenario(),
    await readWithProcessorScenario(
        "an EXIF-oriented JPEG is made upright before its resized diagnostic",
        "photo.jpg",
        ORIENTED_JPEG,
        { maxWidth: 1, maxHeight: 1, maxBytes: 1024 * 1024 },
    ),
    await readWithProcessorScenario("a BMP read through the processor converts to PNG and says so", "image.bmp", bmp(), { maxWidth: 2000, maxHeight: 2000, maxBytes: 4.5 * 1024 * 1024 }),
    await readWithProcessorScenario(
        "an image the processor cannot decode is an explicit error",
        "broken.png",
        png(2, 2).subarray(0, 20),
        DEFAULT_IMAGE_LIMITS,
    ),
    await visionAbsentScenario(),
    await readWithoutProcessorScenario(
        "an inline image within the limits passes through unchanged without a processor",
        "plain.png",
        TINY_PNG,
    ),
    await readWithoutProcessorScenario(
        "reading a BMP without an image processor is an explicit unsupported-image result",
        "image.bmp",
        bmp(),
    ),
    await readWithoutProcessorScenario(
        "an inline GIF passes through unchanged without a processor",
        "plain.gif",
        TINY_GIF,
    ),
    await readWithoutProcessorScenario("an inline image larger than the limits cannot be shrunk", "wide.png", png(2001, 2)),
    await readWithoutProcessorScenario(
        "an inline image above the byte limit cannot be shrunk",
        "huge.png",
        hugePng,
    ),
    await readWithoutProcessorScenario("an image whose header cannot be read is an explicit error", "truncated.png", png(2, 2).subarray(0, 20)),
    sniffScenario(),
];

const hash = (value: string | Buffer) => createHash("sha256").update(value).digest("hex");
const capturedAt = new Date().toISOString();
const report = process.report?.getReport() as any;
const environment = {
    platform: process.platform,
    arch: process.arch,
    libc: "glibc",
    glibcVersion: report.header.glibcVersionRuntime,
    node: process.version,
    tsx: execFileSync(join(checkout, "node_modules/.bin/tsx"), ["--version"], { encoding: "utf8" }).split("\n")[0].replace(/^tsx v?/, ""),
};
const endpoints = ["packages/durable/src/tools/read.ts:createReadTool,readImage,imageModel", "packages/durable/src/tools/image.ts:detectSupportedImageMimeType,imageDimensions", "packages/durable/src/images/index.ts:createPhotonImages"];
const base = { schema: "pike/pi-v1.3.0-audit-evidence/1", baseline: "pi-v1.3.0", revision, ticket: "1090", family: "audit-1090", capturedAt, environment };
const artifact = JSON.stringify({ ...base, sourceEndpoints: endpoints, scenarios }, null, 2) + "\n";
const folder = dirname(script);
writeFileSync(join(folder, "images.json"), artifact);
const sources = [{ path: relative(repo, script), sha256: hash(readFileSync(script)) }];
const manifest = {
    ...base,
    generator: {
        path: relative(repo, script),
        sha256: hash(readFileSync(script)),
        command: "NODE_OPTIONS=--conditions=source PI_CHECKOUT=<pi-checkout> <pi-checkout>/node_modules/.bin/tsx fixtures/pi-v1.3.0/audit-1090/capture-1090.mts",
        sources,
    },
    artifacts: [{ path: "images.json", family: "audit-1090", bytes: Buffer.byteLength(artifact), sha256: hash(artifact), sourceEndpoints: endpoints, scenarios }],
};
writeFileSync(join(folder, "manifest.json"), JSON.stringify(manifest, null, 2) + "\n");
