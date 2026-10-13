/* Phase 4H browser test: only one start POST, thereafter status is read-only.
 * No browser-driven mounting, organization or registration. */
const fs=require('fs'),vm=require('vm'),assert=require('assert');
const h=fs.readFileSync('web/index.html','utf8');
const js=h.match(/<script>([\s\S]*?)<\/script>/)[1];
async function flush() {for(let i=0;i<30;i++)await new Promise(r=>setImmediate(r));}
const games=[1,2,3,4,5].map(i=>({title_id:'CUSA0000'+i,name:'Game '+i,size:i*1024}));
function setup(shared) {
  const elems=new Map(),posts=[],timers=[];
  function el(id){if(!elems.has(id))elems.set(id,{id,value:'',checked:false,textContent:'',className:'',style:{},
    disabled:false,hidden:false,innerHTML:'',handlers:{},addEventListener(n,f){this.handlers[n]=f;},
    showModal(){this.open=true;},close(){this.open=false;},focus(){}});return elems.get(id);}
  function backend(url,method,headers){
    if(method==='POST'){
      posts.push(url);
      assert(/^\/api\/batch-start\?limit=[35]$/.test(url),'NO per-title POST from browser');
      assert.equal(shared.startCount,0,'never replay start POST');
      assert.equal(headers['X-PS4Mount-Token'],'b'.repeat(32));
      shared.startCount++;
      shared.running=true;
      shared.count=Number(url.split('=')[1]);
      return {message:'Queue accepted in ELF'};
    }
    if(url==='/api/sync-status')return {phase:'completed',processed:0,total:0,eligible:0,registered:0,blocked:0,unknown:0};
    if(url==='/api/games')return {games:[],version:'0.5.9',fault:false};
    if(url==='/api/batch-status')return {phase:shared.running?'register':shared.completed?'completed':'idle',
      running:shared.running,count:shared.count,processed:shared.processed,registered:shared.processed,
      current:games[shared.processed]?.title_id||'',titles:games.slice(0,shared.count).map(g=>g.title_id),message:shared.running?'working':'idle'};
    if(url.startsWith('/api/batch-preview?limit=')){
      const count=Number(url.split('=')[1]);
      return {can_start:true,confirm_token:'b'.repeat(32),games:games.slice(0,count),count,limit:count};
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
  x.el('batchlimit').value='3';
  x.el('batchpreview').onclick();await flush();
  assert(x.el('batchitems').textContent.includes('CUSA00003'));
  assert(!x.el('batchitems').textContent.includes('CUSA00004'));
  assert(x.el('batchstart').disabled);
  x.el('batchbackup').checked=true;x.el('batchbackup').onchange();
  x.el('batchstart').onclick();await flush();
  assert.equal(shared.startCount,1);
  assert.deepStrictEqual(x.posts,['/api/batch-start?limit=3']);
  // Browser closes while the ELF continues to operate.
  shared.processed=1;
  x=setup(shared);await flush();
  assert.equal(shared.startCount,1,'reload cannot restart batch');
  assert(x.el('batchstatus').textContent.includes('1 / 3'));
  x.el('batchpreview').onclick();await flush();
  assert(!x.el('batchstart').disabled===false);
  shared.running=false;shared.completed=true;shared.processed=3;
  for(const tick of x.timers)tick();await flush();
  assert.equal(shared.startCount,1);
  console.log('PASS: ELF queue survives browser close/reload; only one batch-start POST; no per-title browser POST');
  // New operator-reviewed limit of five is independently selectable.
  const y=setup({startCount:0,running:false,count:0,processed:0,completed:false});await flush();
  y.el('batchlimit').value='5';y.el('batchpreview').onclick();await flush();
  assert(y.el('batchitems').textContent.includes('CUSA00005'));
  assert.equal(y.el('batchstart').disabled,true);
  console.log('PASS: 3 default/5 explicit UI options require backup and a fresh preview');
})().catch(e=>{console.error(e);process.exit(1)});
