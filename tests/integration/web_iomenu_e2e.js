// web_iomenu_e2e.js — menu des périphériques (F1) dans la build WASM (Chrome headless).
//
// Usage : node web_iomenu_e2e.js <base-url> [capture.png]
// Code de sortie : 0 = OK, 1 = échec, 77 = SKIP (Playwright / Chrome introuvable).
//
// Vérifie, dans le navigateur :
//   1. F1 (clavier physique) ouvre le menu et n'atteint pas le navigateur
//      (preventDefault : pas d'aide) ; le bouton I/O s'allume ;
//   2. machine figée menu ouvert (Timer 1 du VIA immobile), relancée à la fermeture ;
//   3. les touches du clavier virtuel pilotent le menu (RETURN sur « Reprendre ») ;
//   4. le bouton I/O ouvre et ferme le menu.
'use strict';
const path = require('path');
const fs = require('fs');
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
const shot = process.argv[3] || null;
const CHROME = process.env.CHROME || '/usr/bin/google-chrome';
let failures = 0;
function check(ok, msg) { console.log((ok ? '  PASS ' : '  FAIL ') + msg); if (!ok) failures++; }

const activity = p => p.evaluate(() => Module.ccall('web_io_activity', 'number', [], []) || 0);
const menuOpen = async p => ((await activity(p)) & 4) !== 0;
// Timer 1 du VIA ($0304/$0305) : décompte à chaque cycle quand la machine tourne.
const t1 = p => p.evaluate(() => Module.ccall('web_peek', 'number', ['number'], [0x0304]) |
                              (Module.ccall('web_peek', 'number', ['number'], [0x0305]) << 8));
async function running(p) {
  const a = await t1(p); await p.waitForTimeout(250);
  const b = await t1(p); await p.waitForTimeout(250);
  const c = await t1(p);
  return a !== b || b !== c;
}

(async () => {
  let browser;
  try { browser = await pw.chromium.launch({ executablePath: fs.existsSync(CHROME) ? CHROME : undefined }); }
  catch (e) { console.log('SKIP: Chrome headless indisponible (' + e.message.split('\n')[0] + ')'); process.exit(77); }
  const ctx = await browser.newContext({ viewport: { width: 1100, height: 800 } });
  const p = await ctx.newPage();
  try {
    await p.goto(base + '/phosphoric.html');
    await p.waitForFunction(() => typeof ready !== 'undefined' && ready, null, { timeout: 20000 });
    await p.waitForTimeout(3000);                       // boot BASIC
    check(await running(p), 'machine en marche avant le menu');

    // F1 : l'événement est « consommé » (pas d'aide du navigateur).
    await p.evaluate(() => { window.__f1 = null;
      window.addEventListener('keydown', e => { if (e.key === 'F1') window.__f1 = e.defaultPrevented; }); });
    await p.click('#canvas');
    await p.keyboard.press('F1');
    await p.waitForTimeout(600);
    check(await menuOpen(p), 'F1 ouvre le menu');
    check(await p.evaluate(() => window.__f1) === true, 'F1 soustrait au navigateur (preventDefault)');
    check(await p.evaluate(() => document.getElementById('btn-iomenu').classList.contains('on')),
          'bouton I/O allumé');
    check(!(await running(p)), 'machine figée pendant le menu');
    if (shot) await p.screenshot({ path: shot });

    // Clavier virtuel : RETURN sur « Reprendre » (curseur à l'ouverture) ferme le menu.
    await p.evaluate(() => { Module.ccall('web_key', null, ['number','number','number','number','number'], [13,0,0,0,1]);
                             Module.ccall('web_key', null, ['number','number','number','number','number'], [13,0,0,0,0]); });
    await p.waitForTimeout(400);
    check(!(await menuOpen(p)), 'clavier virtuel : RETURN sur « Reprendre » ferme le menu');
    check(await running(p), 'machine relancée après fermeture');

    // Bouton I/O : ouvre puis ferme.
    await p.click('#btn-iomenu');
    await p.waitForTimeout(400);
    check(await menuOpen(p), 'bouton I/O ouvre le menu');
    await p.click('#btn-iomenu');
    await p.waitForTimeout(400);
    check(!(await menuOpen(p)), 'bouton I/O ferme le menu');
  } catch (e) {
    check(false, 'exception : ' + e.message);
  }
  await browser.close();
  console.log(failures ? `=== ${failures} échec(s) ===` : '=== OK ===');
  process.exit(failures ? 1 : 0);
})();
