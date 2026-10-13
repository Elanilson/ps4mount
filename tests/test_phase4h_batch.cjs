/* Phase 4H browser test: only one start POST, thereafter status is read-only.
 * No browser-driven mounting, organization or registration. */
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const h=fs.readFileSync('web/index.html','utf8');
const js=h.match(/<script>([\s\S]*?)<\/script>/)[1];
async function flush() {for(let i=0;i<30;i++)await new Promise(r=>setImmediate(r));}
const games=Array.from({length:12},(_,idx)=>idx+1).map(i=>({title_id:'CUSA'+String(i).padStart(5,'0'),name:'Game '+i,size:i*1024}));
function setup(shared) {
  const elems=new Map(),posts=[],timers=[];
  function el(id){if(!elems.has(id))elems.set(id,{id,value:'',checked:false,textContent:'',className:'',style:{},
    disabled:false,hidden:false,innerHTML:'',handlers:{},addEventListener(n,f){this.handlers[n]=f;},
    showModal(){this.open=true;},close(){this.open=false;},focus(){}});return elems.get(id);}
  function backend(url,method,headers){
    if(method==='POST'){
      posts.push(url);
      assert(/^\/api\/batch-start\?limit=5$/.test(url),'NO per-title POST from browser');
      assert.equal(shared.startCount,0,'never replay start POST');
      assert.equal(headers['X-PS4Mount-Token'],'b'.repeat(32));
      shared.startCount++;
      shared.running=true;
      shared.count=games.length;
      return {message:'Queue accepted in ELF'};
    }
    if(url==='/api/sync-status')return {phase:'completed',processed:0,total:0,eligible:0,registered:0,blocked:0,unknown:0};
    if(url==='/api/games')return {games:[],version:'0.5.9',fault:false};
    if(url==='/api/batch-status')return {phase:shared.running?'running':shared.completed?'completed':'idle',group_phase:'register',
      running:shared.running,count:shared.count,processed:shared.processed,registered:shared.processed,
      group_index:Math.floor(shared.processed/5)+1,group_total:3,group_count:5,wait_seconds:0,
      current:games[shared.processed]?.title_id||'',titles:games.slice(0,shared.count).map(g=>g.title_id),message:shared.running?'working':'idle'};
    if(url.startsWith('/api/batch-preview?limit=')){
      assert.equal(Number(url.split('=')[1]),5);
      return {can_start:true,confirm_token:'b'.repeat(32),games,count:games.length,limit:5};
    }
    throw Error('Unexpected url '+url);
  }
  const ctx={document:{getElementById:el},console,Promise,
    setTimeout(){},clearTimeout(){},setInterval(fn){timers.push(fn);},
    fetch(url,opt={}) {const method=opt.method||'GET';let data;
      try {data=backend(url,method,opt.headers||{});} catch(e){return Promise.resolve({ok:false,json:()=>Promise.resolve({error:e.message})});}
      return Promise.resolve({ok:true,json:()=>Promise.resolve(data)});
    },prompt(){throw Error('prompt forbidden');},confirm(){throw Error('confirm forbidden');},alert(){throw Error('alert forbidden');}};
  vm.runInNewContext(js,ctx);
  return {el,posts,timers};
}
(async()=>{
  const shared={startCount:0,running:false,count:0,processed:0,completed:false};
  let x=setup(shared);await flush();
  assert.equal(shared.startCount,0,'nothing runs on page load');
  x.el('batchlimit').value='5';
  x.el('batchpreview').onclick();await flush();
  assert(x.el('batchitems').textContent.includes('CUSA00012'));
  assert.equal(x.el('batchpreview').hidden,false);
  assert(x.el('batchstart').disabled);
  x.el('batchbackup').checked=true;x.el('batchbackup').onchange();
  for(const tick of x.timers)tick(); await flush();
  assert.equal(x.el('batchstart').disabled,false,'status polling cannot cancel valid reviewed confirmation');
  x.el('batchstart').onclick();await flush();
  assert.equal(shared.startCount,1);
  assert.deepStrictEqual(x.posts,['/api/batch-start?limit=5']);
  // Browser closes while the ELF continues to operate.
  shared.processed=1;
  x=setup(shared);await flush();
  assert.equal(shared.startCount,1,'reload cannot restart batch');
  assert(x.el('batchstatus').textContent.includes('1 / 12'));
  assert(x.el('grouplive').textContent.includes('1 / 12'));
  assert.equal(x.el('groupprogress').max,12);
  x.el('batchpreview').onclick();await flush();
  assert(!x.el('batchstart').disabled===false);
  shared.running=false;shared.completed=true;shared.processed=12;
  for(const tick of x.timers)tick();await flush();
  assert.equal(shared.startCount,1);
  console.log('PASS: ELF queue survives browser close/reload; only one batch-start POST; no per-title browser POST');
  // A new page never executes a POST, even if previous job completed.
  const y=setup({startCount:0,running:false,count:0,processed:0,completed:false});await flush();
  y.el('batchlimit').value='5';y.el('batchpreview').onclick();await flush();
  assert(y.el('batchitems').textContent.includes('CUSA00005'));
  assert.equal(y.el('batchstart').disabled,true);
  console.log('PASS: one manual POST for 12 titles; five games per group, status and read-only page reload');
})().catch(e=>{console.error(e);process.exit(1)});
