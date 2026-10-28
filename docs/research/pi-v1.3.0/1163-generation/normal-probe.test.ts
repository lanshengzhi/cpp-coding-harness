import { writeFileSync } from 'node:fs';
import { applyImmutable } from '@earendil-works/chord/delta';
import { fauxAssistantMessage, fauxText } from '@earendil-works/pi-ai';
import { MemoryStorage } from '@earendil-works/pi-durable';
import { it, expect } from 'vitest';
import { chatSetup, openChat } from './chat-support.ts';
import { context } from './session-support.ts';

it('independently observes request and completion', async () => {
 const setup = chatSetup();
 let release!: () => void; let entered!: () => void;
 const blocked = new Promise<void>(r => release = r);
 const reached = new Promise<void>(r => entered = r);
 const msg = fauxAssistantMessage([fauxText('hello')]);
 setup.faux.setResponses([async () => { entered(); await blocked; return msg; }]);
 const { harness, root } = await openChat(new MemoryStorage(), setup);
 const watch = await root.watch(context); let replay = watch.value;
 const frames: any[] = [];
 watch.start(async (value, ops) => { replay = applyImmutable(replay, ops); expect(replay).toEqual(value); frames.push({value, ops}); });
 const submit = await root.submit({type:'input',content:'hi'}, context);
 await reached;
 const atStream = await root.viewState(context); const streamView = structuredClone(atStream.value); atStream.dispose();
 release(); await submit.wait(context);
 await new Promise(r => setTimeout(r, 10));
 const state = await root.viewState(context); const final = state.value;
 expect(replay).toEqual(final);
 writeFileSync(process.env.CCH_1163_CAPTURE!, JSON.stringify({frames: frames.map(f => f.ops), streamLive: streamView.docs['pi.live'], finalLive: final.docs['pi.live'], usage: final.docs['pi.usage']}, null, 2), {flag:'wx'});
 state.dispose(); await watch.stop(); await harness.close(context);
});
