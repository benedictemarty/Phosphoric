// web_iomenu_e2e.js — peripherals menu (F1) in the WASM build (headless Chrome).
//
// Usage: node web_iomenu_e2e.js <base-url> [capture.png]
// Exit code: 0 = OK, 1 = failure, 77 = SKIP (Playwright / Chrome not found).
//
// Checks, in the browser:
//   1. F1 (physical keyboard) opens the menu and does not reach the browser
//      (preventDefault: no help); the I/O button lights up;
//   2. machine frozen while the menu is open (VIA Timer 1 still), resumed on close;
//   3. the on-screen keyboard keys drive the menu (RETURN on "Reprendre");
//   4. the I/O button opens and closes the menu.
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
// VIA Timer 1 ($0304/$0305): counts down every cycle while the machine runs.
const t1 = p => p.evaluate(() => Module.ccall('web_peek', 'number', ['number'], [0x0304]) |
                              (Module.ccall('web_peek', 'number', ['number'], [0x0305]) << 8));
// The web build advances one frame per loop turn (≈ 20 ms), but a loaded
// headless Chrome may run none for several hundred ms: expected states (menu
// open, machine resumed…) are therefore WAITED FOR, up to 10 s, instead of being
// read after a fixed delay (intermittent failure under load). Only "machine
// frozen" remains an observation over a fixed time: under load it can only pass
// wrongly, never fail wrongly.
const WAIT_MS = 10000;
async function until(p, cond) {
  const end = Date.now() + WAIT_MS;
  do {
    if (await cond()) return true;
    await p.waitForTimeout(100);
  } while (Date.now() < end);
  return false;
}
async function running(p) {           // does Timer 1 move? (up to WAIT_MS)
  const a = await t1(p);
  return until(p, async () => (await t1(p)) !== a);
}
async function frozen(p) {            // Timer 1 still for 1 s
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
    await p.waitForTimeout(3000);                       // BASIC boot
    check(await running(p), 'machine en marche avant le menu');

    // F1: the event is "consumed" (no browser help).
    await p.evaluate(() => { window.__f1 = null;
      window.addEventListener('keydown', e => { if (e.key === 'F1') window.__f1 = e.defaultPrevented; }); });
    await p.click('#canvas');
    await p.keyboard.press('F1');
    check(await until(p, () => menuOpen(p)), 'F1 ouvre le menu');
    check(await p.evaluate(() => window.__f1) === true, 'F1 soustrait au navigateur (preventDefault)');
    // Le bouton suit l'activité lue périodiquement par la page : attendu aussi.
    check(await until(p, () => p.evaluate(() =>
            document.getElementById('btn-iomenu').classList.contains('on'))),
          'bouton I/O allumé');
    check(await frozen(p), 'machine figée pendant le menu');
    if (shot) await p.screenshot({ path: shot });

    // On-screen keyboard: RETURN on "Reprendre" (cursor on open) closes the menu.
    await p.evaluate(() => { Module.ccall('web_key', null, ['number','number','number','number','number'], [13,0,0,0,1]);
                             Module.ccall('web_key', null, ['number','number','number','number','number'], [13,0,0,0,0]); });
    check(await until(p, async () => !(await menuOpen(p))),
          'clavier virtuel : RETURN sur « Reprendre » ferme le menu');
    check(await running(p), 'machine relancée après fermeture');

    // I/O button: opens then closes.
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
