# open303-pi — reprise de contexte

## Le projet

Hôte standalone minimal : MIDI USB (RtMidi) → moteur DSP `rosic::Open303` → audio (RtAudio/ALSA),
sans DAW ni hôte de plugin. Tout le code propre au projet tient dans `src/main.cpp` ;
`third_party/Open303` est un clone de `RobinSchmidt/Open303`, **patché directement** sur deux
points (dépôt amont figé depuis ~2 ans, cf § ci-dessous) — plus de mécanisme `sed` dans les
scripts de build.

- Cible : Raspberry Pi 3B+, **Raspberry Pi OS 64-bit**
- Build : `./docker/build-cross.sh` (conteneur Debian trixie arm64 via QEMU) → `build-cross/open303_pi_host`
- Dev : WSL Ubuntu, VS Code Remote-WSL, build natif x86 dans `build-wsl/` pour IntelliSense
- Contrôleur MIDI : Elektron Digitakt (piste MIDI dédiée)

La chaîne DSP réelle, dans `Open303::getSample()` :

```
BlendOscillator (saw↔square, wavetables mip-mappées)
  └─ ×4 oversampling ─┐
     highpass1 (pré-filtre)
     TeeBeeFilter (ladder 303 + highpass dans la boucle de feedback)
     EllipticQuarterBandFilter (anti-alias, décimation)
  ────────────────────┘
allpass → highpass2 → notch → × ampEnv (dé-clické) → × ampScaler
```

Cutoff instantané : `mainEnv` (DecayEnvelope) → `rc1`/`rc2` (LeakyIntegrator, attaque
normale/accent) → `instCutoff = cutoff · 2^(envScaler·(rc1−envOffset) + accentGain·rc2)`.

## Déjà corrigé (compile sans warning, non testé à l'oreille)

**`src/main.cpp`**

1. **Unités des setters.** `setEnvMod`, `setResonance` et `setAccent` attendent des
   **pourcentages (0..100)**, pas 0..1 — en interne : `linToLin(envMod, 0, 100, 0, 1)`.
   Le code envoyait 0..1, ce qui rendait l'env mod et l'accent quasi inaudibles.
   CC1 fait maintenant `setEnvMod(100.0 * v01)` ; l'init utilise 50.0 au lieu de 0.5.
2. **Plus de mutex dans le thread temps réel.** Le callback audio prenait un `std::mutex`
   que le thread MIDI (priorité normale) pouvait détenir en étant préempté → inversion de
   priorité, cause probable des craquements attribués à `--rt` dans le README. Remplacé par
   une file d'événements MIDI lock-free SPSC (ring buffer de 256, acquire/release), vidée en
   début de bloc par `drainMidiQueue()`. Les débordements sont comptés et signalés depuis la
   boucle principale.
3. `kMidiChannel` est désormais réellement câblé (il était déclaré mais inutilisé).

**`docker/build-cross.sh`**

4. Défaut passé à `linux/arm64` (`linux/arm/v7` en argument explicite).
5. Restitution de la propriété des fichiers à l'UID hôte en sortie (`trap ... EXIT`), le
   conteneur tournant en root.
6. `ensure_qemu()` / `ensure_builder()` : sonde l'émulation et réenregistre QEMU
   (`tonistiigi/binfmt`) + crée le builder buildx `open303-multiarch` si besoin. Confirmé
   présent et correctement câblé (build-cross.sh:27-54).

**`third_party/Open303` (patché directement dans les fichiers, plus de `sed` en script)**

7. `rosic_MipMappedWaveTable.h` : `prototypeTable` était déclaré `[tableLength]` alors que
   `initPrototypeTable()` écrit `tableLength+4` éléments → dépassement de 32 octets, signalé
   par GCC en `-Waggressive-loop-optimizations`. Risque réel de mauvaise restructuration de
   boucle à `-O3`. Corrigé en `[tableLength+4]`.
8. `rosic_Open303.h`, `getSample()` : le flag `idle` était réarmé `false` sans condition en
   fin de fonction (la vraie logique d'extinction était laissée en commentaire par l'auteur).
   Conséquence : la chaîne DSP complète — oscillateur et TeeBeeFilter à 176,4 kHz avec
   l'oversampling ×4 — tournait en permanence, même sans note ; principal gisement de CPU sur
   le 3B+. Activé, simplifié (`idle = ampEnv.endIsReached() && fabs(tmp) < 0.000001;`) : la
   condition originale sur l'état de l'`AcidSequencer` a été retirée, ce moteur n'étant jamais
   utilisé ici (séquenceur MIDI externe via le Digitakt, cf point 6 restant).

**`src/main.cpp`**

9. `--sine` avait un sample rate en dur (`constexpr kSampleRate = 44100.0`) au lieu du taux
   réel passé en argument. Utilise maintenant `gSampleRate`, une globale fixée dans `main()`
   avant l'ouverture du flux (pas de synchronisation nécessaire : écrite avant que le thread
   audio existe).
10. Parsing d'arguments durci : les positionnels (`sampleRate`, `bufferFrames`) et les flags
    (`--sine`, `--rt`) sont distingués explicitement dans une seule boucle, avec validation
    (`sampleRate <= 0`, `bufferFrames <= 0` → message d'erreur et sortie) — avant, un flag
    passé en premier argument (ex: `--sine` seul) finissait dans `std::atof`, qui renvoie
    silencieusement `0.0`.
11. Gestion d'erreur RtAudio 6 : `pkg-config --modversion rtaudio` → 6.0.1 confirmé. En v6,
    `openStream`/`startStream` ne lèvent plus d'exception mais retournent un
    `RtAudioErrorType` ; le `try/catch` était du code mort et un échec d'ouverture passait
    inaperçu (« Open303 pret » affiché, puis silence). Remplacé par une vérification explicite
    du code de retour + `getErrorText()`. `getDeviceCount()` / `getDefaultOutputDevice()`
    étaient déjà utilisés correctement (pas d'hypothèse d'IDs séquentiels, et
    `getDefaultOutputDevice() == 0` n'est ambigu que dans le cas "aucun périphérique", déjà
    exclu par le `getDeviceCount() == 0` juste avant) — rien à changer là.

## Déjà corrigé (suite, compile sans warning WSL + cross, non testé à l'oreille)

**`src/main.cpp`**

12. **CC74 (cutoff) resserré sur le domaine mesuré.** `calculateEnvModScalerAndOffset()`
    interpole `envScaler`/`envOffset` entre 313,8 et 2394,4 Hz (constantes issues de mesures
    sur un vrai 303) sans clamp (`expToLin` extrapole linéairement hors de cette plage). Le
    mapping CC74 était 50–5000 Hz ; resserré exactement sur 313,8152786059267–
    2394,411986817546 Hz pour que le suivi de l'env mod reste dans le domaine validé sur tout
    le knob.
13. **Pitch bend câblé.** `0xE0` (14 bits, LSB=data1/MSB=data2, centre 8192) → `setPitchBend()`,
    ±2 demi-tons (`kPitchBendRangeSemitones`).
14. **Slide time mappé.** CC5 (Portamento Time, convention MIDI standard) → `setSlideTime()`,
    5–500 ms (défaut moteur : 60 ms).
15. **`kMidiChannel` devenu `gMidiChannel`, réglable en CLI.** `--channel N` (0-15), déjà
    disponible sans toucher au service systemd via `EXTRA_ARGS` dans `/etc/default/open303`.

`AcidSequencer` reste délibérément inutilisé (séquenceur MIDI externe via le Digitakt, cf
mémoire projet).

## Reste à faire

Rien d'identifié pour l'instant au-delà de la validation à l'oreille sur le Pi (section
suivante).

## À valider à l'oreille sur le Pi

- **Env mod** : note tenue + balayage de la mod wheel (CC1) — l'enveloppe doit maintenant
  ouvrir franchement le filtre. Idem accent : même note à vélocité < 100 puis ≥ 100.
- **Craquements** : tourner un potard de CC pendant une note tenue, puis retenter `--rt`,
  qui était contre-indiqué sur la sortie jack embarquée. Surveiller `[midi] ... perdu(s)`.
- Accent et slide ne demandent aucun mapping : vélocité ≥ 100 déclenche l'accent, le legato
  déclenche le slide (60 ms par défaut, réglable via CC5), directement dans `Open303::noteOn()`.
- **CC74 resserré** : vérifier que tout le knob (0-127) reste utile et musical maintenant que
  la plage est 313,8–2394,4 Hz au lieu de 50–5000 Hz — potentiellement moins de grave/aigu
  extrême, à comparer au ressenti d'un vrai 303.
- **Pitch bend** : roue de pitch en butée haute/basse sur une note tenue → doit glisser de
  ±2 demi-tons, retour net au centre.
- **Slide (CC5)** : note tenue + legato, balayer CC5 → le temps de glissement doit varier
  perceptiblement entre les extrêmes (5 ms quasi instantané, 500 ms très lent).
- **`--channel N`** : lancer avec `--channel` sur le canal de la piste Digitakt dédiée au 303,
  vérifier que les autres pistes/canaux sont bien ignorés.
