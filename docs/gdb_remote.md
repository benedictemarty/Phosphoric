# Débogage du 6502 via GDB remote (`--gdb`)

Phosphoric embarque un serveur **GDB Remote Serial Protocol (RSP)** : un client RSP
qui connaît le 6502 peut s'y attacher pour poser des breakpoints, single-stepper et
inspecter/modifier registres et mémoire.

> ⚠️ **Le `gdb` du PC ne fonctionne pas.** Testé le 2026-10-31 avec GNU gdb 16.3
> (Ubuntu) : il refuse l'architecture `mos6502` du `target.xml` (« Target description
> specified unknown architecture "mos6502" »), retombe sur l'architecture du PC
> (x86-64), puis échoue sur le bloc registres de 7 octets (« Truncated register 1 in
> remote 'g' packet ») ; même la mémoire est alors inaccessible (« Cannot access
> memory »). `gdb-multiarch` (non installé ici), `lldb` et les IDE (VS Code,
> CLion) : **non vérifiés**. Aucun client 6502
> n'a été vérifié avec ce stub ; il reste testé au niveau du protocole
> (`make test-gdbstub`).
>
> **Pour déboguer, utiliser [`--control`](control_protocol.md)** (protocole texte
> sur stdin/stdout : `regs`, `read`, `write`, `break`, `step`, `next`, `continue`,
> `watch`…, celui qu'utilise un éditeur), ou le débogueur intégré (`--debug`, F9).

## Démarrer

```bash
./oric1-emu -r roms/basic11b.rom --gdb           # port 1234 (défaut)
./oric1-emu -r roms/basic11b.rom --gdb=3333      # port au choix
```

L'émulateur ouvre le port et **attend** la connexion du client. La machine
démarre arrêtée au vecteur de reset ; c'est le client qui pilote l'exécution.

## Attacher un client

Avec un `gdb` construit pour le 6502 (non fourni, non vérifié) :

```bash
gdb -ex 'target remote :1234'
```

Puis, dans GDB :

```
(gdb) info registers          # A X Y SP PC P
(gdb) x/8xb 0xfffc            # lire la mémoire (vecteurs)
(gdb) break *0xc000           # breakpoint sur une adresse
(gdb) continue
(gdb) stepi                   # un pas d'instruction
(gdb) set $pc = 0x0400        # forcer le PC
(gdb) set {char}0x0400 = 0xa9 # écrire un octet
(gdb) detach                  # se détacher (l'Oric continue)
```

> La description des registres est fournie par le stub via `target.xml`
> (architecture `mos6502`). Le `gdb` mainline ne la connaît pas : voir l'avertissement
> en tête de page, rien ne fonctionne avec lui (ni registres, ni mémoire).

## Modèle d'exécution

- Les breakpoints GDB et le REPL natif partagent le **même** `debugger_t` :
  `Z0`/`z0` ajoutent/retirent dans la même table que la commande `b`.
- **Ctrl-C** dans GDB interrompt l'exécution (signal SIGINT, `S02`) ; la latence
  est d'au plus une frame (~20 ms).
- Une déconnexion du client laisse l'Oric reprendre librement.

## Commandes RSP gérées

`?` · `g`/`G` · `p`/`P` · `m`/`M` · `c`/`s` · `Z0`/`z0`, `Z1`/`z1` (breakpoints) ·
`Z2`/`z2` (write watch), `Z3`/`z3` (read watch), `Z4`/`z4` (access watch) · `H` · `D` · `k` ·
`qSupported`, `qAttached`, `qC`, `qfThreadInfo`/`qsThreadInfo`, `qOffsets`,
`qSymbol`, `qXfer:features:read:target.xml` · `QStartNoAckMode` · `vCont?`/`vCont`.

Bloc registres (`g`/`G`) : `A X Y SP PClo PChi P` (7 octets, PC little-endian).

## Notes

- La lecture mémoire (`m`) est **sans effet de bord** : la zone $0000-$BFFF est
  lue dans la RAM (les registres I/O VIA/ACIA ne sont jamais touchés), et
  $C000-$FFFF via la vue CPU (ROM/overlay).
- Le transport est du TCP brut (POSIX sockets), aucune dépendance externe.
