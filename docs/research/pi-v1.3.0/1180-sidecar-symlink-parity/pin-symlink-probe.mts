// Pin frozen pi-v1.3.0 (71023aec) JSONL symlink semantics for the durable
// task-sidecar ruling (owner decision 2026-11-05). Probes, per operation,
// which path components may be symlinks:
//   A. open/recovery: unreferenced symlink task sidecar
//   B. open/recovery: marker-referenced (live) symlink task sidecar
//   C. commit: live append through a final symlink; reopen after it
//   D. commit with fsync: true through a final symlink
//   E. commit: live-then-terminal batch through a final symlink (reclaim)
//   F. main.jsonl final symlink at open and at commit
//   G. store directory through a symlinked intermediate component
//   H. the store directory itself is a symlink
//   I. open/recovery: marker-referenced doc sidecar symlink (out-of-scope obs)
//   J. hard-link control (re-verify the existing pin)
import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, symlinkSync, linkSync, rmSync, renameSync, existsSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const checkout = '/home/lansy/Work/github/coding-agent/pi';
const { openNodeJsonlStorage } = await import(join(checkout, 'packages/durable/src/storage/jsonl/node.ts'));
const { BACKGROUND_CONTEXT: context } = await import(join(checkout, 'packages/chord/src/context/index.ts'));

const live = (extra = {}) => ({
    type: 'task',
    value: {
        id: 2,
        conversationId: 1,
        kind: 'work',
        version: 1,
        input: {},
        background: false,
        abortRequested: false,
        state: { status: 'pending', checkpoint: { phase: 'start' } },
        ...extra,
    },
});
const terminal = {
    type: 'task',
    value: {
        id: 2,
        conversationId: 1,
        kind: 'work',
        version: 1,
        input: {},
        background: false,
        abortRequested: false,
        state: { status: 'terminal', outcome: { status: 'completed' } },
    },
};
const openRead = async (dir, note) => {
    try {
        const s = await openNodeJsonlStorage(dir, context);
        const task = await s.task(2, context);
        console.log(note, 'opened', 'task2=', JSON.stringify(task));
        await s.close(context);
    } catch (error) {
        console.log(note, 'error', error.name, JSON.stringify(error.message));
    }
};
const bytes = (path) => (existsSync(path) ? JSON.stringify(readFileSync(path, 'utf8')) : '<absent>');

// A. Unreferenced symlink task sidecar at open.
{
    const tmp = mkdtempSync(join(tmpdir(), 'pin-A-'));
    try {
        const store = join(tmp, 'store');
        mkdirSync(store);
        const target = join(tmp, 'outside');
        writeFileSync(target, 'complete\npartial');
        symlinkSync(target, join(store, 'task-2.jsonl'));
        await openRead(store, 'A-unreferenced-symlink');
        console.log('A-unreferenced-symlink target', bytes(target));
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}

// B. Marker-referenced (live) symlink task sidecar at open; outside target
//    holds the exact sidecar bytes pi wrote before the swap.
for (const fill of ['original-bytes', 'garbage']) {
    const tmp = mkdtempSync(join(tmpdir(), 'pin-B-'));
    try {
        const store = join(tmp, 'store');
        const s = await openNodeJsonlStorage(store, context);
        await s.commit([live()], context);
        await s.close(context);
        const original = readFileSync(join(store, 'task-2.jsonl'), 'utf8');
        renameSync(join(store, 'task-2.jsonl'), join(tmp, 'kept'));
        const target = join(tmp, 'outside');
        writeFileSync(target, fill === 'original-bytes' ? original : 'complete\npartial');
        symlinkSync(target, join(store, 'task-2.jsonl'));
        await openRead(store, `B-referenced-symlink-${fill}`);
        console.log(`B-referenced-symlink-${fill} target`, bytes(target));
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}

// C. Live commit through a final symlink; then reopen.
{
    const tmp = mkdtempSync(join(tmpdir(), 'pin-C-'));
    try {
        const store = join(tmp, 'store');
        const s = await openNodeJsonlStorage(store, context);
        const target = join(tmp, 'outside');
        writeFileSync(target, 'preexisting\n');
        symlinkSync(target, join(store, 'task-2.jsonl'));
        try {
            console.log('C-commit-through-symlink seq', await s.commit([live()], context));
        } catch (error) {
            console.log('C-commit-through-symlink error', error.name, JSON.stringify(error.message));
        }
        await s.close(context);
        console.log('C-commit-through-symlink target', bytes(target));
        await openRead(store, 'C-reopen');
        console.log('C-reopen target', bytes(target));
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}

// D. fsync:true commit through a final symlink.
{
    const tmp = mkdtempSync(join(tmpdir(), 'pin-D-'));
    try {
        const store = join(tmp, 'store');
        const s = await openNodeJsonlStorage(store, context, { fsync: true });
        const target = join(tmp, 'outside');
        writeFileSync(target, '');
        symlinkSync(target, join(store, 'task-2.jsonl'));
        try {
            console.log('D-fsync-commit-through-symlink seq', await s.commit([live()], context));
        } catch (error) {
            console.log('D-fsync-commit-through-symlink error', error.name, JSON.stringify(error.message));
        }
        await s.close(context);
        console.log('D-fsync-commit-through-symlink target', bytes(target));
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}

// E. Live-then-terminal batch through a final symlink: reclamation should
//    remove the entry; the outside target keeps the orphan sidecar bytes.
{
    const tmp = mkdtempSync(join(tmpdir(), 'pin-E-'));
    try {
        const store = join(tmp, 'store');
        const s = await openNodeJsonlStorage(store, context);
        const target = join(tmp, 'outside');
        writeFileSync(target, '');
        symlinkSync(target, join(store, 'task-2.jsonl'));
        try {
            console.log('E-batch-through-symlink seq', await s.commit([live(), terminal], context));
        } catch (error) {
            console.log('E-batch-through-symlink error', error.name, JSON.stringify(error.message));
        }
        await s.close(context);
        console.log('E-batch-through-symlink entry-exists', existsSync(join(store, 'task-2.jsonl')));
        console.log('E-batch-through-symlink target', bytes(target));
        await openRead(store, 'E-reopen');
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}

// F. main.jsonl final symlink: reopen reads through; commit marker appends through.
{
    const tmp = mkdtempSync(join(tmpdir(), 'pin-F-'));
    try {
        const store = join(tmp, 'store');
        const s = await openNodeJsonlStorage(store, context);
        await s.commit([live()], context);
        await s.close(context);
        renameSync(join(store, 'main.jsonl'), join(store, 'main-real.jsonl'));
        symlinkSync(join(store, 'main-real.jsonl'), join(store, 'main.jsonl'));
        const before = readFileSync(join(store, 'main-real.jsonl'), 'utf8');
        const r = await openNodeJsonlStorage(store, context);
        console.log('F-main-symlink-reopen task2=', JSON.stringify(await r.task(2, context)));
        await r.commit([{ ...live(), value: { ...live().value, id: 3 } }], context);
        await r.close(context);
        const after = readFileSync(join(store, 'main-real.jsonl'), 'utf8');
        console.log('F-main-symlink marker-grew', after.length > before.length, 'link-intact', existsSync(join(store, 'main.jsonl')));
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}

// G. Store reached through a symlinked intermediate parent component.
{
    const tmp = mkdtempSync(join(tmpdir(), 'pin-G-'));
    try {
        const real = join(tmp, 'real', 'store');
        const s = await openNodeJsonlStorage(real, context);
        await s.commit([live()], context);
        await s.close(context);
        symlinkSync(join(tmp, 'real'), join(tmp, 'alias'));
        await openRead(join(tmp, 'alias', 'store'), 'G-intermediate-parent-symlink');
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}

// H. The store directory itself is a symlink.
{
    const tmp = mkdtempSync(join(tmpdir(), 'pin-H-'));
    try {
        const real = join(tmp, 'realstore');
        const s = await openNodeJsonlStorage(real, context);
        await s.commit([live()], context);
        await s.close(context);
        symlinkSync(real, join(tmp, 'storelink'));
        await openRead(join(tmp, 'storelink'), 'H-store-dir-symlink');
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}

// I. Marker-referenced doc sidecar replaced by a symlink (out-of-scope record).
{
    const tmp = mkdtempSync(join(tmpdir(), 'pin-I-'));
    try {
        const store = join(tmp, 'store');
        const s = await openNodeJsonlStorage(store, context);
        await s.commit([
            {
                type: 'document.create',
                record: {
                    id: 3,
                    kind: 'note',
                    scope: { kind: 'session' },
                    history: 'latest',
                    fork: 'current',
                },
                content: { kind: 'base', version: 1, value: { text: 'hello' } },
            },
        ], context);
        await s.close(context);
        const original = readFileSync(join(store, 'doc-3.jsonl'), 'utf8');
        renameSync(join(store, 'doc-3.jsonl'), join(tmp, 'kept'));
        const target = join(tmp, 'outside');
        writeFileSync(target, original);
        symlinkSync(target, join(store, 'doc-3.jsonl'));
        try {
            const r = await openNodeJsonlStorage(store, context);
            const doc = await r.document(3, { kind: 'at', version: 0 }, context).catch((e) => ({ error: e.name }));
            console.log('I-doc-symlink opened doc=', JSON.stringify(doc));
            await r.close(context);
        } catch (error) {
            console.log('I-doc-symlink error', error.name, JSON.stringify(error.message));
        }
        console.log('I-doc-symlink target', bytes(target));
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}

// J. Hard-link control at open (re-verify the existing pin).
{
    const tmp = mkdtempSync(join(tmpdir(), 'pin-J-'));
    try {
        const store = join(tmp, 'store');
        mkdirSync(store);
        const target = join(tmp, 'outside');
        writeFileSync(target, 'complete\npartial');
        linkSync(target, join(store, 'task-2.jsonl'));
        await openRead(store, 'J-hardlink');
        console.log('J-hardlink target', bytes(target));
    } finally { rmSync(tmp, { recursive: true, force: true }); }
}
