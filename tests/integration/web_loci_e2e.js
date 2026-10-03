// web_loci_e2e.js — drives headless Chrome (Playwright) on the WASM build in LOCI mode.
//
// Usage: node web_loci_e2e.js <base-url> [disk.dsk]
// Exit code: 0 = OK, 1 = failure, 77 = SKIP (Playwright not found).
//
// Checks, in the browser:
//   1. ?loci=1 starts the LOCI menu ("LOCI ROM" text on screen, read via web_peek);
//   2. the /loci flash (IDBFS) is seeded with the system ROMs;
//   3. a loaded file (LOAD button) is copied into the flash and survives a reload;
//   4. the menu's selector (Space, Space) lists that file;
//   5. if a real .dsk is provided: mounted as A: + ESC = boot → the disk starts.
'use strict';
const path = require('path');
const fs = require('fs');
const os = require('os');
const { execSync } = require('child_process');

function loadPlaywright() {
  try { return require('playwright'); } catch (e) {}
  try {
    const root = execSync('npm root -g', { stdio: ['ignore', 'pipe', 'ignore'] }).toString().trim();
    return require(path.join(root, 'playwright'));
  } catch (e) { return null; }
}

const pw = loadPlaywright();
if (!pw) { console.log('SKIP: playwright introuvable'); process.exit(77); }

const base = process.argv[2];
const realDisk = process.argv[3] && fs.existsSync(process.argv[3]) ? process.argv[3] : null;
const CHROME = process.env.CHROME || '/usr/bin/google-chrome';

let failures = 0;
function check(ok, msg) { console.log((ok ? '  PASS ' : '  FAIL ') + msg); if (!ok) failures++; }

// ORIC text screen: 28 lines × 40 columns at $BB80 (attributes < 0x20 → space).
async function screenText(p) {
  return p.evaluate(() => {
    let out = '';
    for (let r = 0; r < 28; r++) {
      let line = '';
      for (let c = 0; c < 40; c++) {
        const b = Module.ccall('web_peek', 'number', ['number'], [0xBB80 + r * 40 + c]) & 0x7F;
        line += (b >= 0x20 && b < 0x7F) ? String.fromCharCode(b) : ' ';
      }
      out += line + '\n';
    }
    return out;
  });
}
async function waitScreen(p, re, ms) {
  const t0 = Date.now(); let txt = '';
  while (Date.now() - t0 < ms) { txt = await screenText(p); if (re.test(txt)) return txt; await p.waitForTimeout(300); }
  return txt;
}
// Charge la page et attend que le moteur soit prêt (`ready`, mis à vrai dans
// onRuntimeInitialized) : avant, Module.ccall('web_peek') lève « func is not a
// function » (échec intermittent en suite complète, vu en 2.12.9).
async function gotoReady(p, url) {
  await p.goto(url);
  await p.waitForFunction(() => typeof ready !== 'undefined' && ready, null, { timeout: 30000 });
}
async function press(p, keys) {
  for (const k of keys) { await p.keyboard.down(k); await p.waitForTimeout(200); await p.keyboard.up(k); await p.waitForTimeout(500); }
}

(async () => {
  let browser;
  try { browser = await pw.chromium.launch({ executablePath: fs.existsSync(CHROME) ? CHROME : undefined }); }
  catch (e) { console.log('SKIP: Chrome headless indisponible (' + e.message.split('\n')[0] + ')'); process.exit(77); }
  const ctx = await browser.newContext({ viewport: { width: 1100, height: 800 } });
  const p = await ctx.newPage();
  const url = base + '/phosphoric.html?loci=1';
  try {
    await gotoReady(p, url);
    let txt = await waitScreen(p, /LOCI ROM/, 20000);
    check(/LOCI ROM/.test(txt), 'menu LOCI affiché au boot (?loci=1)');

    const seeded = await p.evaluate(() => FS.readdir('/loci'));
    check(['basic11b.rom', 'basic10.rom', 'microdis.rom', 'locirom'].every(n => seeded.includes(n)),
          'flash /loci semé avec les ROM système');

    // File to import: the real .dsk if provided, otherwise a dummy (only the name is tested).
    let file = realDisk;
    if (!file) { file = path.join(os.tmpdir(), 'webloci.dsk'); fs.writeFileSync(file, Buffer.alloc(256)); }
    const name = path.basename(file);
    await p.setInputFiles('#file-input', file);
    await p.waitForTimeout(1500);
    check((await p.textContent('#status')).includes(name), 'import confirmé dans la barre d\'état');

    await gotoReady(p, url);
    await waitScreen(p, /LOCI ROM/, 20000);
    const after = await p.evaluate(() => FS.readdir('/loci'));
    check(after.includes(name), 'fichier importé persistant après rechargement (IndexedDB)');

    await p.click('#canvas');
    await press(p, ['Space', 'Space']);
    txt = await waitScreen(p, new RegExp(name.replace('.', '\\.')), 5000);
    check(txt.includes(name), 'sélecteur du menu LOCI liste ' + name);

    if (realDisk) {
      await press(p, ['ArrowDown', 'Space', 'Escape']);
      txt = await waitScreen(p, /LOCI ROM/, 1000);
      const t0 = Date.now(); let booted = false;
      while (Date.now() - t0 < 20000) {
        txt = await screenText(p);
        if (!/LOCI ROM/.test(txt) && txt.replace(/\s/g, '').length > 40) { booted = true; break; }
        await p.waitForTimeout(500);
      }
      check(booted, 'disque monté en A: via le menu et démarré (écran du programme)');
    } else {
      console.log('  (boot disque non testé : aucun .dsk réel fourni)');
    }
  } catch (e) {
    check(false, 'exception : ' + e.message);
  }
  await browser.close();
  process.exit(failures ? 1 : 0);
})();
