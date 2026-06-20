10 REM ===== KEY DETECT (Phosphoric) =====
20 REM Affiche le code decimal de chaque touche pressee.
30 PRINT "KEY DETECT - PRESS KEYS (CTRL-C=STOP)"
40 K$=KEY$ : IF K$="" THEN 40
50 PRINT ASC(K$),
60 K$=KEY$ : IF K$<>"" THEN 60
70 GOTO 40
