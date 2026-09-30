#!/usr/bin/env python3
"""check_comment_only_diff.py -- vérifie qu'une traduction n'a touché QUE des commentaires.

Sert à la branche miroir anglaise `main-en` : elle ne diffère de `main` que par
la documentation et les commentaires du code. Le script compare chaque fichier
de l'arbre de travail avec sa version dans REF, une fois commentaires retirés
(chaînes et code conservés, lignes vides ignorées). Toute autre différence est
signalée : code, identifiant ou message modifié.

Usage (depuis la racine du dépôt) :
    tools/check_comment_only_diff.py [--base REF] [fichier...]
Sans fichier : tous les fichiers de code modifiés entre REF (défaut : main) et
l'arbre de travail. Code de sortie 1 si au moins un fichier est en échec.
"""
import io, re, subprocess, sys, tokenize

CODE_RE = re.compile(r"\.(c|h|js|css|sh|py|yml|yaml|mk|html|s|asm|bas|txt)$|(^|/)Makefile$")


def git(*a):
    return subprocess.run(["git", *a], capture_output=True, text=True).stdout


def strip_c(s, css=False):
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if s.startswith("/*", i):
            j = s.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * s.count("\n", i, j) or " ")
            i = j
        elif not css and s.startswith("//", i):
            j = s.find("\n", i)
            i = n if j < 0 else j
        elif c in "\"'`" and not (css and c == "`"):
            j = i + 1
            while j < n and s[j] != c:
                if s[j] == "\\":
                    j += 1
                elif s[j] == "\n" and c != "`":
                    break
                j += 1
            out.append(s[i:j + 1])
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_hash(s):
    """sh/Makefile/yml : retire '#...' hors guillemets (approximation prudente)."""
    res = []
    for line in s.split("\n"):
        q, k = None, 0
        cut = len(line)
        while k < len(line):
            ch = line[k]
            if q:
                if ch == "\\" and q == '"':
                    k += 1
                elif ch == q:
                    q = None
            elif ch in "\"'":
                q = ch
            elif ch == "\\":
                k += 1
            elif ch == "#" and (k == 0 or line[k - 1] in " \t;"):
                if not line.startswith("#!"):
                    cut = k
                    break
            k += 1
        res.append(line[:cut])
    return "\n".join(res)


def strip_py(s):
    toks = []
    try:
        for t in tokenize.generate_tokens(io.StringIO(s).readline):
            if t.type in (tokenize.COMMENT, tokenize.NL, tokenize.NEWLINE,
                          tokenize.INDENT, tokenize.DEDENT):
                continue
            toks.append(t.string)
    except (tokenize.TokenError, IndentationError, SyntaxError) as e:
        return "TOKENIZE-ERROR " + str(e)
    return "\n".join(toks)


def strip_html(s):
    s = re.sub(r"<!--.*?-->", "", s, flags=re.S)
    return s


def normalize(path, s):
    if path.endswith((".c", ".h", ".js")):
        s = strip_c(s)
    elif path.endswith(".css"):
        s = strip_c(s, css=True)
    elif path.endswith(".py"):
        return strip_py(s)
    elif path.endswith(".html"):
        s = strip_html(s)
    elif path.endswith((".sh", ".yml", ".yaml", ".mk")) or path.endswith("Makefile"):
        s = strip_hash(s)
    else:
        return s  # autres : aucune différence tolérée
    return "\n".join(l.rstrip() for l in s.split("\n") if l.strip())


def main():
    args = sys.argv[1:]
    base = "main"
    if len(args) >= 2 and args[0] == "--base":
        base, args = args[1], args[2:]
    files = args or [f for f in git("diff", "--name-only", base).split() if CODE_RE.search(f)]
    bad = 0
    for f in files:
        old = subprocess.run(["git", "show", base + ":" + f],
                             capture_output=True, text=True).stdout
        try:
            new = open(f, encoding="utf-8").read()
        except FileNotFoundError:
            print("FAIL (absent)", f); bad += 1; continue
        a, b = normalize(f, old), normalize(f, new)
        if a != b:
            bad += 1
            al, bl = a.split("\n"), b.split("\n")
            k = next((i for i in range(min(len(al), len(bl))) if al[i] != bl[i]), min(len(al), len(bl)))
            print(f"FAIL {f}\n   main   : {al[k] if k < len(al) else '<fin>'}\n   main-en: {bl[k] if k < len(bl) else '<fin>'}")
    print(f"--- {len(files)} fichiers, {bad} en échec")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
