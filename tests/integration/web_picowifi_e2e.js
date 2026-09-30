// web_picowifi_e2e.js — modem picowifi de la build WASM (Chrome headless, Playwright).
//
// Usage : node web_picowifi_e2e.js <base-url> <relay-ws-url> <rx-log>
// Code de sortie : 0 = OK, 1 = échec, 77 = SKIP (Playwright/Chrome introuvable).
//
// Les programmes BASIC (générés en .tap auto-run par test_web_picowifi.sh et
// servis à côté de la page) rangent tout octet reçu de l'ACIA à partir de
// #4000 ; on le relit via web_peek. Trois scénarios :
//   1. relais WebSocket (?relay=ws://…) : ATDT vers un serveur TCP local →
//      CONNECT, bannière reçue, puis émission Oric → serveur (trace rx-log) ;
//   2. sans relais (?relay=none) : ATGET vers un serveur HTTP CORS rejoué par
//      fetch(), puis ATRD = heure du navigateur ;
//   3. cas ProphetOric (?loci=1&media=…&relay=none&httpsame=…) : cassette +
//      LOCI (ACIA en $0380), ATD-hôte:8998 puis HTTP brut avec ResponseFormat,
//      réécrit vers l'origine de la page ; l'en-tête doit arriver au serveur.
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

// Octets reçus par l'Oric (#4000…), jusqu'au remplissage RAM initial ($55).
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
      try { rx = await rxMem(p); } catch (e) {}      // moteur pas encore prêt
    }
  } catch (e) { check(false, 'exception : ' + e.message); }
  let ok = true;
  for (const [re, label] of checks) { const r = re.test(rx); ok = ok && r; check(r, label); }
  if (!ok) {
    console.log('  --- reçu ---\n  ' + JSON.stringify(rx.slice(0, 400)));
    console.log('  --- logs ---\n' + logs.filter(l => /PicoWiFi|picowifi-js|ACIA|LOCI|media|tap/i.test(l)).slice(-12).join('\n'));
  }
  if (after) await after();                         // avant fermeture : la page doit tourner
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

  await browser.close();
  process.exit(failures ? 1 : 0);
})();
