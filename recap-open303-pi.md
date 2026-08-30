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

## Validé à l'oreille sur le Pi (Digitakt II, RASP303)

Service systemd installé et stable, Digitakt détecté automatiquement au branchement (règle udev).
Deux bugs trouvés et corrigés pendant cette validation :

16. **`pickMidiPort()` retournait toujours `0`.** Malgré le commentaire "on prend le premier
    port USB trouvé", c'était un `return 0;` en dur. `Midi Through` (port virtuel ALSA, toujours
    présent en position 0) était donc systématiquement choisi au lieu du Digitakt — silencieux,
    aucune erreur, juste aucune note qui n'arrivait jamais. Corrigé : premier port dont le nom ne
    contient pas "Midi Through".
17. **`EXTRA_ARGS` vide passé comme argument vide au binaire.** `systemd/open303.service` utilisait
    `${EXTRA_ARGS}` (accolades) dans `ExecStart` : en systemd, cette syntaxe génère toujours un
    argument, même vide, contrairement à `$EXTRA_ARGS` (sans accolades) qui disparaît entièrement
    si la variable est vide. Avec `EXTRA_ARGS=` vide par défaut, le service passait un argument
    `""` au binaire, rejeté par le parsing (`Argument inattendu:`) → boucle de redémarrage infinie.

Confirmé au clavier : cutoff, résonance, env mod, accent (vélocité ≥ 100), decay (uniquement sur
notes non accentuées — normal, `accentDecay` est fixe à 200 ms), volume, waveform, slide (CC5 +
legato). `--rt` craque sur la sortie jack/HDMI embarquée du Pi, comme prévu par le README — pas
testé avec une carte son USB.

## CC supplémentaires ajoutés (paramètres "back-panel" d'Open303)

18. **8 nouveaux CC RT-safe**, plage "undefined" du spec MIDI 1.0 (CC20-27) : attaque filtre
    normale/accentuée (CC20/21), highpass avant/feedback/après filtre (CC22/23/24), sustain/decay/
    release de l'enveloppe d'ampli (CC25/26/27). Détail des plages en commentaire dans
    `applyMidiEvent()` (`src/main.cpp`) et dans le tableau du README section 7.
19. **3 paramètres volontairement exclus du mapping CC** : `setSquarePhaseShift`,
    `setTanhShaperDrive`, `setTanhShaperOffset` appellent tous `fillWithSquare303()` →
    `generateMipMap()` (régénération FFT de la wavetable) à chaque appel. Les brancher sur un CC
    aurait réintroduit, depuis le thread audio, le même type de problème que l'ancienne inversion
    de priorité (point 2 plus haut) — craquements garantis en tournant le knob. Reglables
    uniquement au démarrage via `--square-phase`/`--tanh-drive`/`--tanh-offset` (appliqués avant
    l'ouverture du flux audio, donc hors thread RT).

20. **CC28/29 ajoutés (Tuning, Accent).** En comparant au panneau d'un TD-3-MO (clone 303 modded),
    deux knobs classiques du 303 restaient non exposés malgré des setters triviaux et RT-safe :
    `setTuning` (CC28, 415-466 Hz autour de 440 Hz) et `setAccent` — l'**intensité** de l'accent,
    distincte de son déclenchement par vélocité déjà géré par le moteur (CC29, 0-100%).
    Les autres extras du TD-3-MO (Soft Attack, Filter Tracking, Overdrive, Filter FM, Sweep,
    Muffler, Sub Osc) sont des ajouts "mod" propres au hardware Behringer, sans équivalent dans
    `rosic::Open303` (qui modélise le 303 d'origine) : pas mappables sans écrire du nouveau code
    DSP dans le moteur, hors de portée d'un simple ajout de CC.

Pas encore testés à l'oreille (CC20-29 ni les 3 flags CLI) : à valider sur le Pi à l'occasion.

## Sortie audio USB + `--rt` validés (adaptateur UGREEN)

Deux bugs trouvés en essayant de faire sortir le son sur un adaptateur USB générique ("KT USB
Audio", branché sur le Pi) plutôt que sur la sortie embarquée :

21. **`dac.getDefaultOutputDevice()` ignorait le device USB branché.** ALSA ne priorise pas le
    matériel externe : le device "par défaut" restait `bcm2835 Headphones` même adaptateur USB
    branché. Corrigé par `pickAudioOutputDevice()` (même logique que `pickMidiPort()`) : on
    énumère les devices et on prend le premier qui n'est pas une sortie embarquée connue
    (`bcm2835`, `vc4hdmi`).
22. **`"Default ALSA Device"` déjouait la correction ci-dessus.** RtAudio (backend ALSA, constaté
    sur RtAudio 6.0.1) ajoute une entrée synthétique de ce nom, listée en premier, qui redirige
    elle-même vers le device ALSA "default" (donc `bcm2835`) — sans "bcm2835"/"vc4hdmi" dans son
    nom, elle passait le filtre et était choisie à tort avant la vraie carte USB. Exclue
    explicitement.

Au passage, `setvbuf(stdout, ..., _IOLBF, 0)` ajouté en début de `main()` : sous systemd, stdout
n'est pas un TTY donc glibc bufferise par blocs de 4 Ko au lieu de ligne par ligne — tous les
printf de diagnostic restaient coincés en mémoire jusqu'à l'arrêt du process (flush différé,
`journalctl` montrait alors des messages avec l'horodatage du *redémarrage suivant*, pas du
moment réel où ils avaient été émis — source de confusion en debug, un process tournant
normalement dans sa boucle d'attente semblait "bloqué" faute de sortie visible).

**Validé au clavier virtuel** (notes injectées via `aseqsend` + un script Python sur le port ALSA
`Midi Through` — une première tentative en bash/sed pour convertir l'hex en binaire échouait
silencieusement sur ce Pi, `sed` y perdant les backslashes) : sortie `KT USB Audio` confirmée dans
les logs, `--rt` actif sans craquement sur cet adaptateur, notes séparées, legato/slide (glissando
continu sur chevauchement), accent, note tenue longue — tout confirmé bon à l'oreille.

## Tests de latence (RASP303, adaptateur UGREEN, `--rt`)

Balayage de `BUFFER_FRAMES` (256/128/64/32) avec notes jouées à chaque palier, puis un test plus
exigeant à la taille la plus basse (note tenue + balayage continu cutoff/résonance pendant la
lecture, le scénario "craquements" du README) :

| BUFFER_FRAMES demandé | Buffer réel négocié | Latence nominale | Xruns |
|---|---|---|---|
| 256 | 256 | 5,8 ms | Aucun |
| 128 | 128 | 2,9 ms | Aucun |
| 64  | 64  | 1,5 ms | Aucun |
| 32  | 45 (ajusté par ALSA) | 1,0 ms | Aucun, même sous charge (CC en rafale) |

Température stable ~54°C, pas de throttling actif pendant les tests. `/etc/default/open303` de ce
Pi réglé sur `BUFFER_FRAMES=64` (marge par rapport au minimum testé de 45, tout en restant très en
dessous des 256 par défaut recommandées dans le README pour un 3B+). Le template
`systemd/open303.default` du dépôt (640, conservateur) n'a volontairement pas été changé : ce
résultat est spécifique à ce Pi + cet adaptateur + `--rt`, pas une garantie générale.

Note méthodologique : les notes/CC ont été injectées via `aseqsend` (port ALSA `Midi Through`),
donc sans la latence/gigue USB du vrai Digitakt — à garder en tête en comparant au ressenti réel
au clavier.

## Interface web de pilotage (canal MIDI, sortie audio)

Processus Flask séparé (`web/app.py`), zéro changement au chemin audio temps réel : la page
réécrit `/etc/default/open303` et redémarre `open303.service` via un script sudo scopé
(`web/open303-web-apply.sh` + règle sudoers dédiée, utilisateur système `open303-web` séparé de
`open303`). Look terminal rétro (vert/ambre sur noir, ASCII). Pas d'authentification — documenté
dans le README comme réservé à un réseau local de confiance.

Deux nouveaux flags CLI (hors chemin RT, appliqués une fois au démarrage) rendent ça possible :
`--audio-device SOUS-CHAINE` (force la sortie par nom) et `--list-devices` (affiche les
périphériques et quitte, sert à peupler le menu déroulant).

Oscilloscope initialement envisagé, abandonné : capturer le flux audio réel aurait exigé soit de
retoucher le routage ALSA (risque de régression sur la latence/xruns tout juste validés), soit un
tap lock-free dans le thread audio (accroc au principe "zéro changement au binaire" retenu ici).

Bugs trouvés en installant sur le Pi :
23. **`open303-web` sans le groupe `audio`.** RtAudio ne peut ouvrir aucun `/dev/snd/*` sans ce
    groupe : `--list-devices` lancé par ce compte renvoyait "Aucune carte audio détectée", même
    pour des devices non occupés en exclusif. `install-web.sh` l'ajoute maintenant.
24. **Device actif absent de `--list-devices`.** RtAudio ne peut pas sonder un device déjà ouvert
    en exclusif par `open303.service` — la carte USB active disparaissait de l'énumération tant
    que le service tournait. `app.py` retrouve le device réellement actif via la dernière ligne
    "Sortie audio: ..." des logs (`current_output_device()`) et l'ajoute à la liste s'il manque.

Validé de bout en bout sur le Pi : page accessible (`http://192.168.1.64:8303`), changement de
canal MIDI via le formulaire → écriture config → redémarrage → nouveau canal effectif confirmé
dans les logs, remis ensuite sur "tous les canaux".

Suite à quelques itérations de style sur cette page : titre "OPEN303-PI" en police retro VT323
(Google Fonts, chargée côté client — fonctionne même Pi hors ligne), UI entièrement en anglais,
QR code (généré côté serveur via `qrcode`, coloré pour matcher le thème : `#aaffbb` sur `#0b0f0a`)
déplacé en bas de page sans cadre ni légende — juste l'image.

## MIDI sans fil (rtpmidid) + sélecteur de port dans l'interface web

25. **rtpmidid installé** (`.deb` précompilé, pas dans les dépôts Debian — `rtpmidid-debian-trixie-arm64-26.01.deb`,
    correspond exactement à l'OS du Pi). Exporte par défaut `Midi Through` vers le réseau en
    RTP-MIDI (annonce mDNS sous le nom d'hôte, port 5004) : n'importe quel logiciel RTP-MIDI du
    réseau local peut alors jouer le 303 sans rien reconfigurer côté Pi.
26. **Piège découvert en testant** : `aconnect -l` montre une souscription ALSA bidirectionnelle
    entre `Midi Through` (14:0) et les ports internes de rtpmidid (`129:0 Network Export`,
    `129:1 Announcements`), ce qui suggérait que s'abonner directement à `129:0` serait
    équivalent. **Faux, vérifié empiriquement** (test CPU avant/après un `aseqsend` ciblé sur
    `14:0` : aucune activité du synthé abonné à `129:0`, alors qu'un abonnement à `14:0` déclenche
    bien le moteur DSP) — ce sont des ports de service internes à rtpmidid, pas un passthrough
    générique comme le vrai port kernel `Midi Through`.
27. **`pickMidiPort()` corrigé** pour exclure aussi les noms contenant "rtpmidid" de la sélection
    automatique (en plus de "Midi Through" déjà exclu comme candidat "premier port" — la logique
    de repli final reste "Midi Through" si rien d'autre n'est trouvé). Sans ce correctif, la
    sélection automatique choisissait silencieusement un port rtpmidid non fonctionnel dès que le
    service tournait (toujours présent, contrairement à un vrai contrôleur USB).
28. **`--midi-port SOUS-CHAINE` / `--list-midi-ports`** ajoutés (même pattern que
    `--audio-device`/`--list-devices`). Contrairement à l'audio, pas de souci de "port occupé" :
    les ports de séquenceur ALSA acceptent plusieurs abonnés simultanés, `--list-midi-ports`
    montre donc tout meme service tournant, sans logique de fusion supplémentaire côté web.
29. **Sélecteur de port MIDI ajouté à l'interface web**, même schéma que le device audio (menu
    déroulant peuplé via `--list-midi-ports`, filtré pour exclure les ports internes rtpmidid ;
    écrit `--midi-port "NOM"` dans `EXTRA_ARGS`). Validé de bout en bout sur le Pi (sélection
    explicite de `Midi Through` → confirmé dans les logs → remis sur auto).

Pas testé : réception MIDI réelle depuis un vrai pair réseau (iPad/DAW en RTP-MIDI) — aucun
appareil compatible disponible pendant cette session. Seul le mécanisme local (`Midi Through`
comme point d'entrée partagé, cohérent avec la config `alsa_hw_auto_export` par défaut de
rtpmidid) a pu être vérifié.

## Session de debug MIDI Wi-Fi réel + logging + limite structurelle de rtpmidid

Tentative de faire jouer le 303 en conditions réelles via le Digitakt → USB → PC Windows →
rtpMIDI → réseau → Pi. Plusieurs bugs réels trouvés et corrigés, une limite structurelle
identifiée (non corrigée, cf ci-dessous), et le vrai blocage final situé hors du projet.

30. **`--midi-port`/`--list-midi-ports` corrigés une seconde fois.** Le correctif précédent
    (point 27) excluait les noms contenant "rtpmidid" trop largement lors du choix EXPLICITE
    aussi — non, en fait le vrai bug ici était différent : `pickMidiPort()` fonctionnait, mais
    **la sélection automatique retombe sur un port de test rtpmidid périmé** (`rtpmidid:PyMIDI`,
    `rtpmidid:NimH-PC`...) plutôt que sur `Midi Through` quand plusieurs instances du même nom
    trainent (rtpmidid ne nettoie pas toujours les ports ALSA des sessions mortes). Contournement
    ce soir : `sudo systemctl restart rtpmidid.service` pour purger les ports fantômes.
31. **Log des événements MIDI ajouté** (`logMidiEvent()` dans `handleMidiMessage()`, thread MIDI,
    pas RT-sensible) : `[midi] Note On/Off/CC/Pitch Bend ...` sur stdout. A permis de confirmer
    sans ambiguïté quand un événement atteint réellement `handleMidiMessage()`.
32. **Découverte majeure : injecter du MIDI directement dans un port `rtpmidid:<pair>` via
    `aseqsend` ne fonctionne PAS**, même quand `aconnect -l` montre la souscription comme
    active. Seul `Midi Through` (14:0) relaie fiablement les événements injectés localement.
    Plusieurs "confirmations CPU" antérieures dans cette session sur des ports `rtpmidid:*`
    étaient probablement des faux positifs, coïncidant avec de vraies tentatives de
    l'utilisateur au même moment. **Toujours tester via `--midi-port "Midi Through"` +
    `aseqsend -p 14:0` pour un test fiable et reproductible.**
33. **Limite structurelle non corrigée : rtpmidid crée un nouveau port ALSA à chaque connexion**
    (`[rtpmidi_announce]` et `[rtpmidi_discover]`), sans les nettoyer de façon fiable. Notre
    binaire ne s'abonne qu'une fois au démarrage : si la session réseau change (reconnexion,
    nouveau pair), l'abonnement existant devient orphelin silencieusement, et rien ne permet de
    savoir quel port numéroté est "le bon" quand plusieurs portent le même nom. **Fix propre pour
    une prochaine session** : suivre les événements du port ALSA "Announce" (client 0) pour
    détecter les changements de topologie et se ré-abonner dynamiquement, au lieu de choisir une
    fois au démarrage.
34. **Client Python AppleMIDI/RTP-MIDI minimal écrit** (`rtpmidi_test.py`, stdlib seule) pour
    simuler un envoi MIDI Wi-Fi depuis le PC sans dépendre du Digitakt/rtpMIDI Windows. Handshake
    fonctionnel côté serveur (confirmé dans les logs rtpmidid : peer créé, port ALSA créé,
    connecté) même si le client ne reçoit pas toujours l'accusé de réception du port data (à
    ignorer, sans conséquence — le serveur traite la session correctement quand même).
35. **Cause racine réelle du silence "MIDI envoyé, rien n'arrive" côté Digitakt** : confirmé via
    MIDI-OX (moniteur MIDI Windows) que **le Digitakt n'envoie aucun octet MIDI vers Windows en
    USB**, même piste MIDI dédiée (piste 16), séquenceur en lecture, trigs présents, port USB MIDI
    activé dans `MIDI CONFIG`. Hors du périmètre de ce projet — à creuser côté Digitakt/USB/OS
    (câble, port USB, réglage manquant) dans une session dédiée.
36. **Suspicion forte de dongle audio USB défaillant.** Le même test exact (`Midi Through`,
    même note, même config, aucun changement logiciel) a produit un son audible à 00:58 puis plus
    aucun son à 01:13, quinze minutes plus tard. Combiné aux anomalies déjà observées ce soir
    (renumérotation de carte inexpliquée, 44100 Hz silencieux puis fonctionnel puis silencieux
    à nouveau), le diagnostic le plus probable est un **adaptateur USB audio générique
    ("KT USB Audio") intrinsèquement peu fiable**, pas un bug logiciel. À tester avec un autre
    adaptateur USB audio, ou en repli sur la sortie jack embarquée pour confirmer que le problème
    n'est pas propre à l'USB audio sur ce Pi en général.

## CC20-29 validés à l'oreille (session suivante, après reboot du Pi)

Tous testés via `aseqsend` sur `Midi Through` (méthode fiable, cf point 32), chacun en A/B
franc (valeur min vs max) sur notes tenues. **Tous confirmés fonctionnels** :

| CC | Paramètre | Ressenti |
|---|---|---|
| CC20 | Attaque filtre, notes normales | Audible, mais **subtil** — normal, la plage Devil Fish 0,3–30 ms est très courte. Perceptible seulement en A/B rapproché. |
| CC21 | Attaque filtre, notes accentuées | Idem CC20 (même plage), audible en A/B sur notes vél. ≥ 100. |
| CC22 | Highpass pré-filtre | Très net : le grave disparaît franchement au max. |
| CC23 | Highpass feedback | Un seul saut audible entre 0 Hz et le premier palier, puis plus rien — **quelle que soit la plage**. Explication trouvée depuis (cf § ci-dessous) : limite structurelle du DSP, pas un réglage à corriger. |
| CC24 | Highpass post-filtre | Très net, comme CC22. |
| CC25 | Sustain ampli | Très net : -60 dB = extinction percussive 303, 0 dB = note tenue. |
| CC26 | Decay ampli | Très net : du clic (16 ms) à la note très longue (3000 ms). |
| CC27 | Release ampli | Très net : coupe sèche vs queue qui traîne après le note-off. |
| CC28 | Tuning | Net : progression de hauteur 415 → 440 → 466 Hz sur la même note. |
| CC29 | Intensité accent | Net sur notes accentuées : plus agressif/ouvert à 100 %. |

**Piège rencontré pendant les tests** : mettre « tout à 0 » pour réinitialiser rend le synthé
quasi muet — CC25 à 0 = sustain -60 dB, CC26/27 à 0 = enveloppes très courtes. Pour revenir aux
défauts moteur, utiliser plutôt : CC20/21≈10, CC22≈11, CC23≈38, CC24≈6, CC25=0, CC26≈52, CC27=0.

Confirme aussi (point 36) que le silence audio est bien **intermittent et lié au matériel** : après
un redémarrage complet du Pi, le son est revenu normalement avec la config inchangée.

## CC23 : fausse piste élucidée (ne pas la refaire)

Constatant que seul le premier palier s'entendait, deux resserrages successifs ont été tentés
(0-500 → 0-150 → 0-60 Hz) : aucun n'a rendu le knob progressif, et le second a divisé l'effet
maximal par 8. En allant lire le DSP plutôt qu'en continuant à tâtonner : `feedbackHighpass` vit
dans `TeeBeeFilter`, donc tourne au taux **sur-échantillonné ×4** (176,4 kHz), et `OnePoleFilter`
calcule `x = exp(-2*pi*fc/sampleRate)` → la fraction réellement filtrée (`1-x`) vaut 0,05 % à
15 Hz, 0,21 % à 60 Hz, 0,53 % à 150 Hz, 1,77 % à 500 Hz. L'effet est donc **intrinsèquement ténu
et non progressif**, ce n'est pas un bug ni un mauvais mapping. Plage 0-500 Hz restaurée (maximum
d'effet disponible), calculs consignés dans le commentaire de `applyMidiEvent()` et dans le README
pour clore le sujet.

## Flags CLI `--square-phase` / `--tanh-drive` / `--tanh-offset` validés

Testés en relançant le binaire à la main (service arrêté) avec des valeurs contrastées, mêmes
notes à chaque manche. **Les trois sont bien câblés et fonctionnels**, mais tous produisent des
effets discrets — cohérent avec leur nature : ce sont des paramètres de calibration interne, le
code amont qualifie lui-même les deux `tanh` de *« internal parameter, to be scrapped eventually »*.

- **`--square-phase`** (défaut 180°) : aucun effet en carré pur — normal, le header amont précise
  *« this is important when the two are mixed »*. Testé correctement à **CC70=64** (mélange
  saw+carré 50/50), la différence s'entend : surtout un changement de **niveau perçu**, par
  interférence constructive/destructive entre les deux ondes selon leur phase relative.
- **`--tanh-drive`** (défaut 36,9 dB) : différence de volume entre 12 / 36,9 / 60 dB, mais pas de
  timbre. Explication calculée : `tanh(facteur·x + offset)` **sature totalement** dès ~25 dB
  (facteur 70 à 36,9 dB → sortie ±1,0 partout). Le défaut est déjà dans la zone saturée, donc
  monter plus haut ne change plus rien à la forme d'onde. À 12 dB la saturation est incomplète et
  l'onde n'est plus vraiment un carré, d'où l'écart de niveau.
- **`--tanh-offset`** (défaut 4,37) : décale le point de bascule → change le **rapport cyclique**.
  Calculé : offset 0 → 50 %, 4,37 → 46,9 %, 8 → 44,3 %, 50 → 14,3 %. Sur la plage utile (0-8) la
  variation est trop faible pour s'entendre ; à la valeur extrême 50 (impulsion fine) la
  différence devient perceptible mais reste peu marquée — le filtre passe-bas et le
  band-limiting du mip-mapping adoucissent la forme d'onde.

## ✅ MIDI Wi-Fi fonctionnel de bout en bout (point 33 corrigé)

37. **Ré-souscription MIDI dynamique implémentée.** La boucle principale (déjà réveillée toutes
    les 200 ms pour le rapport d'événements perdus) resonde désormais la liste des ports ALSA
    toutes les ~2 s, en mode silencieux, et se rebranche dès que le port retenu change. Corrige
    la limite structurelle du point 33 : rtpmidid crée un nouveau port à chaque connexion réseau
    et l'abonnement pris au démarrage devenait orphelin en silence (plus aucune note, aucune
    erreur, redémarrage manuel obligatoire). Gère aussi, au passage, le **hot-plug d'un
    contrôleur USB**, qui ne reposait jusqu'ici que sur la règle udev redémarrant tout le service.
38. **CC 123 (All Notes Off) ajouté**, poussé dans la file pendant la bascule — à cet instant le
    thread de callback RtMidi est arrêté par `closePort()`, donc un seul producteur : hypothèse
    SPSC préservée. Sans ça, une note tenue au moment où la session tombe ne reçoit jamais son
    note-off et le moteur tourne indéfiniment (c'était l'origine du CPU anormalement élevé au
    repos observé pendant le débogage).

**Validé de bout en bout** avec `tools/rtpmidi_melody.py` (client AppleMIDI Python) : séquence de
8 notes avec accents envoyée depuis le PC en Wi-Fi → reçue et **entendue** sur le Pi. Les logs
montrent la bascule automatique dans les deux sens (`Midi Through` ↔ `rtpmidid:PyMIDI`) au fil des
sessions réseau qui apparaissent et meurent. C'est la première fois que le chemin Wi-Fi complet
fonctionne : PC → RTP-MIDI → rtpmidid → ALSA → open303 → audio USB.

## Reste à faire

- Valider avec un **vrai pair réseau musical** (iPad, DAW...) plutôt que le client Python de test.
- **Résoudre la fiabilité de l'adaptateur audio USB** (remplacer ou tester un autre modèle) —
  suspecté défaillant par intermittence (point 36 ci-dessus).
- **Diagnostiquer pourquoi le Digitakt n'envoie rien en USB MIDI vers Windows** (point 35),
  indépendamment de ce projet.

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
