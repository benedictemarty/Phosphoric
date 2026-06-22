# Débogage du 6502 via GDB remote (`--gdb`)

Phosphoric embarque un serveur **GDB Remote Serial Protocol (RSP)** : on attache
`gdb`, `lldb` ou un IDE (VS Code, CLion) à l'Oric émulé pour poser des
breakpoints, single-stepper et inspecter/modifier registres et mémoire du 6502.
Aucun autre émulateur Oric n'offre cela.

## Démarrer

```bash
./oric1-emu -r roms/basic11b.rom --gdb           # port 1234 (défaut)
./oric1-emu -r roms/basic11b.rom --gdb=3333      # port au choix
```

L'émulateur ouvre le port et **attend** la connexion du client. La machine
démarre arrêtée au vecteur de reset ; c'est GDB qui pilote l'exécution.

## Attacher GDB

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

> `gdb` mainline ne connaît pas l'architecture `mos6502` : il peut émettre un
> avertissement, mais l'accès mémoire / breakpoints / step fonctionnent. La
> description des registres est fournie par le stub via `target.xml`.

## Modèle d'exécution

- Les breakpoints GDB et le REPL natif partagent le **même** `debugger_t` :
  `Z0`/`z0` ajoutent/retirent dans la même table que la commande `b`.
- **Ctrl-C** dans GDB interrompt l'exécution (signal SIGINT, `S02`) ; la latence
  est d'au plus une frame (~20 ms).
- Une déconnexion du client laisse l'Oric reprendre librement.

## Commandes RSP gérées

`?` · `g`/`G` · `p`/`P` · `m`/`M` · `c`/`s` · `Z0`/`z0`, `Z1`/`z1` (breakpoints) ·
`Z2`/`z2` (write watchpoint) · `H` · `D` · `k` ·
`qSupported`, `qAttached`, `qC`, `qfThreadInfo`/`qsThreadInfo`, `qOffsets`,
`qSymbol`, `qXfer:features:read:target.xml` · `QStartNoAckMode` · `vCont?`/`vCont`.

Bloc registres (`g`/`G`) : `A X Y SP PClo PChi P` (7 octets, PC little-endian).

## Notes

- La lecture mémoire (`m`) est **sans effet de bord** : la zone $0000-$BFFF est
  lue dans la RAM (les registres I/O VIA/ACIA ne sont jamais touchés), et
  $C000-$FFFF via la vue CPU (ROM/overlay).
- Le transport est du TCP brut (POSIX sockets), aucune dépendance externe.
