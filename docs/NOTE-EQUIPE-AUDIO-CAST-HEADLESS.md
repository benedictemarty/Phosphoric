# Un mot à l'équipe Phosphoric — le flux audio du cast (`/audio`) est muet en headless (`-n`)

**Date** : 2026-09-02
**De** : bmarty <bmarty@mailo.com>
**Objet** : oriced embarque Phosphoric en **headless** et reçoit bien la vidéo (`/stream`) mais
**aucun son** via `/audio`. Cause racine identifiée dans le code : le push audio vers le cast est
piloté par le **callback SDL**, inactif en headless. Demande de correctif précise ci-dessous.

---

## En deux mots

oriced (l'éditeur/IDE BASIC) lance l'émulateur en `-n --realtime --cast-server=… --http-api=…` et
affiche la sortie **live** dans la page (MJPEG). On voudrait aussi le **son**, exposé par le serveur
cast sur `/audio` (WAV mono 44,1 kHz 16-bit). En headless, ce flux ne porte **aucun échantillon** —
y compris avec un programme qui joue (`10 SOUND 1,1000,15 : 20 GOTO 20`).

## Cause racine (vérifiée dans le code)

- Le **seul appelant** de `cast_server_push_audio()` est **`audio_callback()`**
  (`src/audio/audio_output.c`, ~l.116).
- Or `audio_callback` est le **callback du périphérique audio SDL** :
  `want.callback = audio_callback;` puis `SDL_OpenAudioDevice(...)` (`src/audio/audio_output.c`, ~l.168-170).
- En **headless**, le device SDL audio n'est pas ouvert → `audio_callback` **n'est jamais invoqué**
  → le ring buffer audio du cast reste vide → `/audio` ne diffuse rien.
- La génération PSG **par frame** existe pourtant déjà en headless (`src/main.c`, ~l.1298-1305), via
  `ay_generate` (le commentaire dit *« the same routine the SDL callback uses »*), **mais seulement**
  quand on enregistre en WAV/AVI (`emu->audio_wav_fp || avi_audio`), et elle **ne pousse pas** au cast.

## Preuve

En headless avec `--cast-server`, une connexion à `/audio` pendant un `SOUND` actif ne fournit aucun
PCM. À l'inverse, en mode GUI (SDL ouvert) le son du cast fonctionne — cohérent avec le fait que le
push dépend du callback SDL.

## Demande concrète

Dans le bloc de génération audio headless par frame (`src/main.c`, ~l.1304) :

1. **déclencher aussi** ce bloc quand `emu->has_cast_server` (aujourd'hui : seulement `audio_wav_fp ||
   avi_audio`) ;
2. **pousser** les échantillons de la frame au cast :
   `cast_server_push_audio(&emu->cast_server, frame_pcm, WAV_FRAME_SAMPLES)`.

C'est l'exact pendant headless de ce que fait déjà le callback SDL en GUI (même routine `ay_generate`,
même push). Réversible, sans impact sur le mode GUI ni sur l'enregistrement WAV/AVI existant.

Idéalement : générer **une seule fois** par frame et alimenter *à la fois* le tap WAV/AVI **et** le
cast (le commentaire existant insiste déjà : `ay_generate` consomme les événements PSG, ne pas
générer deux fois).

## Ce que fera oriced ensuite

Une fois le flux `/audio` alimenté en headless : proxy `/emu/audio` (même origine, comme
`/emu/stream`) + balise `<audio>` dans la page, démarrée sur un geste utilisateur (politique
autoplay des navigateurs). Rien de plus n'est requis côté émulateur.

## Question ouverte

Y a-t-il une raison délibérée de **ne pas** générer/pousser l'audio en headless hors enregistrement
(coût CPU par frame, réservé aux artefacts) ? Si oui, un **flag opt-in** (ex. `--cast-audio`)
conviendrait parfaitement à oriced.
