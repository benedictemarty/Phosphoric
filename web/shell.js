// Phosphoric WebAssembly — logique de la page (Module, UI, clavier, drag&drop).
// Externalisé du <script> inline de shell.html pour être compatible avec une
// Content-Security-Policy stricte (script-src 'self' 'wasm-unsafe-eval') : un
// script inline serait bloqué (script-src-elem) -> Module.canvas undefined ->
// 'canvas is undefined' au createContext WebGL. Chargé via <script src>.

//  ── config ───────────────────────────────────────────────────────────────
var ROMS = { atmos: '/roms/basic11b.rom', oric1: '/roms/basic10.rom' };
// ?rom=oric1|atmos on the URL forces the machine for this load (handy for
// linking a tape that needs a specific BASIC, e.g. ORIC-1-only games).
var urlRom = new URLSearchParams(location.search).get('rom');
var rom = (urlRom==='oric1'||urlRom==='atmos') ? urlRom : (sessionStorage.getItem('phos_rom') || 'atmos');
var mediaName = sessionStorage.getItem('phos_media_name') || '';
var mediaKind = sessionStorage.getItem('phos_media_kind') || '';
var args = ['-r', ROMS[rom] || ROMS.atmos];
if (mediaName && mediaKind === 'tap') args.push('-t', '/media/' + mediaName, '-f');
if (mediaName && mediaKind === 'dsk') args.push('--disk-rom', '/roms/microdis.rom', '-d', '/media/' + mediaName);

function idb(cb){ var r=indexedDB.open('phosphoric',1);
  r.onupgradeneeded=function(){r.result.createObjectStore('media');};
  r.onsuccess=function(){cb(r.result);}; r.onerror=function(){cb(null);}; }
function idbPut(k,b,d){ idb(function(db){ if(!db)return d&&d();
  var t=db.transaction('media','readwrite'); t.objectStore('media').put(b,k);
  t.oncomplete=function(){d&&d();}; }); }
function idbGet(k,d){ idb(function(db){ if(!db)return d(null);
  var g=db.transaction('media','readonly').objectStore('media').get(k);
  g.onsuccess=function(){d(g.result||null);}; g.onerror=function(){d(null);}; }); }

var statusEl = document.getElementById('status');
var ready = false;
var Module = {
  arguments: args, canvas: document.getElementById('canvas'),
  print:function(t){console.log(t);}, printErr:function(t){console.warn(t);},
  preRun: [function(){ if(!mediaName) return; addRunDependency('media-file');
    idbGet(mediaName, function(buf){ try{ if(buf){ try{FS.mkdir('/media');}catch(e){}
      FS.writeFile('/media/'+mediaName, new Uint8Array(buf)); } }catch(e){console.warn(e);}
      removeRunDependency('media-file'); }); }],
  onRuntimeInitialized: function(){ ready=true; refreshUI(); maybeLoadUrlMedia(); }
};

// ?media=<file> on the URL → fetch it from the server and hot-insert it live
// (no reboot), reusing the same C bridge as drag-and-drop. Handy for sharing a
// direct link to a tape/disk, e.g. phosphoric.html?media=poker-asn.tap
function maybeLoadUrlMedia(){
  var p = new URLSearchParams(location.search).get('media');
  if(!p) return;
  var low=p.toLowerCase(); var kind=low.endsWith('.dsk')?'dsk':(low.endsWith('.tap')?'tap':'');
  if(!kind){ console.warn('?media: unsupported (need .tap/.dsk):',p); return; }
  fetch(p).then(function(r){ if(!r.ok) throw new Error('HTTP '+r.status); return r.arrayBuffer(); })
    .then(function(buf){ var name=p.split('/').pop();
      try{FS.mkdir('/media');}catch(e){}
      var path='/media/'+name; FS.writeFile(path, new Uint8Array(buf));
      // The C bridge needs main() to have set g_web_emu; onRuntimeInitialized
      // fires just before that, so retry briefly until the insert takes.
      (function tryInsert(n){
        var ok = (kind==='tap')
          ? Module.ccall('web_insert_tap','number',['string'],[path])
          : Module.ccall('web_insert_disk','number',['number','string'],[0,path]);
        if(ok){ idbPut(name, buf, function(){}); sessionStorage.setItem('phos_media_name', name); sessionStorage.setItem('phos_media_kind', kind); refreshUI(); }
        else if(n>0) setTimeout(function(){tryInsert(n-1);}, 100);
        else console.warn('?media: insert failed — "'+name+'" is not a valid '+kind.toUpperCase()+
          ' (server may have returned an HTML page instead of the binary — check it downloads raw),'+
          (kind==='dsk'?' or the disk needs the Microdisc ROM,':'')+' or the engine was not ready.');
      })(50);
    }).catch(function(e){ console.warn('?media load failed:',e); });
}

function refreshUI(){
  document.getElementById('machine-badge').textContent = (rom==='oric1')?'1':'A';
  var ej=document.getElementById('btn-eject'); ej.hidden=!mediaName; if(mediaName) ej.title='Eject '+mediaName;
  var label=(rom==='oric1'?'ORIC-1 / BASIC 1.0':'Atmos / BASIC 1.1');
  if(mediaName) label += ' · '+mediaName+' ('+mediaKind+')';
  statusEl.textContent=(ready?'Running — ':'Loading — ')+label;
}
document.getElementById('btn-machine').onclick=function(){ sessionStorage.setItem('phos_rom', rom==='oric1'?'atmos':'oric1'); location.reload(); };
document.getElementById('btn-reset').onclick=function(){ location.reload(); };
document.getElementById('btn-eject').onclick=function(){ sessionStorage.removeItem('phos_media_name'); sessionStorage.removeItem('phos_media_kind'); location.reload(); };
document.getElementById('btn-load').onclick=function(){ document.getElementById('file-input').click(); };
document.getElementById('file-input').onchange=function(e){ if(e.target.files[0]) loadMedia(e.target.files[0]); };
(function(){
  var f=document.getElementById('frame');
  function fsEl(){ return document.fullscreenElement || document.webkitFullscreenElement || null; }
  function reqFs(){ (f.requestFullscreen||f.webkitRequestFullscreen||function(){}).call(f); }
  function exitFs(){ (document.exitFullscreen||document.webkitExitFullscreen||function(){}).call(document); }
  document.getElementById('btn-fs').onclick=function(){ if(fsEl()) exitFs(); else reqFs(); };
  // Toggle the .fs class ourselves (robust across browsers + beats SDL's inline
  // canvas style via !important in CSS).
  function onChange(){ f.classList.toggle('fs', !!fsEl()); }
  document.addEventListener('fullscreenchange', onChange);
  document.addEventListener('webkitfullscreenchange', onChange);
})();
// keyboard overlay toggle
var btnKbd=document.getElementById('btn-kbd'), frame=document.getElementById('frame');
btnKbd.onclick=function(){ var off=frame.classList.toggle('kbd-off'); btnKbd.classList.toggle('on', !off); };

// CRT filter toggle (persisted)
var btnCrt=document.getElementById('btn-crt');
if(localStorage.getItem('phos_crt')==='1'){ frame.classList.add('crt'); btnCrt.classList.add('on'); }
btnCrt.onclick=function(){ var on=frame.classList.toggle('crt'); btnCrt.classList.toggle('on',on);
  localStorage.setItem('phos_crt', on?'1':'0'); };

// I/O activity LEDs (poll bit0=tape, bit1=disk)
var ledTape=document.getElementById('led-tape'), ledDisk=document.getElementById('led-disk');
setInterval(function(){ if(!ready) return;
  var b=0; try{ b=Module.ccall('web_io_activity',null,[],[])||0; }catch(e){}
  ledTape.classList.toggle('on', (b&1)!==0); ledDisk.classList.toggle('on', (b&2)!==0); }, 120);

// Save state → download .ost
document.getElementById('btn-savestate').onclick=function(){ if(!ready) return;
  try{ if(!Module.ccall('web_save_state',['number'],[],[])){ alert('Save failed'); return; }
    var data=FS.readFile('/state.ost');
    var blob=new Blob([data],{type:'application/octet-stream'});
    var a=document.createElement('a'); a.href=URL.createObjectURL(blob);
    a.download='phosphoric-'+(rom==='oric1'?'oric1':'atmos')+'.ost';
    document.body.appendChild(a); a.click(); a.remove(); URL.revokeObjectURL(a.href);
  }catch(e){ console.warn(e); alert('Save failed'); } };
// Restore state ← upload .ost (applied live, no reload)
document.getElementById('btn-loadstate').onclick=function(){ document.getElementById('state-input').click(); };
document.getElementById('state-input').onchange=function(e){ var f=e.target.files[0]; if(!f||!ready) return;
  f.arrayBuffer().then(function(buf){ try{ FS.writeFile('/state.ost', new Uint8Array(buf));
    if(!Module.ccall('web_load_state',['number'],[],[])) alert('Restore failed (incompatible snapshot?)');
  }catch(err){ console.warn(err); alert('Restore failed'); } }); e.target.value=''; };

function loadMedia(file){
  var low=file.name.toLowerCase(); var kind=low.endsWith('.dsk')?'dsk':(low.endsWith('.tap')?'tap':'');
  if(!kind){ alert('Unsupported file — drop a .tap or .dsk'); return; }
  file.arrayBuffer().then(function(buf){ idbPut(file.name, buf, function(){
    // Remember the media so a future reload reattaches it.
    sessionStorage.setItem('phos_media_name', file.name); sessionStorage.setItem('phos_media_kind', kind);
    // Try a live hot-swap first — no reboot (tape arms auto-CLOAD""; disk mounts
    // into drive A). Falls back to a full reload on first media or when the disk
    // controller is not active this session.
    if(ready){
      try{
        try{FS.mkdir('/media');}catch(e){}
        var path='/media/'+file.name;
        FS.writeFile(path, new Uint8Array(buf));
        if(kind==='tap'){
          if(Module.ccall('web_insert_tap','number',['string'],[path])) return;
        }else{
          if(Module.ccall('web_insert_disk','number',['number','string'],[0,path])) return;
        }
      }catch(err){ console.warn('hot-swap failed, reloading:',err); }
    }
    location.reload();
  }); });
}
var dd=0;
window.addEventListener('dragenter',function(e){e.preventDefault(); if(dd++===0)document.body.classList.add('dragging');});
window.addEventListener('dragover',function(e){e.preventDefault();});
window.addEventListener('dragleave',function(e){e.preventDefault(); if(--dd<=0){dd=0;document.body.classList.remove('dragging');}});
window.addEventListener('drop',function(e){e.preventDefault(); dd=0; document.body.classList.remove('dragging');
  if(e.dataTransfer.files&&e.dataTransfer.files[0]) loadMedia(e.dataTransfer.files[0]);});
window.addEventListener('click',function(){ if(Module.SDL2&&Module.SDL2.audioContext&&Module.SDL2.audioContext.state==='suspended') Module.SDL2.audioContext.resume(); });

//  ── ORIC keyboard — layout & shifted symbols derived from the emulator's
//     keyboard matrix (src/io/keyboard.c char_map): 2→@, 6→^, etc. ──────────
var ESC=27,RET=13,SPC=32,DEL=0x84,UP=0x80,DN=0x81,LF=0x82,RG=0x83;
var LAYOUT = [
  [['ESC',ESC,'lbl w12'],['1',49,'','!'],['2',50,'','@'],['3',51,'','#'],['4',52,'','$'],['5',53,'','%'],['6',54,'','^'],['7',55,'','&'],['8',56,'','*'],['9',57,'','('],['0',48,'',')'],['-',45,'','_'],['=',61,'','+'],['DEL',DEL,'lbl w12']],
  [['CTRL','M_C','mod w15'],['Q',113],['W',119],['E',101],['R',114],['T',116],['Y',121],['U',117],['I',105],['O',111],['P',112],['[',91,'','{'],[']',93,'','}'],['RET',RET,'lbl ret']],
  [['FUNCT','M_F','mod w18'],['A',97],['S',115],['D',100],['F',102],['G',103],['H',104],['J',106],['K',107],['L',108],[';',59,'',':'],["'",39,'','"'],['\\',92,'','|']],
  [['SHIFT','M_S','mod w20'],['Z',122],['X',120],['C',99],['V',118],['B',98],['N',110],['M',109],[',',44,'','<'],['.',46,'','>'],['/',47,'','?'],['SHIFT','M_S','mod w15'],['↑',UP,'lbl']],
  [['SPACE',SPC,'space'],['←',LF,'lbl'],['↓',DN,'lbl'],['→',RG,'lbl']]
];
// Two sources for each modifier: 'stick' = armed by tapping the on-screen key
// (one-shot), 'phys' = the physical Shift/Ctrl/Alt held on the real keyboard.
// The effective modifier is the OR of both, so real Shift/Ctrl drive the
// virtual keyboard too (and light up its on-screen keys).
var stick={c:false,f:false,s:false}, phys={c:false,f:false,s:false};
function modOn(k){ return stick[k]||phys[k]; }
function callKey(code,down){ if(!ready)return;
  try{ Module.ccall('web_key',null,['number','number','number','number','number'],
    [code,modOn('c')?1:0,modOn('f')?1:0,modOn('s')?1:0,down?1:0]); }catch(e){} }
function syncMods(){ document.querySelectorAll('.key.mod').forEach(function(el){ var s=el.dataset.mod; if(s) el.classList.toggle('on',modOn(s)); }); }
function clearMods(){ stick.c=stick.f=stick.s=false; syncMods(); }   // clears only the one-shot sticky state
// Mirror the physical Shift / Ctrl / Alt(→FUNCT) into the virtual modifiers.
function physMod(e, down){
  var k=e.key, slot = (k==='Shift')?'s':(k==='Control')?'c':(k==='Alt')?'f':null;
  if(!slot) return; phys[slot]=down; syncMods();
}
window.addEventListener('keydown', function(e){ physMod(e,true); });
window.addEventListener('keyup',   function(e){ physMod(e,false); });
window.addEventListener('blur',    function(){ phys.c=phys.f=phys.s=false; syncMods(); });
function buildKbd(){
  var root=document.getElementById('kbd');
  LAYOUT.forEach(function(row){ var rd=document.createElement('div'); rd.className='row';
    row.forEach(function(k){ var label=k[0],code=k[1],cls=k[2]||'',sup=k[3]||'';
      // The ORIC-1 keyboard has no FUNCT key (Atmos-only) — render an inert filler.
      if (code==='M_F' && rom==='oric1') {
        var f=document.createElement('div'); f.className='key inert '+cls.replace('mod',''); f.innerHTML='';
        rd.appendChild(f); return;
      }
      var el=document.createElement('div'); el.className='key '+cls;
      var plain=(cls.indexOf('lbl')<0 && cls.indexOf('mod')<0);
      el.innerHTML=(plain?'<span class="sup">'+sup+'</span>':'')+label;
      if(code==='M_C'||code==='M_F'||code==='M_S'){ var slot=code==='M_C'?'c':(code==='M_F'?'f':'s'); el.dataset.mod=slot;
        el.addEventListener('pointerdown',function(e){e.preventDefault(); stick[slot]=!stick[slot]; syncMods();}); }
      else { var press=function(e){e.preventDefault(); el.classList.add('held'); callKey(code,true);};
        var rel=function(e){ if(e)e.preventDefault(); if(!el.classList.contains('held'))return;
          el.classList.remove('held'); callKey(code,false); if(stick.c||stick.f||stick.s) clearMods(); };
        el.addEventListener('pointerdown',press); el.addEventListener('pointerup',rel);
        el.addEventListener('pointerleave',rel); el.addEventListener('pointercancel',rel); }
      rd.appendChild(el); });
    root.appendChild(rd); });
}
buildKbd(); refreshUI();

// Remplace l'ancien attribut inline oncontextmenu du <canvas> (bloqué par CSP
// script-src-attr) par un écouteur DOM équivalent.
(function(){ var c=document.getElementById('canvas');
  if(c) c.addEventListener('contextmenu', function(e){ e.preventDefault(); }); })();
