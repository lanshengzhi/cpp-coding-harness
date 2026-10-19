#!/usr/bin/env tsx
/**
 * Program-status detection and override capture for #1050.
 *
 * Runs in its own process because `ProcessTerminal` owns `process.stdin`/`process.stdout`. The
 * harness replaces those host streams, then drives the public `start()`/`setProgramStatus()`/
 * `stop()` boundary and prints the observed writes as JSON. No private member is called.
 */

import path from "node:path";
import { pathToFileURL } from "node:url";

const piCheckout = process.env.PI_CHECKOUT ?? "";
const { ProcessTerminal } = await import(
    pathToFileURL(path.join(piCheckout, "packages/tui/src/terminal.ts")).href
);

type CaseResult = {
    name: string;
    queryWrite: string;
    programStatusWrites: string[];
    stopWrites: string[];
    inputs: string[];
};

function captureCase(
    name: string,
    override: string | undefined,
    replyBeforeDeviceAttributes: boolean,
): CaseResult {
    const previousOverride = process.env.PI_PROGRAM_STATUS;
    if (override === undefined) delete process.env.PI_PROGRAM_STATUS;
    else process.env.PI_PROGRAM_STATUS = override;

    const writes: string[] = [];
    const inputs: string[] = [];
    let dataHandler: ((data: string) => void) | undefined;
    const previousWrite = process.stdout.write;
    const previousOn = process.stdin.on;
    const terminal = new ProcessTerminal();
    try {
        process.stdout.write = ((chunk: string | Uint8Array) => {
            writes.push(String(chunk));
            return true;
        }) as typeof process.stdout.write;
        process.stdin.on = ((event: string | symbol, listener: (...args: unknown[]) => void) => {
            if (event === "data") dataHandler = listener as (data: string) => void;
            return process.stdin;
        }) as typeof process.stdin.on;
        terminal.start(
            (data: string) => inputs.push(data),
            () => {},
        );
        terminal.setProgramStatus({ state: "working", app: "pi" });
        const deviceAttributes = "\x1b[?62;4;52c";
        if (replyBeforeDeviceAttributes) {
            dataHandler?.("\x1b]7501;?\x1b\\");
            dataHandler?.(deviceAttributes);
        } else {
            dataHandler?.(deviceAttributes);
            dataHandler?.("\x1b]7501;?\x07");
        }
        terminal.setProgramStatus({ state: "blocked", kind: "permission", message: "Allow?" });
        const programStatusWrites = writes.filter((write) => write.startsWith("\x1b]7501;state="));
        const beforeStop = writes.length;
        terminal.stop();
        const stopWrites = writes.slice(beforeStop);
        return {
            name,
            queryWrite: writes.find((write) => write.includes("\x1b[?u")) ?? "",
            programStatusWrites,
            stopWrites,
            inputs: [...inputs],
        };
    } finally {
        process.stdout.write = previousWrite;
        process.stdin.on = previousOn;
        if (previousOverride === undefined) delete process.env.PI_PROGRAM_STATUS;
        else process.env.PI_PROGRAM_STATUS = previousOverride;
    }
}

const cases: CaseResult[] = [
    captureCase("default-reply-before-device-attributes", undefined, true),
    captureCase("default-device-attributes-before-reply", undefined, false),
    captureCase("override-enabled", "1", true),
    captureCase("override-disabled", "0", true),
];
process.stdout.write(`${JSON.stringify({ cases }, null, 2)}\n`);
process.exit(0);
