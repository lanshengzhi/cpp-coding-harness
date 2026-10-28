import { mkdtempSync, mkdirSync, writeFileSync, readFileSync, symlinkSync, linkSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
const checkout = '/tmp/cch-1080-pi-frozen';
const { openNodeJsonlStorage } = await import(join(checkout, 'packages/durable/src/storage/jsonl/node.ts'));
const { BACKGROUND_CONTEXT: context } = await import(join(checkout, 'packages/chord/src/context/index.ts'));
for (const mode of ['symlink', 'hardlink']) {
  const tmp = mkdtempSync(join(tmpdir(), '1085-link-probe-'));
  try {
    const store = join(tmp, 'store'); mkdirSync(store);
    const target = join(tmp, 'outside'); writeFileSync(target, 'complete\npartial');
    (mode === 'symlink' ? symlinkSync : linkSync)(target, join(store, 'task-2.jsonl'));
    try { const s = await openNodeJsonlStorage(store, context); await s.close(context); console.log(mode, 'opened'); }
    catch (error) { console.log(mode, 'error', (error as Error).name, (error as Error).message); }
    console.log(mode, 'target', JSON.stringify(readFileSync(target, 'utf8')));
  } finally { rmSync(tmp, { recursive: true, force: true }); }
}
