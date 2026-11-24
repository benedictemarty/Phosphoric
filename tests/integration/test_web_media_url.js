// Un ?media= vers un autre fichier que le média mémorisé doit l'emporter (sinon l'ancien
// média, ex. une démo, démarre et le nouveau n'est inséré qu'à chaud). Exécute l'en-tête
// de web/shell.js avec un sessionStorage et une URL simulés.
const fs = require('fs'), vm = require('vm');
const src = fs.readFileSync(__dirname + '/../../web/shell.js', 'utf8');
const head = src.slice(src.indexOf("var urlRom"), src.indexOf("var args"));
function run(search, store) {
  const ss = { getItem: k => (k in store ? store[k] : null), removeItem: k => { delete store[k]; } };
  const ctx = { location: { search }, sessionStorage: ss, URLSearchParams };
  vm.createContext(ctx); vm.runInContext(head, ctx);
  return { name: ctx.mediaName, kind: ctx.mediaKind };
}
let fail = 0;
function check(label, got, want) { const ok = JSON.stringify(got) === JSON.stringify(want); console.log((ok ? 'PASS ' : 'FAIL ') + label); if (!ok) { console.log(' ', JSON.stringify(got), '!=', JSON.stringify(want)); fail++; } }
const old = () => ({ phos_media_name: 'oricdemo_floppy.dsk', phos_media_kind: 'dsk' });
check('autre fichier dans ?media= : média mémorisé oublié', run('?media=%2Ffiles%2Fx%2F0%2Fjeu.dsk', old()), { name: '', kind: 'dsk' });
check('même fichier (rechargement) : média mémorisé gardé', run('?media=%2Ffiles%2Fx%2F1%2Foricdemo_floppy.dsk', old()), { name: 'oricdemo_floppy.dsk', kind: 'dsk' });
check('sans ?media= : média mémorisé gardé', run('', old()), { name: 'oricdemo_floppy.dsk', kind: 'dsk' });
check('?media= .tap sur mémoire .dsk : type déduit de l\'URL', run('?media=a%2Fb.tap', old()), { name: '', kind: 'tap' });
process.exit(fail ? 1 : 0);
