import { mkdtempSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
const checkout='/tmp/cch-1080-pi-frozen';
const {openNodeJsonlStorage}=await import(join(checkout,'packages/durable/src/storage/jsonl/node.ts'));
const {BACKGROUND_CONTEXT:ctx}=await import(join(checkout,'packages/chord/src/context/index.ts'));
const dir=mkdtempSync(join(tmpdir(),'1085-batch-'));
try {
 const s=await openNodeJsonlStorage(dir,ctx);
 const common={id:2,conversationId:1,kind:'work',version:1,input:{},background:false,abortRequested:false};
 try {
  console.log('commit',await s.commit([{type:'task',value:{...common,state:{status:'terminal',outcome:{status:'completed'}}}},{type:'task',value:{...common,state:{status:'pending',checkpoint:{phase:'again'}}}}],ctx));
  console.log('task',JSON.stringify(await s.task(2,ctx)));
 } catch(e){console.log('error',e.name,e.message)}
 await s.close(ctx);
 const r=await openNodeJsonlStorage(dir,ctx); console.log('reopen',JSON.stringify(await r.task(2,ctx))); await r.close(ctx);
} finally {rmSync(dir,{recursive:true,force:true})}
