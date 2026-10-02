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
// La version web avance d'une trame à chaque tour de boucle (≈ 20 ms), mais un
// Chrome headless chargé peut n'en faire aucune pendant plusieurs centaines de
// ms : les états attendus (menu ouvert, machine relancée…) sont donc ATTENDUS
// jusqu'à 10 s au lieu d'être lus après un délai fixe (échec intermittent sous
// charge). Seule « machine figée » reste une observation sur une durée fixe :
// sous charge elle ne peut que réussir à tort, jamais échouer à tort.
const WAIT_MS = 10000;
async function until(p, cond) {
  const end = Date.now() + WAIT_MS;
  do {
    if (await cond()) return true;
    await p.waitForTimeout(100);
  } while (Date.now() < end);
  return false;
}
async function running(p) {           // Timer 1 bouge-t-il ? (jusqu'à WAIT_MS)
  const a = await t1(p);
  return until(p, async () => (await t1(p)) !== a);
}
async function frozen(p) {            // Timer 1 immobile pendant 1 s
  const a = await t1(p);
  for (let i = 0; i < 10; i++) {
    await p.waitForTimeout(100);
    if ((await t1(p)) !== a) return false;
  }
  return true;
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
    check(await until(p, () => menuOpen(p)), 'F1 ouvre le menu');
    check(await p.evaluate(() => window.__f1) === true, 'F1 soustrait au navigateur (preventDefault)');
    check(await p.evaluate(() => document.getElementById('btn-iomenu').classList.contains('on')),
          'bouton I/O allumé');
    check(await frozen(p), 'machine figée pendant le menu');
    if (shot) await p.screenshot({ path: shot });

    // Clavier virtuel : RETURN sur « Reprendre » (curseur à l'ouverture) ferme le menu.
    await p.evaluate(() => { Module.ccall('web_key', null, ['number','number','number','number','number'], [13,0,0,0,1]);
                             Module.ccall('web_key', null, ['number','number','number','number','number'], [13,0,0,0,0]); });
    check(await until(p, async () => !(await menuOpen(p))),
          'clavier virtuel : RETURN sur « Reprendre » ferme le menu');
    check(await running(p), 'machine relancée après fermeture');

    // Bouton I/O : ouvre puis ferme.
    await p.click('#btn-iomenu');
    check(await until(p, () => menuOpen(p)), 'bouton I/O ouvre le menu');
    await p.click('#btn-iomenu');
    check(await until(p, async () => !(await menuOpen(p))), 'bouton I/O ferme le menu');
  } catch (e) {
    check(false, 'exception : ' + e.message);
  }
  await browser.close();
  console.log(failures ? `=== ${failures} échec(s) ===` : '=== OK ===');
  process.exit(failures ? 1 : 0);
})();
