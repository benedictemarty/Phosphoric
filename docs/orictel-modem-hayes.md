# Phosphoric — Support modem Hayes AT pour OricTel

Bonjour,

Phosphoric supporte maintenant un modem Hayes complet avec commandes AT, utilisable depuis le BASIC ou le code assembleur ORIC.

## Lancement

```bash
# Mode commande pur — le programme ORIC choisit la destination via ATD
./phosphoric -r roms/basic10.rom --serial modem

# Host prédéfini — ATD seul connecte à ce host
./phosphoric -r roms/basic10.rom --serial modem:bbs.host:23

# Mode serveur BBS — attend des connexions entrantes
./phosphoric -r roms/basic10.rom --serial modem:listen:2323
```

## Commandes AT supportées

| Commande | Action | Réponse |
|----------|--------|---------|
| `AT` | Test modem | `OK` |
| `ATZ` | Reset modem | `OK` |
| `ATE0` / `ATE1` | Echo off / on | `OK` |
| `ATH` | Raccrocher | `OK` |
| `ATD host:port` | Connecter via TCP | `CONNECT` ou `NO CARRIER` |
| `ATDT host:port` | Idem (T ignoré) | `CONNECT` ou `NO CARRIER` |
| `ATA` | Accepter connexion entrante (mode listen) | `CONNECT` ou `ERROR` |
| `ATS0=N` | Auto-answer après N rings | `OK` |
| `+++` | Escape data → command mode (guard time) | `OK` |

## Exemple BASIC

```basic
10 DA=796:ST=797:CM=798:CT=799
20 POKE ST,0:POKE CT,31:POKE CM,3
30 REM == DIAL ==
40 A$="ATD pavi.3617.fr:3617"+CHR$(13)
50 FOR I=1 TO LEN(A$)
60 POKE DA,ASC(MID$(A$,I,1))
70 FOR W=1 TO 100:NEXT W
80 NEXT I
90 REM == ATTENDRE CONNECT ==
100 R$=""
110 S=PEEK(ST):IF (S AND 8)=0 THEN 110
120 C=PEEK(DA)
130 IF C=10 THEN 110
140 IF C=13 THEN 160
150 R$=R$+CHR$(C):GOTO 110
160 IF R$="CONNECT" THEN PRINT"CONNECTE!":GOTO 200
170 PRINT R$:END
200 REM == TERMINAL ==
210 S=PEEK(ST)
220 IF (S AND 8)=8 THEN C=PEEK(DA):IF C>31 AND C<127 THEN PRINT CHR$(C);
230 IF (S AND 8)=8 THEN IF C=13 THEN PRINT
240 K$=KEY$:IF K$="" THEN 210
250 IF ASC(K$)=27 THEN 300
260 POKE DA,ASC(K$):GOTO 210
300 REM == HANGUP ==
310 FOR W=1 TO 500:NEXT W
320 A$="+++":FOR I=1 TO 3:POKE DA,43:FOR W=1 TO 200:NEXT W:NEXT I
330 FOR W=1 TO 500:NEXT W
340 A$="ATH"+CHR$(13)
350 FOR I=1 TO LEN(A$):POKE DA,ASC(MID$(A$,I,1)):FOR W=1 TO 100:NEXT W:NEXT I
360 PRINT"DECONNECTE":END
```

## Autres options disponibles

```bash
--serial-buffer 256      RX FIFO (évite overrun pendant clear screen)
--serial-irq-on-rdrf     Mode WDC 65C51 (IRQ re-trigger tant que RDRF set)
--serial-trace FILE      Debug trace TX/RX avec timestamps CPU
--serial-v23             Mode V23 1200/75 bauds (Minitel)
```

## Fix AY-3-8912 registre 7 (inclus dans cette version)

Le bug clavier après modification du registre 7 est corrigé. Phosphoric bloque maintenant le scan clavier quand le Port A du PSG est en output (reg7 bit 6 = 0), comme Oricutron et le vrai hardware. Le workaround `ay_write(7, $7F)` reste nécessaire côté programme.

---

bmarty — Phosphoric v1.15.1-alpha, 303 tests
