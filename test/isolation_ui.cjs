/* Headless panel sanity: isolation and restore never trigger AppInstallAll. */
const assert=require('node:assert/strict');
const fs=require('node:fs');
const vm=require('node:vm');
const html=fs.readFileSync('web/index.html','utf8');
const code=html.match(/<script>([\s\S]*?)<\/script>/)[1];
const els={};
const get=(id)=> els[id] ||= {value:'',textContent:'',hidden:false,innerHTML:'',checked:false,
  style:{}, disabled:false, removeAttribute(){},addEventListener(evt,fn){this.onClick=fn;},showModal(){},close(){}};
let current={active:false,target:'',hidden:0},requests=[], confirms=0;
let entry={title_id:'CUSA34384',name:'GOW',state:'ready',dlc:3,mount:'rw',has_patch:true,source:'/mnt/usb0/ps4/CUSA34384',size:100,version:'01.00'};
function fetch(url, opts={}) {
 requests.push(url);
 let j;
 if (url==='/api/games')j={version:'test',games:[entry]};
 else if (url==='/api/isolation') j=current;
 else if (url==='/api/isolate-others?title_id=CUSA34384') {current={active:true,target:'CUSA34384',hidden:15};j={message:'Hidden 15'};}
 else if (url==='/api/restore-others') {current={active:false,target:'',hidden:0};j={message:'Restored 15'};}
 else j={phase:'idle',ok:0,done:0,total:0,message:'ok'};
 return Promise.resolve({ok:true,json:()=>Promise.resolve(j)});
}
const ctx=vm.createContext({document:{getElementById:get},fetch,
 confirm:()=>{confirms++;return true;},setTimeout:()=>1,clearTimeout(){},setInterval:()=>0,console});
vm.runInContext(code,ctx);
let flush=async()=>{for(let i=0;i<8;i++)await new Promise(r=>setImmediate(r));};
(async()=>{
 await flush();
 assert(get('grid').innerHTML.includes('Isolar outros'));
 function click(action){ get('grid').onClick({target:{closest:()=>({getAttribute:k=>k==='data-a'?action:'CUSA34384'})}});}
 click('isolate-others'); await flush();
 assert(current.active && get('grid').innerHTML.includes('Restaurar outros'));
 assert(get('grid').innerHTML.includes('Registrar DLCs (GLOBAL)'));
 assert(!requests.some(x=>x.startsWith('/api/register-dlcs')));
 click('restore-others'); await flush();
 assert(!current.active && get('grid').innerHTML.includes('Isolar outros'));
 assert.deepEqual(requests.filter(x=>x.startsWith('/api/isolate-others') || x==='/api/restore-others'),
 ['/api/isolate-others?title_id=CUSA34384','/api/restore-others']);
 assert.equal(confirms,1,'Only isolation confirmation requested');
 console.log('OK: isolation buttons POST isolated calls; GLOBAL API not triggered');
})().catch(e=>{console.error(e);process.exitCode=1;});
