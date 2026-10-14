/* Browser-independent panel behavior checks. Does not require a PS5. */
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const page = fs.readFileSync('web/index.html', 'utf8');
const source = page.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = Object.create(null);
const get = (id) => elements[id] ||= {
  id, value: '', textContent:'', innerHTML:'', hidden: id === 'progress-wrap',
  disabled:false, style:{}, checked:false, max:1, className:'',
  removeAttribute(k) { if(k === 'value') delete this.value; },
  addEventListener(_, f) { this.onClick = f; },
  showModal() {}, close() {},
};
get('q').value = '';
const tid = 'CUSA11111';
let confirms = 0, requests = [], state = 'pending', fastComplete = false, librarySetting = false,
  progress = {phase:'idle',total:0,done:0,ok:0,failed:0,current:'',detail:'',scan_folders:0,scan_pkgs:0,scan_organized:0,scan_path:''};
const entries = [
  {name:'Beta 10',title_id:'CUSA33333',state:'pending'},
  {name:'Águia',title_id:tid,state:'pending'},
  {name:'Beta 2',title_id:'CUSA22222',state:'ready'},
];
const games = () => entries.map(g => ({ ...g, state:g.title_id === tid ? state : g.state,
  version:'01.00',size:0,source:'/mnt/usb0/ps4/'+g.title_id,dlc:0,has_patch:false }));
function fetch(path, opts = {}) {
  requests.push(path);
  let payload;
  if(path === '/api/config' && opts.method === 'POST') {
    librarySetting = opts.body === 'remove_from_library=1';
    payload = {ok:true,message:'saved'};
  } else if(path === '/api/config') payload = {remove_from_library:librarySetting};
  else if(path === '/api/games') payload = {version:'test',games:games()};
  else if(path === '/api/progress') payload = progress;
  else if(path === '/api/mount?title_id='+tid) {
    progress = {...progress,phase:fastComplete ? 'complete':'queued',current:tid,detail:'Preparing selected game'};
    if(fastComplete) state = 'ready';
    payload = {message:'Mount queued'};
  } else if(path === '/api/unmount?title_id='+tid) {
    state='inactive'; payload = {message:'Unmounted'};
  } else payload = {message:'ok'};
  return Promise.resolve({ok:true,json:()=>Promise.resolve(payload)});
}
const context = vm.createContext({ document:{getElementById:get}, fetch,
  confirm:()=>{ confirms++; return true; },setTimeout:()=>1,clearTimeout:()=>{},
  setInterval:()=>0, encodeURIComponent, console });
vm.runInContext(source,context);
const flush = async()=>{ for(let i=0;i<6;i++) await new Promise(resolve=>setImmediate(resolve)); };
(async()=>{
  await flush();
  let html = get('grid').innerHTML;
  assert(html.indexOf('Águia') < html.indexOf('Beta 2'));
  assert(html.indexOf('Beta 2') < html.indexOf('Beta 10'));
  assert.equal(get('progress-wrap').hidden,true,'idle should hide global progress');
  assert(!html.includes('card-progress'));
  // A mount must start immediately and appear on the game card.
  const button = (action) => ({getAttribute:k=>k==='data-t'?tid:action});
  get('grid').onClick({target:{closest:()=>button('mount')}});
  assert.equal(confirms,0,'Mount must not request confirmation');
  assert(get('grid').innerHTML.includes('Montando e registrando DLCs'));
  assert(get('grid').innerHTML.includes('class="card-progress"'));
  await flush();
  assert(get('grid').innerHTML.includes('Preparando jogo'));
  assert.equal(get('progress-wrap').hidden,true,'mount progress belongs to the game card');
  // Show a step change while installing.
  progress = {...progress,phase:'activating',detail:'Mounting registered game and update'};
  vm.runInContext('loadProgress()',context);
  await flush();
  assert(get('grid').innerHTML.includes('Montando jogo e atualização'));
  // End clears the progress without success toast, even if it is very fast.
  state = 'ready';
  progress = {...progress,phase:'complete',done:1,ok:1};
  vm.runInContext('loadProgress()',context);
  await flush();
  assert(!get('grid').innerHTML.includes('card-progress'),'Completed card must not show progress');
  assert.equal(get('progress-wrap').hidden,true,'No permanent Operação concluída banner');
  assert.equal(get('toast').style.display,undefined,'No success toast for mounting');
  // Unmount still prompts and shows a local spinner only while its POST is pending.
  get('grid').onClick({target:{closest:()=>button('unmount')}});
  assert.equal(confirms,1,'Unmount confirmation is preserved');
  assert(get('grid').innerHTML.includes('Desmontando jogo'));
  await flush();
  assert(!get('grid').innerHTML.includes('card-progress'));
  assert.equal(get('toast').style.display,undefined,'No success toast for unmounting');
  // Extremely fast completion before the first progress poll must also clear its card.
  fastComplete = true;
  get('grid').onClick({target:{closest:()=>button('mount')}});
  await flush();
  assert.equal(confirms,1,'No mount confirmation, including a second attempt');
  assert(!get('grid').innerHTML.includes('card-progress'),'Fast completion must clear card');
  assert.equal(get('toast').style.display,undefined);
  // USB scan remains global and visible only while scanning.
  progress = {...progress,phase:'searching',scan_folders:7,scan_pkgs:12,scan_organized:3,scan_path:'/mnt/usb0'};
  vm.runInContext('loadProgress()',context);
  await flush();
  assert.equal(get('progress-wrap').hidden,false,'Scan progress must be visible');
  assert(get('progress-title').textContent.includes('7 pastas'));
  progress = {...progress,phase:'complete'};
  vm.runInContext('loadProgress()',context);
  await flush();
  assert.equal(get('progress-wrap').hidden,true);
  // Library removal option is off by default; changing it must persist via API.
  assert.equal(get('remove-library').checked,false);
  get('remove-library').checked=true;
  get('remove-library').onchange();
  await flush();
  assert.equal(librarySetting,true);
  assert(requests.includes('/api/config'));
  assert.equal(get('remove-library').disabled,false);
  get('remove-library').checked=false;
  get('remove-library').onchange();
  await flush();
  assert.equal(librarySetting,false);
  assert.equal(get('remove-library').checked,false);
  console.log('Panel UI: 15+ behavioral assertions passed');
})().catch(e=>{console.error(e);process.exitCode=1;});
