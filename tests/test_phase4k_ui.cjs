const fs=require('fs'),vm=require('vm'),assert=require('assert');
const h=fs.readFileSync('web/index.html','utf8');
const js=h.match(/<script>([\s\S]*?)<\/script>/)[1];
const elems=new Map(),requests=[];
function el(id){if(!elems.has(id))elems.set(id,{id,value:'',checked:false,textContent:'',innerHTML:'',style:{},disabled:false,hidden:false,
  handlers:{},addEventListener(n,f){this.handlers[n]=f;},showModal(){this.open=true;},close(){this.open=false;}});return elems.get(id)}
function api(url,method){
  requests.push((method||'GET')+' '+url);
  if(url==='/api/games')return {games:[{title_id:'CUSA06886',name:'What Remains of Edith Finch',size:2625896448,state:'discovered',version:'01.00',has_patch:false,source:'USB',update_candidates:0}],version:'PS4Mount 0.5.12',fault:false};
  if(url==='/api/batch-status')return {phase:'failed',running:false,count:5,processed:2,registered:2,current:'CUSA06886',message:'previous failure'};
  if(url==='/api/sync-status')return {phase:'completed',total:147,processed:147,eligible:125,registered:17,blocked:5,unknown:0};
  if(url==='/api/registration-plan')return {phase:'ready',total:147,eligible:125,registered:17,blocked:5,unknown:0,previous_batch_unresolved:true,single_title_test:'CUSA06886',games:[{title_id:'CUSA06886',name:'What Remains of Edith Finch',has_patch:false}]};
  if(url==='/api/mount-audit?title_id=CUSA06886')return {title_id:'CUSA06886',has_patch:false,app_layer:'unmounted',patch_layer:'foreign_or_unknown',patch_action:'UNUSED_UNTOUCHED',safe_to_probe:true,message:'NO changes'};
  throw Error('unexpected endpoint '+url);
}
const ctx={document:{getElementById:el},console,Promise,setTimeout(){},clearTimeout(){},setInterval(){},
  fetch(url,opt={}){let data;try {data=api(url,opt.method);}catch(e){return Promise.resolve({ok:false,json:()=>Promise.resolve({error:e.message})})}
    return Promise.resolve({ok:true,json:()=>Promise.resolve(data)})},
  prompt(){throw Error('prompt forbidden on page load')},confirm(){throw Error('confirm forbidden on page load')},alert(){throw Error('alert forbidden on page load')}};
async function flush(){for(let i=0;i<20;i++)await new Promise(r=>setImmediate(r))}
(async()=>{
 vm.runInNewContext(js,ctx);await flush();
 assert(!requests.some(x=>x.startsWith('POST ')),'loading the page NEVER starts writes');
 assert(el('grid').innerHTML.includes('mount-audit'));
 assert(el('grid').innerHTML.includes('Tentar registro individual'));
 el('queueplan').onclick(); await flush();
 assert(el('queuedlg').open);
 assert(el('queueinfo').textContent.includes('125'));
 assert(el('queueinfo').textContent.includes('true'));
 el('grid').handlers.click({target:{closest(){return {getAttribute(k){return k==='data-a'?'mount-audit':'CUSA06886'}}}}});await flush();
 assert(el('auditdlg').open);
 assert(el('auditinfo').textContent.includes('UNUSED_UNTOUCHED'));
 assert(!requests.some(x=>x.startsWith('POST ')),'audits + preview are read-only');
 console.log('PASS: Phase 4K queue plan and mount audit use GET only; page reload never registers');
})().catch(e=>{console.error(e);process.exit(1)});
