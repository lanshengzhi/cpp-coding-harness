// Independent frozen capture for #1180: two document creates in one commit,
// in a write order that distinguishes frozen pi's sidecar append/flush order
// from sorted-filename order. A Proxy spy records the public FileSystem
// operation order; no frozen source is modified.
import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const checkout = '/home/lansy/Work/github/coding-agent/pi';
const { NodeExecutionEnv } = await import(join(checkout, 'packages/durable/src/env/node.ts'));
const { JsonlStorage } = await import(join(checkout, 'packages/durable/src/storage/jsonl/storage.ts'));
const { BACKGROUND_CONTEXT: context } = await import(join(checkout, 'packages/chord/src/context/index.ts'));

const operations = [];
const makeSpy = (inner) =>
    new Proxy(inner, {
        get(target, prop, receiver) {
            const value = Reflect.get(target, prop, receiver);
            if (typeof value !== 'function') return value;
            if (prop === 'appendFile' || prop === 'flushFile' || prop === 'remove' || prop === 'writeFile' || prop === 'truncateFile') {
                return async (path, ...rest) => {
                    operations.push(`${prop} ${String(path).split('/').pop()}`);
                    return value.call(target, path, ...rest);
                };
            }
            return value.bind(target);
        },
    });

const docCreate = (id, text) => ({
    type: 'document.create',
    record: { id, kind: 'note', key: `k${id}`, scope: { kind: 'session' }, history: 'latest', fork: 'current' },
    content: { kind: 'base', version: 1, value: { text } },
});

for (const fsync of [false, true]) {
    const tmp = mkdtempSync(join(tmpdir(), 'pin-order-'));
    try {
        operations.length = 0;
        const fs = makeSpy(new NodeExecutionEnv({ cwd: tmp }));
        const storage = await JsonlStorage.open(join(tmp, 'store'), fs, context, { fsync });
        // Write order deliberately differs from sorted filename order.
        await storage.commit([docCreate(9, 'nine'), docCreate(3, 'three')], context);
        await storage.close(context);
        console.log(`fsync=${fsync} operations`, JSON.stringify(operations));
    } finally {
        rmSync(tmp, { recursive: true, force: true });
    }
}
