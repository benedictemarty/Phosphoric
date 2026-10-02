# 0005 — Two mirror branches, French and English, differing only in comments

- **Status**: accepted (2.1.4)

## Context

The project is written in French (comments, documentation, commit messages), but
also published for an English-speaking audience.

## Decision

- `main` (French) is pushed to Framagit and `origin`.
- `main-en` (English) = `main` + translation commits; it is pushed as `main` to
  GitHub and Codeberg. `main` is never pushed to those two mirrors.
- Only comments and documentation differ: code, strings (displayed texts
  included) and data files are identical. `tools/check_comment_only_diff.py`
  checks it (comments stripped, each file must equal the one in `main`);
  `make test-comment-diff` self-tests that checker.

## Consequences

- Every delivery happens in two steps: `main`, then a merge into `main-en` and
  the translation of the new comments.
- GitHub CI runs on the English branch, whose code is identical.
