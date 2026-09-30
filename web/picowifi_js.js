/* SPDX-License-Identifier: EUPL-1.2 */
/* picowifi_js.js — transport « sans relais » du modem picowifi de la build WASM.
 *
 * Un navigateur n'ouvre pas de TCP brut. Par défaut, le modem passe par un relais
 * WebSocket (tools/picowifi_ws_relay.py) qui sait tout faire. Avec ?relay=none,
 * ce fichier fournit à la place des « sockets virtuelles » (même idée que le
 * neomodem.js de Phosphoneo) :
 *   - HTTP : la requête brute écrite par le modem (ATGET, ATDISKRD/ATDISKWR…)
 *     est rejouée avec fetch() — directement, ou via un proxy HTTP même
 *     origine (?httpproxy=/proxy?url=) pour les sites sans CORS — et la réponse
 *     est rendue en HTTP/1.1 reconstitué (statut, Content-Type, Content-Length,
 *     Content-Range, corps), puis la connexion se ferme. Les en-têtes de la
 *     requête sont transmis (ex. ResponseFormat), sauf ceux que fetch() refuse
 *     ou fixe lui-même (Host, Connection, Content-Length, User-Agent…).
 *     ?httpsame=h1,h2 : une requête vers h1/h2 (quel que soit le port ou le
 *     schéma) part vers location.origin + chemin — utile quand la page est
 *     servie par le même serveur (CSP connect-src 'self', pas de contenu mixte) ;
 *   - DAYTIME (port 13, ATRD/ATRT) : ligne NIST synthétisée depuis l'horloge
 *     du navigateur ;
 *   - tout autre flux (telnet, BBS…) : impossible sans relais → la connexion
 *     est fermée dès le premier octet non-HTTP (le modem répond NO CARRIER).
 *
 * Interface commune avec le transport WebSocket (voir serial_picowifi.c) :
 *   PicoWifiJS.open(host, port, secure, opts) → { q: [Uint8Array], state, send(u8), close() }
 *   state : 0 connexion, 1 ouvert, 2 fermé, 3 échec.
 * Auteur : bmarty <bmarty@mailo.com>
 */
var PicoWifiJS = (function () {
  var enc = new TextEncoder();
  var dec = new TextDecoder('latin1');
  var METHODS = /^(GET|POST|PUT|HEAD|DELETE|OPTIONS|PATCH) /;
  // En-têtes que fetch() refuse ou que le navigateur fixe lui-même : non transmis.
  var FORBIDDEN = /^(host|connection|content-length|user-agent|keep-alive|transfer-encoding|upgrade|te|trailer|expect|cookie|cookie2|date|origin|referer|via|accept-charset|accept-encoding|access-control-request-headers|access-control-request-method|dnt|proxy-.*|sec-.*)$/;

  function concat(a, b) { var r = new Uint8Array(a.length + b.length); r.set(a); r.set(b, a.length); return r; }
  function pad(n, w) { n = String(n); while (n.length < w) n = '0' + n; return n; }

  // Ligne RFC 867 au format NIST : « MJD YY-MM-DD HH:MM:SS TT L H msADV UTC(NIST) * ».
  function daytimeLine(d) {
    var mjd = Math.floor(d.getTime() / 86400000) + 40587;
    return '\n' + mjd + ' ' + pad(d.getUTCFullYear() % 100, 2) + '-' + pad(d.getUTCMonth() + 1, 2) + '-' +
           pad(d.getUTCDate(), 2) + ' ' + pad(d.getUTCHours(), 2) + ':' + pad(d.getUTCMinutes(), 2) + ':' +
           pad(d.getUTCSeconds(), 2) + ' 00 0 0   0.0 UTC(NIST) * \n';
  }

  function open(host, port, secure, opts) {
    opts = opts || {};
    var s = { q: [], state: 1, buf: new Uint8Array(0), busy: false };
    s.close = function () { s.state = 2; };
    s.push = function (u8) { if (u8.length) s.q.push(u8); };

    if (port === 13) {                                  // DAYTIME : heure du navigateur
      s.push(enc.encode(daytimeLine(new Date())));
      s.drainClose = true;                            // ouvert, fermé une fois la ligne lue
      s.send = function () {};
      return s;
    }

    s.send = function (u8) {
      if (s.state !== 1 || s.busy) return;
      s.buf = concat(s.buf, u8);
      var head = dec.decode(s.buf.subarray(0, Math.min(s.buf.length, 8)));
      if (s.buf.length >= 4 && !METHODS.test(head) && !/^[A-Z]{1,7}$/.test(head.trim())) {
        if (opts.log) opts.log('picowifi-js : flux non-HTTP vers ' + host + ':' + port + ' — relais requis');
        s.state = 2; return;
      }
      var txt = dec.decode(s.buf), end = txt.indexOf('\r\n\r\n');
      if (end < 0) return;
      var lines = txt.slice(0, end).split('\r\n'), req = lines[0].split(' ');
      var headers = {}, clen = 0;
      lines.slice(1).forEach(function (l) {
        var i = l.indexOf(':'); if (i < 0) return;
        var k = l.slice(0, i).trim().toLowerCase(), v = l.slice(i + 1).trim();
        if (k === 'content-length') clen = parseInt(v, 10) || 0;
        else if (!FORBIDDEN.test(k)) headers[l.slice(0, i).trim()] = v;   // ex. ResponseFormat, Range
      });
      if (s.buf.length < end + 4 + clen) return;        // corps pas encore complet
      var body = clen ? s.buf.slice(end + 4, end + 4 + clen) : null;
      s.busy = true;
      var defPort = secure ? 443 : 80;
      var url = (secure ? 'https://' : 'http://') + host + (port !== defPort ? ':' + port : '') + req[1];
      var target;
      if (opts.sameHosts && opts.sameHosts.indexOf(host.toLowerCase()) >= 0 && typeof location !== 'undefined')
        target = location.origin + req[1];              // ?httpsame= : même API servie par l'origine de la page
      else
        target = opts.httpProxy ? opts.httpProxy + encodeURIComponent(url) : url;
      fetch(target, { method: req[0], headers: headers, body: body, cache: 'no-store' })
        .then(function (r) {
          return r.arrayBuffer().then(function (ab) {
            var b = new Uint8Array(ab);
            var h = 'HTTP/1.1 ' + r.status + ' ' + (r.statusText || 'OK') + '\r\n' +
                    'Content-Type: ' + (r.headers.get('Content-Type') || 'application/octet-stream') + '\r\n' +
                    'Content-Length: ' + b.length + '\r\n' +
                    (r.headers.get('Content-Range') ? 'Content-Range: ' + r.headers.get('Content-Range') + '\r\n' : '') +
                    'Connection: close\r\n\r\n';
            s.push(enc.encode(h)); s.push(b);
          });
        })
        .catch(function (e) {
          if (opts.log) opts.log('picowifi-js : fetch ' + url + ' impossible (' + e + ') — CORS ? essayez ?httpproxy= ou le relais');
          s.push(enc.encode('HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n'));
        })
        .then(function () { s.state = 2; });
    };
    return s;
  }
  return { open: open, daytimeLine: daytimeLine };
})();
