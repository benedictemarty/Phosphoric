#!/usr/bin/env bash
#
# test_ula_ng_visible.sh — end-to-end ULA-NG video tests via --ula-ng-poke.
#
# Programs the ULA-NG registers directly at startup (no BASIC POKEs) and checks
# the rendered framebuffer (PPM screenshot). Guards the full video pipeline:
# palette-indirection (§5.1), start-address (§5.3), copper (§5.4).
#
# Author: bmarty <bmarty@mailo.com>

set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EMU="./oric1-emu"
ROM="roms/basic11b.rom"
TMP="$(mktemp -d)"
pass=0; fail=0
ok() { printf '  [OK]   %s\n' "$*"; pass=$((pass+1)); }
ko() { printf '  [FAIL] %s\n' "$*"; fail=$((fail+1)); }
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

echo "=== ULA-NG visible (end-to-end via --ula-ng-poke) ==="

[ -f "$ROM" ] || ROM="roms/basic10.rom"
[ -f "$ROM" ] || { echo "  (skip: no ROM)"; exit 0; }
[ -x "$EMU" ] || { echo "  (skip: build $EMU first)"; exit 0; }

shot() { # <name> <ula-ng-poke seq | ""> -> $TMP/<name>.ppm
    local out="$TMP/$1.ppm"; shift
    local seq="$1"
    if [ -n "$seq" ]; then
        "$EMU" -r "$ROM" -n --ula-ng-poke "$seq" --screenshot-at 4000000:"$out" -c 4500000 >/dev/null 2>&1
    else
        "$EMU" -r "$ROM" -n --screenshot-at 4000000:"$out" -c 4500000 >/dev/null 2>&1
    fi
    [ -s "$out" ]
}

shot base "" || { ko "baseline screenshot"; echo "=== result: $pass passed, $fail failed ==="; [ "$fail" -eq 0 ]; exit; }
ok "baseline screenshot rendered"

# palette : couleur 7 (blanc) -> vert
shot pal "340=4E,340=47,341=01,348=07,349=00,34A=F0"
# copper : couleur 7 rouge (ligne 0) puis bleu (ligne $1E=30)
shot cop "340=4E,340=47,341=01,34B=00,34C=00,34C=7F,34C=00,34C=1E,34C=70,34C=0F"
# start-address : scroll d'une rangée ($BB80+40 = $BBA8)
shot scr "340=4E,340=47,341=01,342=A8,343=BB"
# scroll fin Y = 4 px ($0345=04)
shot scy "340=4E,340=47,341=01,345=04"
# scroll fin X = 3 px ($0344=03)
shot scx "340=4E,340=47,341=01,344=03"
# attributs parallèles : NG_MODE.b1 + fill papier bleu(4)/encre rouge(1) = $21
shot atr "340=4E,340=47,341=02,34D=21"
# sprites : sprite 0 (16x16) rempli d'index 1 (rouge), enable global + sprite, à (100,100)
SPRSEQ="340=4E,340=47,350=01,351=00,352=64,353=64,354=01"
for _i in $(seq 1 256); do SPRSEQ="$SPRSEQ,355=01"; done
shot spr "$SPRSEQ"
# chunky 4bpp (§5.8) : NG_MODE=0x05 (enable+chunky) + palette index 0 = magenta → 320px
shot chunky "340=4E,340=47,341=05,348=00,349=0F,34A=0F"
# texte 80 colonnes (§5.8) : NG_MODE=0x09 (enable+text80) → 480px
shot t80 "340=4E,340=47,341=09"

python3 - "$TMP" <<'PY'
import sys
TMP = sys.argv[1]
def load(p):
    d=open(p,"rb").read(); i=2
    def tok(i):
        while d[i] in b' \t\n\r': i+=1
        s=i
        while d[i] not in b' \t\n\r': i+=1
        return d[s:i],i
    w,i=tok(i);h,i=tok(i);m,i=tok(i);i+=1
    return int(w),int(h),d[i:]
W,H,base=load(f"{TMP}/base.ppm")
_,_,pal=load(f"{TMP}/pal.ppm")
_,_,cop=load(f"{TMP}/cop.ppm")
_,_,scr=load(f"{TMP}/scr.ppm")
_,_,scy=load(f"{TMP}/scy.ppm")
_,_,scx=load(f"{TMP}/scx.ppm")
_,_,atr=load(f"{TMP}/atr.ppm")
_,_,spr=load(f"{TMP}/spr.ppm")
cW,cH,chunky=load(f"{TMP}/chunky.ppm")
tW,tH,t80=load(f"{TMP}/t80.ppm")
res=[]
# palette : les blancs de la ref deviennent verts
g=sum(1 for o in range(0,W*H*3,3) if base[o]==0xFF and base[o+1]==0xFF and base[o+2]==0xFF and (pal[o],pal[o+1],pal[o+2])==(0,0xFF,0))
res.append(("palette: white->green (%d px)"%g, g>1000))
# copper : bandeau rouge (haut), bytes-free bleu (bas)
def band(fb,y0,y1,col):
    n=0
    for y in range(y0,y1):
        for x in range(W):
            o=(y*W+x)*3
            if base[o]==0xFF and base[o+1]==0xFF and base[o+2]==0xFF and (fb[o],fb[o+1],fb[o+2])==col: n+=1
    return n
red=band(cop,8,24,(0xFF,0,0)); blue=band(cop,40,48,(0,0,0xFF))
res.append(("copper: red top (%d) + blue bottom (%d)"%(red,blue), red>100 and blue>50))
# start-address : rangee texte 0 du decale == rangee 1 de la ref
row=W*3
res.append(("start-address: scrolled row0 == base row1", scr[0:8*row]==base[8*row:16*row]))
# scroll fin Y=4 : contenu decale de 4 px vers le haut (scy[R] == base[R+4])
res.append(("fine scroll Y=4: content shifted up 4px", scy[8*row:40*row]==base[12*row:44*row]))
# scroll fin X=3 : contenu decale de 3 px vers la gauche (scx[x] == base[x+3])
okX=True
for y in range(8,40):
    b=y*W*3
    if scx[b:b+(W-3)*3] != base[b+9:b+9+(W-3)*3]: okX=False; break
res.append(("fine scroll X=3: content shifted left 3px", okX))
# attributs // : zone principale (y<200) entierement papier bleu + encre rouge (aucun clash)
ablue=ared=aother=0
for y in range(200):
    for x in range(W):
        o=(y*W+x)*3
        c=(atr[o],atr[o+1],atr[o+2])
        if c==(0,0,0xFF): ablue+=1
        elif c==(0xFF,0,0): ared+=1
        else: aother+=1
res.append(("parallel attrs: main area = blue paper(%d)+red ink(%d), other=%d"%(ablue,ared,aother),
            ablue>40000 and ared>500 and aother==0))
# sprites : bloc 16x16 rouge (index 1) à (100,100), composé sur le fond
sred=0
for y in range(100,116):
    for x in range(100,116):
        o=(y*W+x)*3
        if (spr[o],spr[o+1],spr[o+2])==(0xFF,0,0): sred+=1
res.append(("sprite: 16x16 red block at (100,100) = %d/256 px"%sred, sred==256))
# chunky 4bpp : résolution 320 large + palette index 0 (magenta) dominant
cmag=sum(1 for o in range(0,cW*cH*3,3) if (chunky[o],chunky[o+1],chunky[o+2])==(0xFF,0,0xFF))
res.append(("chunky 4bpp: %dx%d, magenta(idx0)=%d px"%(cW,cH,cmag), cW==320 and cmag>10000))
# texte 80 colonnes : résolution 480 large + caractères rendus (pixels non-noirs)
tnb=sum(1 for o in range(0,tW*tH*3,3) if (t80[o],t80[o+1],t80[o+2])!=(0,0,0))
res.append(("text 80col: %dx%d, %d non-black px"%(tW,tH,tnb), tW==480 and tnb>1000))
for msg,okk in res:
    print(("OK " if okk else "KO ")+msg)
sys.exit(0 if all(o for _,o in res) else 1)
PY
if [ $? -eq 0 ]; then
    ok "palette / copper / start-address rendered correctly"
else
    ko "one or more ULA-NG visible checks failed"
fi

echo "=== result: $pass passed, $fail failed ==="
[ "$fail" -eq 0 ]
