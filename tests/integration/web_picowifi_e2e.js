// web_picowifi_e2e.js -- picowifi modem of the WASM build (headless Chrome, Playwright).
//
// Usage: node web_picowifi_e2e.js <base-url> <relay-ws-url> <rx-log>
// Exit code: 0 = OK, 1 = failure, 77 = SKIP (Playwright/Chrome not found).
//
// The BASIC programs (generated as auto-run .tap by test_web_picowifi.sh and
// served next to the page) store every byte received from the ACIA starting at
// #4000; it is read back via web_peek. Three scenarios:
//   1. WebSocket relay (?relay=ws://…): ATDT to a local TCP server →
//      CONNECT, banner received, then Oric → server transmission (rx-log trace);
//   2. without relay (?relay=none): ATGET to a CORS HTTP server replayed by
//      fetch(), then ATRD = browser time;
//   3. ProphetOric case (?loci=1&media=…&relay=none&httpsame=…): tape +
//      LOCI (ACIA at $0380), ATD-host:8998 then raw HTTP with ResponseFormat,
//      rewritten to the page's origin; the header must reach the server;
//   4. LOCI + modem by default: --serial-buffer 32 (RX ring of the LOCI
//      firmware); without LOCI, no FIFO.
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

const [base, relay, rxLog] = process.argv.slice(2);
const CHROME = process.env.CHROME || '/usr/bin/google-chrome';
let failures = 0;
function check(ok, msg) { console.log((ok ? '  PASS ' : '  FAIL ') + msg); if (!ok) failures++; }

// Bytes received by the Oric (#4000…), up to the initial RAM fill ($55).
async function rxMem(p) {
  return p.evaluate(() => {
    let s = '';
    for (let a = 0x4000; a < 0x4400; a++) {
      const b = Module.ccall('web_peek', 'number', ['number'], [a]);
      if (b === 0x55 && s.endsWith('UUU')) break;
      s += String.fromCharCode(b);
    }
    return s.replace(/U+$/, '');
  });
}

async function scenario(browser, title, query, until, checks, after) {
  console.log(title);
  const p = await (await browser.newContext()).newPage();
  const logs = []; p.on('console', m => logs.push(m.text()));
  let rx = '';
  try {
    await p.addInitScript(() => sessionStorage.setItem('phos_extra_args', '["--serial-buffer","1024"]'));
    await p.goto(base + '/phosphoric.html?' + query);
    for (let i = 0; i < 480 && !until.test(rx); i++) {
      await p.waitForTimeout(250);
      try { rx = await rxMem(p); } catch (e) {}      // engine not ready yet
    }
  } catch (e) { check(false, 'exception : ' + e.message); }
  let ok = true;
  for (const [re, label] of checks) { const r = re.test(rx); ok = ok && r; check(r, label); }
  if (!ok) {
    console.log('  --- reçu ---\n  ' + JSON.stringify(rx.slice(0, 400)));
    console.log('  --- logs ---\n' + logs.filter(l => /PicoWiFi|picowifi-js|ACIA|LOCI|media|tap/i.test(l)).slice(-12).join('\n'));
  }
  if (after) await after();                         // before closing: the page must be running
  await p.close();
  return rx;
}

(async () => {
  let browser;
  try { browser = await pw.chromium.launch({ executablePath: fs.existsSync(CHROME) ? CHROME : undefined }); }
  catch (e) { console.log('SKIP: Chrome headless indisponible'); process.exit(77); }

  await scenario(browser, '1. relais WebSocket',
    'rom=atmos&loci=0&modem=1&media=relay.tap&relay=' + encodeURIComponent(relay),
    /HELLO-RELAY/, [
      [/CONNECT/, 'ATDT -> CONNECT (TCP ouvert par le relais)'],
      [/HELLO-RELAY/, 'bannière du serveur reçue par l\'Oric (TCP -> WebSocket -> ACIA $031C)'],
    ], async () => {
      let got = '';
      for (let i = 0; i < 240 && !/PING-ORIC/.test(got); i++) {
        try { got = fs.readFileSync(rxLog, 'latin1'); } catch (e) {}
        if (!/PING-ORIC/.test(got)) await new Promise(r => setTimeout(r, 250));
      }
      check(/PING-ORIC/.test(got), 'octets de l\'Oric reçus par le serveur (ACIA -> WebSocket -> TCP)');
    });

  await scenario(browser, '2. sans relais (fetch)',
    'rom=atmos&loci=0&modem=1&media=fetch.tap&relay=none',
    /\d\d-\d\d-\d\d \d\d:\d\d:\d\d/, [
      [/HTTP\/1\.1 200[\s\S]*HELLO-FETCH/, 'ATGET rejoué par fetch() (réponse HTTP reconstituée)'],
      [/\d\d-\d\d-\d\d \d\d:\d\d:\d\d/, 'ATRD = heure du navigateur'],
    ]);

  await scenario(browser, '3. LOCI + cassette + httpsame (cas ProphetOric)',
    'rom=atmos&loci=1&modem=1&media=same.tap&relay=none&httpsame=prophet.example',
    /FORMAT=\S+\r/, [
      [/CONNECT/, 'ATD-prophet.example:8998 -> CONNECT (ACIA $0380 sous LOCI)'],
      [/FORMAT=cli/, 'requête réécrite vers l\'origine, en-tête ResponseFormat transmis'],
    ]);

  // 4. Default LOCI + modem setting, without test argument: 32-byte RX FIFO
  //    like the LOCI firmware (acia.c, ACIA_RX_BUFFER_SIZE).
  console.log('4. LOCI + modem : FIFO RX du firmware');
  {
    const p = await (await browser.newContext()).newPage();
    const logs = []; p.on('console', m => logs.push(m.text()));
    await p.goto(base + '/phosphoric.html?rom=atmos&loci=1&modem=1&relay=none');
    for (let i = 0; i < 80 && !logs.some(l => /RX FIFO enabled/.test(l)); i++) await p.waitForTimeout(250);
    check(logs.some(l => /ACIA RX FIFO enabled: 32 bytes/.test(l)), 'anneau RX 32 octets actif (--serial-buffer 32)');
    await p.close();
    const q = await (await browser.newContext()).newPage();
    const logs2 = []; q.on('console', m => logs2.push(m.text()));
    await q.goto(base + '/phosphoric.html?rom=atmos&loci=0&modem=1&relay=none');
    for (let i = 0; i < 80 && !logs2.some(l => /Serial interface enabled/.test(l)); i++) await q.waitForTimeout(250);
    check(!logs2.some(l => /RX FIFO enabled/.test(l)), 'sans LOCI (6551 nu en $031C) : pas de FIFO');
    await q.close();
  }

  await browser.close();
  process.exit(failures ? 1 : 0);
})();
