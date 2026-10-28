import { defineTask, MemoryStorage } from '/tmp/cch-1080-pi-frozen/packages/durable/src/index.ts';
import { openTasks, eventually } from '/tmp/cch-1080-pi-frozen/packages/durable/test/task-support.ts';
import { context } from '/tmp/cch-1080-pi-frozen/packages/durable/test/session-support.ts';

async function main() {
 const events: string[] = [];
 let pending: Promise<unknown> | undefined;
 let harness: any;
 const task = defineTask<null, {phase: 'run'}, null>({
  name: 'probe.order', version: 1, initial: () => ({phase: 'run'}),
  phases: { run: async (_task, runtime, ctx) => {
   events.push('phase');
   await runtime.commit(() => ({status:'waiting', checkpoint:{phase:'run'}, on:[], policy:'allSettled'}), ctx);
   events.push('waiting-committed');
   await new Promise<void>(resolve => runtime.signal.addEventListener('abort', () => resolve(), {once:true}));
  } }, abort: async () => {},
 });
 ({harness} = await openTasks(new MemoryStorage(), [task]));
 const root = await harness.root(context);
 const id = await root.commit((tx: any) => tx.createTask(task, null, {ownership: {kind: 'conversation'}}), context);
 harness.resume();
 await eventually(() => events.includes('waiting-committed'));
 const observation = await harness.inspect(context);
 const row = observation.tasks.find((entry: any) => entry.record.id === id);
 console.log('CCH1088_WAITING', JSON.stringify({status:row?.record.state.status, kind:row?.state.kind}));
 if (row?.record.state.status !== 'waiting' || row?.state.kind !== 'running') throw Error('unexpected state');
 await harness.close(context);
}
await main();
