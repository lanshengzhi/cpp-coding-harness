// Run with PI_CHECKOUT=<frozen checkout> bun --conditions=source
// fixtures/pi-v1.3.0/audit-1119/capture.mts
import { execFileSync } from 'node:child_process';
import { mkdtempSync, readFileSync, writeFileSync, rmSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';
import { createHash } from 'node:crypto';
const root = process.env.PI_CHECKOUT!;
const registry = JSON.parse(readFileSync(resolve('fixtures/pi-v1.3.0/baselines.json'),'utf8'));
const revision = registry.baselines['pi-v1.3.0'].revision;
if (!root || execFileSync('git', ['rev-parse', 'HEAD'], {cwd: root, encoding:'utf8'}).trim() !== revision)
    throw new Error('exact pi-v1.3.0 checkout required');
if (execFileSync('git', ['status', '--porcelain', '--untracked-files=no'], {cwd:root, encoding:'utf8'}).trim())
    throw new Error('tracked upstream sources must be clean');
const load = (p: string) => import(pathToFileURL(join(root,p)).href);
const {executeCodemode, readCodemodeStore} = await load('packages/coding-agent/src/extensions/codemode/execute.ts');
const {SessionManager} = await load('packages/coding-agent/src/core/session-manager.ts');
const {createWriteTool} = await load('packages/coding-agent/src/core/tools/write.ts');
const {createReadTool} = await load('packages/coding-agent/src/core/tools/read.ts');
const {createBashTool} = await load('packages/coding-agent/src/core/tools/bash.ts');
const workspace = mkdtempSync(join(tmpdir(),'pike-1119-'));
let manager = SessionManager.create(workspace,workspace);
manager.appendMessage({role:'user',content:[{type:'text',text:'save'}],timestamp:1});
const failTool = {name:'fail', label:'fail', description:'Fail with a fixed error.', parameters:{type:'object',properties:{}},
    execute: async () => { throw new Error('boom'); }};
const tools = [createWriteTool(workspace),createReadTool(workspace),createBashTool(workspace),failTool];
let counter = 0;
const ctx = {tools, get sessionManager(){return manager;}, executeTool:async(name:string,args:unknown,options:any)=>{
    const tool = tools.find(t=>t.name===name)!;
    const id = `call_1/${++counter}`;
    try {return {toolCall:{id,name,arguments:args},result:await tool.execute(id,args,options.signal),isError:false};}
    catch(e){return {toolCall:{id,name,arguments:args},result:{content:[{type:'text',text:(e as Error).message}]},isError:true};}
}};
const observations:any[]=[];
async function run(name:string,code:string){
    // Let the previous worker's exit notification release admission between scenarios.
    await new Promise(resolve => setTimeout(resolve, 100));
    counter=0;
    const result=await executeCodemode('call_1',{code},undefined,undefined,ctx,
        {appendEntry:(type:string,data:unknown)=>manager.appendCustomEntry(type,data)});
    const paths:string[]=[];
    for(const block of result.content){if(block.type==='text'){
        block.text=block.text.replace(/Wall time [\d.]+ seconds/g,'Wall time <elapsed> seconds')
          .replace(/\/[^\s\]]*pi-codemode[^\s\])]+/g,(p:string)=>{paths.push(p);return '<output-file>';});
    }}
    if(result.details?.fullOutputPath){result.fullText=readFileSync(result.details.fullOutputPath,'utf8');paths.push(result.details.fullOutputPath);result.details.fullOutputPath='<output-file>';}
    for(const call of result.details?.calls??[])if('durationMs' in call)call.durationMs='<elapsed>';
    const normalized = JSON.parse(JSON.stringify(result).replaceAll(workspace,'<workspace>'));
    if(JSON.stringify(normalized).includes('worker limit reached')) throw new Error('scenario exhausted worker admission');
    observations.push({name,inputs:{code},expected:{result:normalized,store:readCodemodeStore(manager.getBranch())},
        normalizations:['Wall time and present durationMs values become <elapsed>; presence is retained.',
            'Temporary output paths become <output-file>; full spilled bytes are captured separately.',
            'The unique temporary workspace becomes <workspace>.']});
    for(const p of new Set(paths))rmSync(p,{force:true});
}
try {
 await run('persist',"await tools.write({path: 'answer.txt', content: '42'}); store('answer', await tools.read({path: 'answer.txt'})); return load('answer');");
 manager=SessionManager.open(manager.getSessionFile());
 await run('resume-failure',"text(load('answer')); store('answer', 'wrong'); throw 'rollback';");
 await run('resume-load',"return load('answer');");
 await run('presentation',"console.log('log'); text('first'); text('second'); return 'last';");
 await run('prior-calls',"await tools.write({path:'before.txt',content:'kept'}); console.log('log'); text('before'); throw 'boom';");
 await run('budget',"// @options: {\"max_output_tokens\": 2}\ntext('abcdefghijklmnopqrstuvwxyz');");
 await run('unicode-budget',"// @options: {\"max_output_tokens\": 1}\ntext('猫'.repeat(12));");
 await run('zero-error-budget',"// @options: {\"max_output_tokens\": 0}\nthrow 'boom';");
 await run('nested-failure',"await tools.write({path:'before.txt',content:'kept'}); await tools.read({path:'missing.txt'});");
 await run('nested-failure-fixed',"await tools.write({path:'before.txt',content:'kept'}); await tools.fail({});");
 await run('timeout',"// @options: {\"timeout_ms\": 100}\nawait tools.write({path:'before.txt',content:'kept'}); text('before'); await tools.bash({command:'sleep 30'});");
 await run('missing-discovery',"return [typeof await describeTool('missing'), typeof await describeNamespace('missing')];");
 await run('image',"image('data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=');");
 await run('delete',"store('answer', undefined); return load('answer');");
 const sources=['packages/coding-agent/src/extensions/codemode/execute.ts','packages/coding-agent/src/core/session-manager.ts'];
 const sha = (data: string | Buffer) => createHash('sha256').update(data).digest('hex');
 const environment={node:process.version,bun:process.versions.bun,platform:process.platform,arch:process.arch,
     libc:'glibc',glibcVersion:execFileSync('getconf',['GNU_LIBC_VERSION'],{encoding:'utf8'}).trim().split(' ')[1]};
 const metadata={schema:'pike/pi-v1.3.0-audit-evidence/1',baseline:'pi-v1.3.0',revision,
     capturedAt:new Date().toISOString(),environment};
 const sourceEndpoints=['packages/coding-agent/src/extensions/codemode/execute.ts:executeCodemode,readCodemodeStore',
     'packages/coding-agent/src/core/session-manager.ts:SessionManager'];
 const artifact=JSON.stringify({...metadata,sourceEndpoints,scenarios:observations,
     upstreamSources:Object.fromEntries(sources.map(p=>[p,sha(readFileSync(join(root,p)))]))},null,2)+'\n';
 const generatorPath='fixtures/pi-v1.3.0/audit-1119/capture.mts';
 const generator={path:generatorPath,sha256:sha(readFileSync(generatorPath)),
     command:'PI_CHECKOUT=<pi-v1.3.0 checkout> bun --conditions=source '+generatorPath,
     sources:[{path:generatorPath,sha256:sha(readFileSync(generatorPath))}]};
 writeFileSync(resolve('fixtures/pi-v1.3.0/audit-1119/execution.json'),artifact);
 writeFileSync(resolve('fixtures/pi-v1.3.0/audit-1119/manifest.json'),JSON.stringify({...metadata,ticket:'1119',family:'audit-1119',generator,
     artifacts:[{path:'execution.json',family:'audit-1119',bytes:Buffer.byteLength(artifact),sha256:sha(artifact),sourceEndpoints,scenarios:observations}]},null,2)+'\n');
} finally {rmSync(workspace,{recursive:true,force:true});}
