/* Phase 4G browser harness: deterministic one-by-one, max 3, no AppInstallAll. */
const fs = require('fs'), vm = require('vm'), assert = require('assert');
const html = fs.readFileSync('web/index.html','utf8');
const js = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const titles = [1,2,3,4].map(n => 'CUSA0000'+n);
const fakeGames = titles.map((tid,i) => ({ title_id:tid, name:'Fake game '+i,
  state:'discovered', source:'/mnt/usb0/PS4/'+tid+'/app.pkg', update_source:'',
  version:'01.00', update_version:'', size: 5000000 + i * 50000,
  has_patch:false, update_issue:'', suspicious_updates:0 }));
fakeGames.push({title_id:'CUSA00005',name:'Conflict',state:'conflict',size:1,source:'',update_source:'',version:'01.00',has_patch:true});
function setup(failAt) {
  const elements = new Map(), posts = [], seen = [];
  const mounted = {}, released = {}, registered = {};
  function el(id) { if(!elements.has(id)) elements.set(id,{id,value:'',checked:false,textContent:'',className:'',
    style:{},innerHTML:'',disabled:false,hidden:false,handlers:{},addEventListener(ev,f){this.handlers[ev]=f;},
    showModal(){this.open=true;},close(){this.open=false;},focus(){}}); return elements.get(id); }
  function response(url, method) {
    const tid = new URL('http://local'+url).searchParams.get('title_id');
    if (method === 'POST') {
      posts.push(url);
      if (failAt && url === '/api/register-one?title_id='+failAt) throw new Error('simulated app.db write failure');
      if (url.includes('/api/probe-mount?')) { assert(!mounted[tid]); mounted[tid]=true; return {message:'mounted'}; }
      if (url.includes('/api/probe-release?')) {assert(mounted[tid]); mounted[tid]=false; released[tid]=true;return {message:'released'};}
      if (url.includes('/api/register-one?')) {assert(released[tid]); registered[tid]=true;seen.push(tid);return {message:'registered'};}
      throw Error('Unexpected POST '+url);
    }
    if(url==='/api/games') return {version:'test',games:fakeGames,fault:false};
    if (url.startsWith('/api/launch-check?')) return {database_status:registered[tid]?'already_registered':'not_registered',preflight_passed:true, message:'ok'};
    if (url.startsWith('/api/plan?')) return {title_id:tid,confirm_token:'a'.repeat(32),move_base:false,move_update:false};
    if (url.startsWith('/api/probe-preview?')) return {title_id:tid,can_mount:!mounted[tid]&&!released[tid],can_release:!!mounted[tid],confirm_token:'b'.repeat(32),message:'ready'};
    if (url.startsWith('/api/register-preview?')) return {title_id:tid,can_register:!!released[tid],confirm_token:'c'.repeat(32),message:'ready'};
    throw Error('Unexpected GET '+url);
  }
  const ctx={document:{getElementById:el}, console,
    setInterval(){},setTimeout(){},clearTimeout(){},Promise,
    fetch(url,opt={}){let data;
      try { data=response(url, opt.method || 'GET'); }
      catch(e){ return Promise.resolve({ok:false,json:()=>Promise.resolve({error:e.message})}); }
      return Promise.resolve({ok:true,json:()=>Promise.resolve(data)});
    }, alert(){throw Error('unexpected alert');},prompt(){throw Error('unexpected prompt');},confirm(){throw Error('unexpected confirm');}}
  vm.runInNewContext(js,ctx);
  return {ctx,el,posts,seen};
}
async function microtask() { for(let i=0;i<35;i++) await new Promise(resolve=>setImmediate(resolve)); }
async function testSuccess(){
  const t=setup(null);
  await microtask();
  assert.equal(t.posts.length,0,'must never start automatically');
  t.el('batchpreview').onclick();
  assert(t.el('batchdlg').open);
  assert(t.el('batchitems').textContent.includes('CUSA00001'));
  assert(!t.el('batchitems').textContent.includes('CUSA00004'));
  assert.equal(t.el('batchstart').disabled,true);
  t.el('batchbackup').checked=true;
  t.el('batchbackup').onchange();
  assert.equal(t.el('batchstart').disabled,false);
  await t.el('batchstart').onclick();
  await microtask();
  assert.deepStrictEqual(t.seen,titles.slice(0,3));
  for(const tid of titles.slice(0,3)) {
    assert.equal(t.posts.filter(x=>x==='/api/probe-mount?title_id='+tid).length,1);
    assert.equal(t.posts.filter(x=>x==='/api/probe-release?title_id='+tid).length,1);
    assert.equal(t.posts.filter(x=>x==='/api/register-one?title_id='+tid).length,1);
  }
  assert(!t.posts.some(x=>x.includes('AppInstallAll')||x.includes('register-all')));
  assert(t.el('batchstatus').textContent.includes('LOTE CONCLUIDO'));
  console.log('PASS: batch automatically sequences 3 distinct titles, never the 4th, never duplicates');
}
async function testAbort(){
  const t=setup(titles[1]); await microtask();
  t.el('batchpreview').onclick(); t.el('batchbackup').checked=true;
  t.el('batchbackup').onchange();
  await t.el('batchstart').onclick(); await microtask();
  assert.deepStrictEqual(t.seen,[titles[0]]);
  assert.equal(t.posts.filter(x=>x==='/api/register-one?title_id='+titles[1]).length,1);
  assert(!t.posts.some(x=>x.includes(titles[2])));
  assert(t.el('batchstatus').textContent.includes('PARADO NO PRIMEIRO ERRO'));
  console.log('PASS: first failed registration stops the batch immediately without retry');
}
(async()=>{await testSuccess();await testAbort()})().catch(e=>{console.error(e);process.exit(1)});
