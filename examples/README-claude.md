# Claude sur Oric-1

Un client de chat pour l'**API Anthropic** qui tourne sur un Oric-1/Atmos émulé
et parle au monde réel via le **PicoWiFiModemUSB** physique (USB CDC), relié à
l'ACIA exposée par LOCI à **$0380**. Le Pico termine le **TLS** : l'Oric n'envoie
que du texte en clair (`ATPOST`).

```
Oric-1 (BASIC)  --AT/$0380-->  Pico WiFi reel  --HTTPS/TLS-->  api.anthropic.com
```

## Fichiers

| Fichier | Rôle |
|---|---|
| `claude.bas` | Le programme Oric BASIC (source) |
| `claude.tap` | Cassette générée (`bas2tap ... --auto-run`) |
| `run-claude.sh` | Lance l'émulateur câblé sur le Pico réel |

## Mise en route

1. **Brancher** le Pico WiFi (`lsusb` → `cafe:4001 TinyUSB PicoWifiModemUSB`,
   apparaît en `/dev/ttyACM0`).
2. **Connecter le Pico au WiFi** une fois (persistant en NVRAM) :
   `AT$SSID=...`, `AT$PASS=...`, `ATC1` — ou via `picowifi/oric/wificonf.bas`.
3. **Clé API** : éditer `claude.bas` ligne 40 :
   `40 KY$="sk-ant-api03-..."` puis régénérer la cassette :
   `./bas2tap examples/claude.bas -o examples/claude.tap --auto-run`
4. **Lancer** : `examples/run-claude.sh`

À l'invite `VOUS>`, tapez une question ; `BYE` pour quitter.

## Comment ça marche (protocole)

- Init ACIA $0380 : `POKE 897,0 : POKE 899,30 : POKE 898,3` (reset / 9600 8N1 / DTR).
- `ATPOSThttps://api.anthropic.com/v1/messages` puis, segmentés par **LF** :
  en-têtes (`x-api-key`, `anthropic-version: 2023-06-01`, `content-type`),
  ligne vide, corps JSON sur **une seule ligne**, puis une ligne `.`.
- Le firmware calcule `Content-Length`, ajoute `Host`/`Connection: close`, fait
  le TLS, et renvoie la réponse **en clair** sur la série.
- L'Oric scanne le flux pour le marqueur `"text":"` et affiche la réponse de
  l'assistant jusqu'au guillemet de fin (déséchappement `\n`, `\"`, `\\`).

## Limites connues

- **`INPUT`** coupe la saisie sur une virgule (BASIC) : évitez les virgules dans
  la question, ou enrichir avec une saisie `GET` clavier.
- **Réponses longues** : l'API répond en `Transfer-Encoding: chunked`. Le cadrage
  des chunks (`<taille hex>\r\n`) peut s'insérer dans une réponse très longue. Le
  prompt système borne déjà la réponse (ASCII, 6 lignes max) → un seul chunk en
  pratique. Pour du robuste : dé-chunker côté Oric.
- Le jeu de caractères Oric est ASCII majuscule : le prompt système demande à
  Claude de répondre en majuscules sans accents.

## Validé

- ✅ Chargement + exécution (BASIC 1.1, ACIA $0380).
- ✅ `ATE0`/`ATC1`/`ATI` contre le **vrai** Pico → connexion WiFi réelle + IP.
- ✅ `ATPOST` HTTPS vers `api.anthropic.com` : handshake **TLS OK** (CA reconnu),
  format en-têtes/corps accepté (401 `invalid x-api-key` avec une fausse clé).
- ⏳ Réponse texte complète : nécessite une vraie clé API (appel facturé).
