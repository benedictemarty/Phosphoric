# 0005 — Deux branches miroir : français et anglais, qui ne diffèrent que par les commentaires

- **Statut** : acceptée (2.1.4)

## Contexte

Le projet est écrit en français (commentaires, documentation, messages de
commit), mais publié aussi pour un public anglophone.

## Décision

- `main` (français) est poussée sur Framagit et `origin`.
- `main-en` (anglais) = `main` + des commits de traduction ; elle est poussée en
  `main` sur GitHub et Codeberg. `main` n'est jamais poussée sur ces deux miroirs.
- Seuls les commentaires et la documentation diffèrent : le code, les chaînes
  (textes affichés compris) et les fichiers de données sont identiques.
  `tools/check_comment_only_diff.py` le vérifie (commentaires retirés, chaque
  fichier doit être identique à celui de `main`) ; `make test-comment-diff`
  auto-teste ce vérificateur.

## Conséquences

- Chaque livraison se fait en deux temps : `main`, puis fusion dans `main-en` et
  traduction des nouveaux commentaires.
- La CI GitHub tourne sur la branche anglaise, au code identique.
