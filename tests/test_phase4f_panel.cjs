/* Host-only UI request test. This simulates browser events; no PS5 calls. */
const fs = require('fs');
const vm = require('vm');
const assert = require('assert');
const html = fs.readFileSync('web/index.html', 'utf8');
const js = html.match(/<script>([\s\S]*?)<\/script>/)[1];
const elements = new Map();
const calls = [];
function el(id) {
  if (!elements.has(id)) {
    const e = {
      id, value: '', checked: false, textContent: '', className: '',
      style: {}, innerHTML: '', disabled: false, hidden: false, handlers: {},
      addEventListener(event, cb) { this.handlers[event] = cb; },
      showModal() { this.open = true; }, close() { this.open = false; },
      focus() {}
    };
    elements.set(id, e);
  }
  return elements.get(id);
}
const tid = 'CUSA03323', token = 'a'.repeat(32), regtoken = 'b'.repeat(32);
let registerReady = false;
let mounted = false;
const handlers = {
  '/api/sync-status': () => ({phase:'completed',processed:1,total:1,eligible:1,registered:0,blocked:0,unknown:0}),
  '/api/batch-status': () => ({phase:'idle',running:false,count:0,processed:0,registered:0,current:'',titles:[],message:''}),
  '/api/games': () => ({version: '0.5.7-test', games: [{title_id:tid,name:'Uncanny Valley',state:'discovered',version:'01.00',has_patch:true,update_version:'01.02',source:'/mnt/usb0/PS4/CUSA03323 - Uncanny Valley/app.pkg',size:90000000}],fault:false}),
  ['/api/probe-preview?title_id='+tid]: () => ({title_id:tid,can_mount:!mounted,can_release:mounted,attempted:mounted,confirm_token:token,message:'ready'}),
  ['/api/probe-mount?title_id='+tid]: () => {mounted = true; return {message:'MOUNTED READ-ONLY'};},
  ['/api/probe-release?title_id='+tid]: () => {mounted = false; registerReady = true; return {message:'RELEASED'};},
  ['/api/register-preview?title_id='+tid]: () => ({title_id:tid,can_register:registerReady,single_api_available:true,verified_mount:registerReady,confirm_token:regtoken,message:'ready'}),
  ['/api/register-one?title_id='+tid]: () => ({message:'REGISTERED INDIVIDUALLY'})
};
let alertCount = 0, promptCount = 0, confirmCount = 0;
const context = {
  document:{getElementById:el}, console,
  fetch: (url, options) => {
    calls.push({url, options});
    assert(handlers[url], 'unknown endpoint '+url);
    return Promise.resolve({ok:true, json:()=>Promise.resolve(handlers[url]())});
  },
  setTimeout:()=>123,clearTimeout:()=>{},setInterval:()=>123,
  alert:()=>{alertCount++},prompt:()=>{promptCount++;throw new Error('prompt blocked in <dialog>')},
  confirm:()=>{confirmCount++;throw new Error('confirm blocked in <dialog>')},
  Promise
};
vm.runInNewContext(js, context);
const sleep = () => new Promise(r => setTimeout(r,0));
const click = a => el('grid').handlers.click({target:{closest:()=>({getAttribute:k=> k==='data-a'?a:tid})}});
(async()=> {
  await sleep();
  click('probe'); await sleep();
  assert(el('probedlg').open);
  el('probeaction').onclick(); await sleep();
  assert(mounted);
  assert(el('probestatus').textContent.includes('MOUNTED READ-ONLY'));
  assert.equal(calls.filter(x=>x.url==='/api/probe-mount?title_id='+tid).length,1);
  el('probeaction').onclick(); await sleep();
  assert.equal(calls.filter(x=>x.url==='/api/probe-mount?title_id='+tid).length,1,'no duplicate POST');
  click('probe'); await sleep();
  el('probeaction').onclick(); await sleep();
  assert(!mounted && registerReady);
  click('register'); await sleep();
  assert(el('registerdlg').open);
  assert(el('registeraction').disabled===false);
  el('registeraction').onclick();
  assert.equal(calls.filter(x=>x.url==='/api/register-one?title_id='+tid).length,0,'backup checkbox required');
  el('registerbackup').checked=true;
  el('registeraction').onclick(); await sleep();
  assert(el('registerstatus').textContent.includes('REGISTERED INDIVIDUALLY'));
  el('registeraction').onclick(); await sleep();
  assert.equal(calls.filter(x=>x.url==='/api/register-one?title_id='+tid).length,1,'single-use request');
  const mountRequest = calls.find(x => x.url==='/api/probe-mount?title_id='+tid);
  const registerRequest = calls.find(x => x.url==='/api/register-one?title_id='+tid);
  assert.equal(mountRequest.options.method,'POST');
  assert.equal(mountRequest.options.headers['X-PS4Mount-Token'],token);
  assert.equal(registerRequest.options.headers['X-PS4Mount-Token'],regtoken);
  assert.equal(promptCount,0); assert.equal(confirmCount,0); assert.equal(alertCount,0);
  console.log('PASS: UI mount, release, targeted registration; no popup; backup and single-use guards');
})().catch(e=>{console.error(e);process.exitCode=1});
