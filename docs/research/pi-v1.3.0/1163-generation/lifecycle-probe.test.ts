import { writeFileSync } from 'node:fs';
import { applyImmutable } from '@earendil-works/chord/delta';
import { fauxAssistantMessage, fauxText, fauxToolCall } from '@earendil-works/pi-ai';
import { MemoryStorage, defineTool } from '@earendil-works/pi-durable';
import { Type } from '@earendil-works/pi-ai';
import { it, expect } from 'vitest';
import { chatSetup, openChat } from './chat-support.ts';
import { addTool } from './harness-support.ts';
import { context } from './session-support.ts';

const EchoParameters = Type.Object({ text: Type.Optional(Type.String()) });

async function observe(name: string, setup: any, script: (root: any, harness: any) => Promise<void>) {
 const storage = new MemoryStorage();
 const { harness, root } = await openChat(storage, setup);
 const watch = await root.watch(context);
 let replay = watch.value;
 const frames: any[] = [];
 watch.start(async (value: any, ops: any) => { replay = applyImmutable(replay, ops); expect(replay).toEqual(value); frames.push(ops); });
 await script(root, harness);
 await new Promise(r => setTimeout(r, 10));
 const state = await root.viewState(context);
 const final = state.value;
 expect(replay).toEqual(final);
 const settled = (globalThis as any).__settled;
 (globalThis as any).__settled = undefined;
 writeFileSync(process.env.CCH_1163_CAPTURE!, JSON.stringify({ name, frames, finalLive: final.docs['pi.live'], usage: final.docs['pi.usage'], settled }, null, 2), { flag: 'wx' });
 state.dispose(); await watch.stop(); await harness.close(context);
}

it('aborted', async () => {
 const setup = chatSetup();
 const msg: any = fauxAssistantMessage([fauxText('partial')], { stopReason: 'aborted', errorMessage: 'provider aborted' });
 msg.usage = { input: 5, output: 1, cacheRead: 0, cacheWrite: 0, totalTokens: 6, cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
 setup.faux.setResponses([msg]);
 await observe('aborted', setup, async root => {
  const submit = await root.submit({ type: 'input', content: 'hi' }, context);
  const settled = await submit.wait(context);
  (globalThis as any).__settled = settled;
 });
});

it('retry', async () => {
 const setup = chatSetup();
 setup.settings.retry = { enabled: true, maxRetries: 2, baseDelayMs: 1, maxAgentDelayMs: 2 };
 const fail: any = fauxAssistantMessage([fauxText('')], { stopReason: 'error', errorMessage: 'overloaded' });
 fail.usage = { input: 3, output: 1, cacheRead: 0, cacheWrite: 0, totalTokens: 4, cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
 const ok: any = fauxAssistantMessage([fauxText('done')]);
 ok.usage = { input: 7, output: 2, cacheRead: 1, cacheWrite: 0, totalTokens: 10, cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
 setup.faux.setResponses([fail, ok]);
 await observe('retry', setup, async root => {
  const submit = await root.submit({ type: 'input', content: 'hi' }, context);
  const settled = await submit.wait(context);
  (globalThis as any).__settled = settled;
 });
});

it('tool', async () => {
 const setup = chatSetup();
 addTool(setup.registry, defineTool({ name: 'echo', description: 'echo', parameters: EchoParameters, execute: async (args: any) => ({ output: [{ type: 'text', text: args.text ?? 'ok' }] }) }));
 const call: any = fauxAssistantMessage([fauxToolCall('echo', { text: 'hi' }, { id: 'c1' })], { stopReason: 'toolUse' });
 call.usage = { input: 4, output: 1, cacheRead: 0, cacheWrite: 0, totalTokens: 5, cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
 const ok: any = fauxAssistantMessage([fauxText('done')]);
 ok.usage = { input: 6, output: 3, cacheRead: 0, cacheWrite: 2, totalTokens: 11, cost: { input: 0, output: 0, cacheRead: 0, cacheWrite: 0, total: 0 } };
 setup.faux.setResponses([call, ok]);
 await observe('tool', setup, async root => {
  const submit = await root.submit({ type: 'input', content: 'hi' }, context);
  const settled = await submit.wait(context);
  (globalThis as any).__settled = settled;
 });
});
