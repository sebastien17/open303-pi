# Open303 sur Raspberry Pi, pilote par MIDI USB

Hote "standalone" minimal : un clavier/controleur MIDI branche en USB pilote
directement le moteur DSP **Open303** (clone du Roland TB-303), sans passer
par un DAW ni un hote de plugin. La sortie audio se fait via la sortie
jack/HDMI du Pi ou une carte son USB.

## 1. Materiel / OS recommandes

- Raspberry Pi 4 ou 5 (le Pi 3 fonctionne mais avec des buffers plus larges)
- Raspberry Pi OS **Lite 64-bit** (pas besoin de bureau graphique), ou toute
  Debian recente (Bookworm ou Trixie) — **verifiez la version exacte avec
  `cat /etc/os-release` sur le Pi**, elle doit correspondre a l'image Docker
  utilisee en cross-compilation (section 4bis), sinon les versions de libs
  runtime (`librtaudio`, `librtmidi`...) ne matcheront pas.
- Une interface audio USB si possible (latence et qualite bien meilleures
  que la sortie jack integree du Pi, qui a un DAC assez faible)
- Un clavier/controleur MIDI USB **class-compliant** (la plupart le sont)

## 2. Dependances

```bash
sudo apt update
sudo apt install -y build-essential cmake git pkg-config \
    librtaudio-dev librtmidi-dev libasound2-dev
```

## 3. Recuperer le moteur DSP Open303

```bash
cd ~
git clone https://github.com/RobinSchmidt/Open303 open303-pi/third_party/Open303
```

C'est la lib `rosic` (licence MIT) contenue dans ce depot qui fait tout le
travail de synthese ; ce projet ne fait que la piloter en MIDI + audio.

## 4. Compiler

```bash
cd ~/open303-pi
mkdir build && cd build
cmake .. -DOPEN303_DIR=~/open303-pi/third_party/Open303
make -j$(nproc)
```

> Le code DSP se trouve dans `Source/DSPCode/` du depot clone (fichiers
> `rosic_Open303.h`, `rosic_TeeBeeFilter.h`, `rosic_RealFunctions.h`...).
> Il n'y a pas de header umbrella `rosic.h` dans ce depot (contrairement a
> la lib rosic complete de RS-MET) : `main.cpp` inclut directement
> `rosic_Open303.h`.

## 4bis. Cross-compiler depuis un PC (plus rapide que sur le Pi 3B+)

Compiler directement sur un Pi 3B+ fonctionne mais reste lent. Vous pouvez
cross-compiler depuis un PC x86 plus puissant via Docker + emulation QEMU :
le conteneur "se fait passer" pour un ARM, donc `apt install` et `cmake`
fonctionnent exactement comme dans le reste de ce guide, sans sysroot a
bricoler a la main.

**Une seule fois sur le PC de dev :**

Sous Linux natif :
```bash
sudo apt install docker.io qemu-user-static binfmt-support
docker run --rm --privileged tonistiigi/binfmt --install all
```

Sous Windows (Docker Desktop + WSL2) :
- Installez [Docker Desktop](https://www.docker.com/products/docker-desktop/),
  puis dans `Settings → Resources → WSL Integration`, activez l'integration
  pour votre distro Ubuntu, `Apply & Restart`.
- Docker Desktop embarque deja QEMU, mais son builder par defaut (driver
  `docker`) ne supporte pas le multi-plateforme. Creez un builder dedie :
  ```bash
  docker buildx create --name multiarch --driver docker-container --bootstrap --use
  docker run --rm --privileged tonistiigi/binfmt --install all
  ```
- Verifiez que l'emulation fonctionne :
  ```bash
  docker run --rm --platform linux/arm64 arm64v8/debian uname -m   # doit renvoyer aarch64
  ```
- Ces enregistrements QEMU/builder ne survivent pas toujours a un
  redemarrage de Docker Desktop ou a `wsl --shutdown` : si vous recroisez une
  erreur `exec format error`, refaites simplement la commande
  `tonistiigi/binfmt --install all`.

**A chaque build :**
```bash
# Pi OS 64-bit (arm64) — cible par defaut :
./docker/build-cross.sh

# Pi OS 32-bit (armhf) :
./docker/build-cross.sh linux/arm/v7
```

Le binaire sort dans `build-cross/open303_pi_host`. Transferez-le sur le Pi
(`scp build-cross/open303_pi_host pi@raspberrypi.local:~/`) puis lancez-le
directement — pas besoin de recompiler sur place. L'image Docker utilise
Debian, la meme base que l'OS installe sur le Pi (verifiez avec
`cat /etc/os-release` sur le Pi et ajustez `FROM debian:...` dans
`docker/Dockerfile.cross` en consequence si besoin), donc les
versions de libc/libstdc++/ALSA sont compatibles avec le Pi.

`build-cross.sh` nettoie systematiquement `build-cross/` avant chaque
compilation (le cache CMake garde en memoire l'architecture ciblee au
premier `cmake ..`, ce qui produit des erreurs de flags incomprehensibles
si on change de plateforme — ex: `armv7` puis `arm64` — sans repartir de
zero).

> Limite de cette approche : c'est de l'emulation, donc la compilation
> elle-meme est plus lente qu'un vrai cross-toolchain natif — mais toujours
> largement plus rapide que sur le Pi 3B+ directement, et surtout beaucoup
> plus simple a mettre en place qu'un cross-toolchain + sysroot synchronise
> depuis le Pi (l'autre option, plus rapide mais plus fragile a configurer,
> si vous compilez tres souvent).

## 5. Lancer

```bash
./open303_pi_host 44100 256
```

Arguments optionnels : `<sample_rate> <buffer_frames> [--rt] [--sine]`
(defaut 44100 / 256, cf section 5bis pour le choix du buffer sur Pi 3B+).
`--rt` active le scheduling temps reel (deconseille sur la sortie jack
embarquee, voir section Depannage) ; `--sine` remplace Open303 par un simple
sinus de test, utile pour isoler un souci audio du moteur DSP. Au demarrage,
le programme liste les ports MIDI disponibles et se connecte automatiquement
au premier trouve — branchez votre controleur USB **avant** de lancer le
programme.

## 5bis. Specificites Raspberry Pi 3B+

Le 3B+ (Cortex-A53 quad-core 1.4 GHz, 1 Go de RAM) a moins de marge que les
Pi 4/5. Quelques ajustements par rapport au reste du guide :

- **Buffer** : partez de 256 frames (`./open303_pi_host 44100 256`), et ne
  tentez 128 que si aucun xrun n'apparait. En dessous de 128, le simple cout
  du callback + verrou risque de depasser le temps disponible par bloc sur
  ce CPU.
- **OS** : Raspberry Pi OS Lite **64-bit** est supporte sur le 3B+ et donne
  un peu plus de marge que le 32-bit ; sinon le 32-bit (armhf) fonctionne
  aussi, `CMakeLists.txt` detecte automatiquement lequel des deux et adapte
  les flags de compilation (Cortex-A53 + NEON).
- **Throttling thermique** : le gouverneur `performance` (voir plus bas)
  pousse le CPU a 1.4 GHz en continu, ce qui chauffe. Un dissipateur passif
  suffit generalement pour cette charge legere (un thread audio + un thread
  MIDI), mais verifiez en conditions reelles apres quelques minutes
  d'utilisation :
  ```bash
  vcgencmd measure_temp        # temperature actuelle
  vcgencmd get_throttled       # 0x0 = OK, autre valeur = throttling detecte
  ```
  Si `get_throttled` reste a `0x0`, vous pouvez meme tenter un buffer plus
  petit (128). S'il remonte un flag, ajoutez un peu de circulation d'air
  (boitier ouvert ou petit ventilateur) ou restez sur un buffer plus large.
- **Liberez le CPU** : desactivez le Wi-Fi et le Bluetooth si vous ne vous
  en servez pas (`sudo systemctl disable hciuart bluetooth wpa_supplicant`
  ou plus simplement `dtoverlay=disable-wifi` et `dtoverlay=disable-bt` dans
  `/boot/firmware/config.txt`), et coupez les services inutiles
  (`avahi-daemon`, `triggerhappy`...). Sur un CPU a 4 cœurs partages avec le
  systeme, ca reduit sensiblement la gigue d'ordonnancement.
- **Carte son USB fortement recommandee** : le DAC integre au Pi 3B+ (sortie
  jack) est bruyant et sa latence via ALSA est plus elevee qu'une carte USB
  classe audio (ex: une petite interface Behringer/Focusrite). Ca allege
  aussi le travail du bus USB partage puisque MIDI et audio passent par des
  controleurs differents.

## 6. Reduire la latence (important sur Pi)

- **Buffer audio** : commencez a 256, puis descendez (128, 64) tant qu'il n'y
  a pas de craquements (xruns affiches dans la console).
- **Gouverneur CPU en mode performance** :
  ```bash
  sudo apt install -y linux-cpupower
  sudo cpupower frequency-set -g performance
  cpupower frequency-info   # verifie: la ligne "governor" doit dire "performance"
  ```
  (`cpufrequtils`/`cpufreq-set` sont obsoletes depuis Debian trixie, remplaces
  par `cpupower`/`cpupower frequency-set`. Sur certains Pi, le driver cpufreq
  du SoC peut ne rien exposer a controler via cette interface — dans ce cas
  le gain de ce reglage est de toute facon marginal sur un CPU deja a
  frequence fixe.)
- **Priorite temps reel (optionnelle, opt-in)** : le programme peut demander
  `RTAUDIO_SCHEDULE_REALTIME` via le flag `--rt`, **mais ce n'est plus actif
  par defaut**. Sur la sortie jack embarquee de certains Pi (driver
  `bcm2835`), une priorite RT trop elevee sur le thread audio peut en fait
  degrader le son (craquements periodiques sans xrun ALSA classique, cf
  section Depannage) plutot que l'ameliorer. Avec une **carte son USB**,
  `--rt` est generalement benefique et permet de descendre a un buffer plus
  petit — testez au cas par cas :
  ```bash
  ./open303_pi_host 44100 256 --rt
  ```
  Pour que `--rt` fonctionne sans `sudo`, autorisez votre utilisateur a
  prendre une priorite RT :
  ```bash
  sudo groupadd -f audio
  sudo usermod -aG audio $USER
  echo "@audio   -  rtprio     95" | sudo tee -a /etc/security/limits.conf
  echo "@audio   -  memlock    unlimited" | sudo tee -a /etc/security/limits.conf
  ```
  puis redemarrez la session (deconnexion/reconnexion SSH complete).
- **Noyau PREEMPT_RT** (optionnel, pour aller chercher les toutes petites
  latences avec un buffer de 32-64 frames) : Raspberry Pi OS propose des
  noyaux RT préconstruits, cherchez "raspberry pi kernel PREEMPT_RT" pour la
  procedure a jour selon votre version d'OS.
- Desactivez le Wi-Fi/Bluetooth si vous n'en avez pas besoin (moins de
  contention sur le bus et l'ordonnanceur).

## 7. Mapping MIDI par defaut (a adapter dans `src/main.cpp`)

| Message MIDI       | Parametre Open303        |
|---------------------|---------------------------|
| Note On / Note Off  | Note jouee / relachee     |
| CC74                | Cutoff du filtre (313.8-2394.4 Hz, domaine mesure) |
| CC71                | Resonance                 |
| CC73                | Decay de l'enveloppe      |
| CC1 (mod wheel)     | Env Mod                   |
| CC7                 | Volume                    |
| CC70                | Waveform (saw/square)     |
| CC5 (portamento time) | Temps de slide (5-500 ms, defaut moteur: 60 ms) |
| Pitch bend           | +/- 2 demi-tons           |
| CC20                 | Attaque enveloppe filtre, notes non accentuees (0.3-30 ms) |
| CC21                 | Attaque enveloppe filtre, notes accentuees (0.3-30 ms) |
| CC22                 | Highpass avant le filtre principal (0-500 Hz) |
| CC23                 | Highpass dans la boucle de feedback du filtre (0-150 Hz, defaut moteur en butee haute) |
| CC24                 | Highpass apres le filtre principal (0-500 Hz) |
| CC25                 | Sustain de l'enveloppe d'amplitude (-60-0 dB) |
| CC26                 | Decay de l'enveloppe d'amplitude (16-3000 ms) |
| CC27                 | Release de l'enveloppe d'amplitude, notes non accentuees (1-500 ms) |
| CC28                 | Tuning, reference La4 (415-466 Hz, defaut: 440 Hz) |
| CC29                 | Intensite de l'accent (0-100%, defaut: 50%) — distinct du declenchement (velocite >= 100) |

**Accent et slide n'ont rien a mapper** : le moteur les gere lui-meme dans
`Open303::noteOn()`.
- **Accent** : declenche automatiquement par une **velocite >= 100**. Il bascule
  l'enveloppe de filtre sur `accentDecay` (200 ms, fixe - non reglable en CC)
  et ajoute `accentGain` au gain de l'enveloppe d'amplitude. **CC26 (decay) et
  CC73 (decay filtre) n'ont donc aucun effet sur une note accentuee**, seule
  la vitesse < 100 les rend audibles.
- **Slide** : declenche par le **legato**. Tant qu'une note est encore tenue,
  la suivante ne retrigge pas l'enveloppe mais glisse en pitch
  (`slideToNote`, 60 ms par defaut comme sur le 303). Sur le Digitakt : notes
  qui se recouvrent = slide.

**CC20-27 choisis dans la plage "undefined" du spec MIDI 1.0** (aucune
collision avec un usage standard), cf commentaires dans `applyMidiEvent()`
(`src/main.cpp`) pour le detail de chaque plage.

`setSquarePhaseShift`, `setTanhShaperDrive` et `setTanhShaperOffset` sont
**volontairement absents de tout mapping CC** : ils regenerent la wavetable
mip-mappee entiere (FFT) a chaque appel, ce qui depuis le thread audio
provoquerait des craquements (meme categorie de bug que l'ancienne inversion
de priorite MIDI/audio, cf `recap-open303-pi.md`). Reglables uniquement au
demarrage via `--square-phase`, `--tanh-drive`, `--tanh-offset` (`--help`
pour le detail).

D'autres parametres (tuning, `setAccentDecay`...) peuvent etre ajoutes de la
meme facon dans `applyMidiEvent()` en appelant les setters correspondants de
`rosic::Open303` (voir `Source/DSPCode/rosic_Open303.h` pour la liste
complete) — en verifiant au prealable, comme pour les trois ci-dessus, que le
setter ne fait rien de couteux (allocation, FFT) qui serait dangereux appele
depuis le thread audio.

> **Attention aux unites** : `setEnvMod`, `setResonance` et `setAccent`
> attendent des **pourcentages (0..100)**, pas une valeur normalisee 0..1
> (en interne : `linToLin(envMod, 0, 100, 0, 1)`). Envoyer 0..1 donne un env
> mod quasi nul — c'est-a-dire le parametre le plus caracteristique du 303
> rendu inaudible. `setDecay` est en ms, `setVolume` en dB.

## 7bis. Lancer au demarrage (service systemd)

Tout est fourni dans le dossier `systemd/` : unite de service, redemarrage
automatique tant que le controleur MIDI n'est pas branche, et regle udev
qui redemarre le service des que le Digitakt (ou tout peripherique MIDI
USB) apparait, sans attendre un reboot.

**Sur le Pi**, une fois le binaire compile (section 4 ou 4bis) :

```bash
cd ~/open303-pi
sudo systemd/install-service.sh build/open303_pi_host
# ou, si compile en cross-compilation puis transfere par scp :
sudo systemd/install-service.sh ~/open303_pi_host
```

Ce script :
- cree un utilisateur systeme dedie `open303` (membre du groupe `audio`
  pour l'acces a `/dev/snd/*`, pas de shell de connexion) ;
- installe le binaire dans `/usr/local/bin/` ;
- installe l'unite `open303.service`, l'unite `open303-restart.service` et
  la regle udev `99-open303-midi.rules` ;
- active et demarre le service.

Commandes utiles ensuite :
```bash
sudo systemctl status open303.service     # etat courant
sudo journalctl -u open303.service -f     # logs en direct (utile pour voir les xruns)
sudo systemctl restart open303.service    # apres avoir branche/debranche le Digitakt
```

Pour changer le sample rate ou la taille de buffer sans toucher au code,
editez `/etc/default/open303` puis `sudo systemctl restart open303`.

**Notes** :
- `LimitRTPRIO`/`AmbientCapabilities=CAP_SYS_NICE` dans l'unite permettent
  au service de tourner en priorite temps reel **sans etre root** — c'est
  l'equivalent du reglage `limits.conf` du guide, mais gere par systemd.
- `Restart=on-failure` fait retenter le lancement toutes les 3s si aucun
  port MIDI n'est trouve au demarrage (utile si le Pi boote avant que vous
  ayez branche le Digitakt) ; la regle udev accelere la reprise des que
  le peripherique est effectivement detecte.
- Si vous avez plusieurs peripheriques MIDI USB branches, `main.cpp` prend
  actuellement le premier port trouve — utilisez `--channel N` (0-15, dans
  `EXTRA_ARGS` ci-dessus) pour filtrer sur le canal de la piste MIDI dediee
  au 303 sur le Digitakt.

## 7ter. MIDI sans fil (rtpmidid)

[rtpmidid](https://github.com/davidmoreno/rtpmidid) implemente le protocole
RTP-MIDI (AppleMIDI) : il permet a un DAW, un iPad (GarageBand...) ou une
autre machine sur le meme reseau local d'envoyer du MIDI au Pi sans fil,
sans materiel USB. Pas empaquete dans les depots Debian, mais le projet
publie des `.deb` precompiles par version de Debian :

```bash
curl -fL -o /tmp/rtpmidid.deb \
  https://github.com/davidmoreno/rtpmidid/releases/download/v26.01/rtpmidid-debian-trixie-arm64-26.01.deb
sudo apt install -y /tmp/rtpmidid.deb
```

(adaptez le nom de fichier a votre version de Debian et a la derniere
release — `curl -s https://api.github.com/repos/davidmoreno/rtpmidid/releases/latest`
liste les assets disponibles).

Une fois installe et demarre (`rtpmidid.service`, active par defaut),
rtpmidid annonce ce Pi en mDNS/Bonjour sous son nom d'hote, port 5004, et
exporte automatiquement le port ALSA `Midi Through` vers le reseau
(configuration par defaut dans `/etc/rtpmidid/default.ini`, sections
`[rtpmidi_announce]` et `[alsa_hw_auto_export]`) : n'importe quel logiciel
RTP-MIDI du reseau peut alors s'y connecter et jouer directement le 303,
sans rien reconfigurer cote Pi. C'est le meme port `Midi Through` que le
programme utilise deja par defaut en l'absence de controleur USB.

**Attention** : rtpmidid cree aussi deux ports ALSA a lui (`rtpmidid:Network
Export`, `rtpmidid:Announcements`) qui apparaissent dans `--list-midi-ports`
mais sont sa plomberie interne — s'y connecter directement (via
`--midi-port`) ne recoit rien, verifie empiriquement. Utilisez `--midi-port`
pour forcer un controleur USB precis par nom (ex: `--midi-port Digitakt`)
ou laissez la selection automatique par defaut, qui gere deja correctement
le choix entre USB et `Midi Through` (donc le reseau).

## 8. Interface web de pilotage (optionnel)

Petite page web pour changer le canal MIDI ecoute, le port MIDI d'entree
(USB ou reseau/rtpmidid) et le peripherique de sortie audio sans taper de
commande — pratique une fois le Pi installe sans ecran/clavier branches. Ne
touche jamais au binaire audio directement : elle reecrit
`/etc/default/open303` et redemarre `open303.service` (via un script `sudo`
scope a cette seule action).

Quatre nouveaux flags CLI la rendent possible :
- `--audio-device SOUS-CHAINE` : force la sortie audio dont le nom contient
  cette sous-chaine (ex: `--audio-device USB`). Sans correspondance, repli
  sur la selection automatique (cf section 6) avec un avertissement.
- `--list-devices` : affiche les peripheriques audio disponibles (memes noms
  que ceux vus au demarrage normal) et quitte, sans ouvrir MIDI ni audio.
- `--midi-port SOUS-CHAINE` : force le port MIDI dont le nom contient cette
  sous-chaine (ex: `--midi-port Digitakt`). Sans correspondance, repli sur la
  selection automatique (cf section 7ter) avec un avertissement.
- `--list-midi-ports` : affiche les ports MIDI disponibles et quitte.

**Installation** (apres `systemd/install-service.sh`, sur le Pi) :
```bash
sudo apt install -y python3-flask python3-qrcode
sudo web/install-web.sh
```
L'interface est alors accessible sur `http://<ip-du-pi>:8303`.

**Aucune authentification** : reservez cette interface a un reseau local de
confiance (Wi-Fi maison), ne l'exposez pas directement sur Internet — un
tiers y ayant acces pourrait redemarrer le service ou changer sa config.

## 9. Depannage

Problemes rencontres en pratique et leur solution, dans l'ordre ou ils ont
le plus de chances d'apparaitre.

**`exec format error` pendant `docker buildx build` ou `docker run`**
QEMU n'est pas (ou plus) enregistre pour l'emulation multi-arch.
```bash
docker run --rm --privileged tonistiigi/binfmt --install all
```
A refaire apres chaque redemarrage de Docker Desktop / `wsl --shutdown`.

**`WARNING: No output specified with docker-container driver` puis
`pull access denied` au `docker run` suivant**
Le driver `docker-container` (necessaire pour le multi-plateforme) ne charge
pas automatiquement l'image dans le stockage local. Il faut `--load` dans la
commande `docker buildx build` (deja present dans `build-cross.sh` a jour).

**`invalid mount config for type "bind": bind source path does not exist`**
Builder buildx corrompu (frequent apres un redemarrage de Docker Desktop
sous WSL2). Recreez-le :
```bash
docker buildx rm multiarch
docker buildx create --name multiarch --driver docker-container --bootstrap --use
```

**`CMake Error ... Introuvable: .../Source/rosic`**
Mauvais chemin : le code DSP est dans `Source/DSPCode/`, pas `Source/rosic/`
(deja corrige dans `CMakeLists.txt` fourni).

**Erreurs `memcpy`/`memmove`/`INT_MAX` non declares**
Le code source d'Open303 est ancien et omet `<cstring>`/`<climits>` a
certains endroits ; GCC recent est plus strict que les compilateurs de
l'epoque. Deja corrige via `-include cstring -include climits` dans
`CMAKE_CXX_FLAGS` (pas via `target_compile_options`, qui a tendance a
fragmenter incorrectement les paires `-include <header>` dans certaines
versions de CMake).

**`warning: iteration 2048 invokes undefined behavior`
(`-Waggressive-loop-optimizations`) dans `rosic_MipMappedWaveTable.cpp`**
Bug amont : `prototypeTable` est declare `[tableLength]` (2048) alors que
`initPrototypeTable()` ecrit `tableLength+4` elements — un depassement de 4
doubles dans `tableSet`. Les commentaires du header decrivent pourtant bien
4 echantillons de garde pour l'interpolation (`tableSet`, lui, est bien
declare `[tableLength+4]`) : c'est la declaration qui est fausse. Ce n'est
pas qu'un avertissement cosmetique — GCC s'autorise a supposer que la boucle
n'atteint jamais `i==2048` et peut la restructurer de facon incorrecte a
`-O3`. `docker/build-cross.sh` applique automatiquement le correctif (un
`sed` idempotent) avant de compiler. **En compilation native sur le Pi**
(section 4), appliquez-le a la main une fois :
```bash
sed -i 's/double prototypeTable\[tableLength\];/double prototypeTable[tableLength+4];/' \
  third_party/Open303/Source/DSPCode/rosic_MipMappedWaveTable.h
```

**`unrecognized command-line option '-mfpu=neon-fp-armv8'` en ciblant arm64**
Cache CMake perime : `build-cross/` a ete configure pour une architecture
differente (armv7) lors d'un run precedent. `build-cross.sh` fait desormais
un `rm -rf build-cross` systematique avant chaque build pour eviter ca.

**`noteOn(int&, int)` : no matching function**
La vraie signature est `noteOn(int noteNumber, int velocity, double detune)`,
pas 2 arguments (deja corrige dans `main.cpp`, appels avec `detune=0.0`).

**`librtaudio.so.X => not found` / `librtmidi.so.X => not found` a
l'execution sur le Pi**
Version de Debian differente entre l'image de cross-compilation
(`docker/Dockerfile.cross`) et l'OS reel du Pi → numeros de version de lib
(`.so.6` vs `.so.7`...) incompatibles. Verifiez `cat /etc/os-release` sur le
Pi et alignez `FROM debian:<codename>-slim` dans le Dockerfile en
consequence (ex: `trixie-slim`), puis reconstruisez l'image avec
`--no-cache`.

**`ldd` affiche `not a dynamic executable` alors que le binaire est bien lie
dynamiquement (`file` le confirme)**
Signe indirect d'une architecture qui ne correspond pas a celle de l'OS
installe (ex: binaire armhf 32-bit sur un Pi OS 64-bit sans couche de
compatibilite 32-bit) : `ldd` echoue silencieusement a executer
l'interprete au lieu d'afficher une erreur claire. Verifiez `uname -m` sur
le Pi et recompilez avec la bonne plateforme (`linux/arm/v7` vs
`linux/arm64`).

**`RtAudio alsa: _NOT_ running realtime scheduling` puis xruns**
Priorite temps reel non accordee. Appliquez le reglage `limits.conf` de la
section precedente, **puis deconnectez/reconnectez la session SSH**
(indispensable, un `source` ou un nouveau terminal ne suffit pas).

**Son haché/craquant en continu (meme sur une note tenue, sans xrun signale)**
Deux causes possibles, a distinguer avec `--sine` :

*Si le sinus (`--sine`) est propre mais Open303 craque* : c'etait une
inversion de priorite. Le callback audio prenait un `std::mutex` que le
thread MIDI (priorite normale) pouvait detenir au moment d'etre preempte,
bloquant le thread audio temps reel pendant toute une tranche
d'ordonnancement. **Corrige** : `main.cpp` utilise desormais une file
d'evenements MIDI lock-free (ring buffer SPSC) videe en debut de bloc audio,
plus aucun verrou dans le chemin temps reel. Si vous voyez
`[midi] N evenement(s) MIDI perdu(s)`, c'est que la file (256 evenements)
deborde — signe d'un bloc audio anormalement long.

*Si le sinus craque aussi, sur la sortie jack embarquee du Pi* :
contre-intuitivement, c'est parfois le scheduling temps reel lui-meme qui en
est la cause sur ce driver (`bcm2835`) : une priorite RT trop elevee sur le
thread audio peut perturber le thread noyau gerant les interruptions du DMA
audio. Diagnostic :
```bash
speaker-test -c 2 -t sine -f 440    # son propre ? -> le materiel/pilote est sain
./open303_pi_host 44100 640 --sine  # sinus via notre callback, sans RT (defaut) et sans Open303
```
Si le sinus est propre sans `--rt` mais haché avec, n'utilisez pas `--rt` sur
la sortie jack embarquee (c'est le comportement par defaut du programme).
Reservez `--rt` a une carte son USB.

**`cpufrequtils`/`cpufreq-set` introuvables**
Package obsolete depuis Debian trixie, remplace par `linux-cpupower`/
`cpupower` (cf section 6).

## 10. Aller plus loin

- **Sequenceur interne / arpegiateur** : le TB-303 original a un
  sequenceur pas-a-pas ; ce projet se contente de jouer les notes recues en
  direct. Si vous voulez le pattern-sequencer, regardez plutot le projet
  `jc303` (github.com/midilab/jc303), qui embarque Open303 dans un plugin
  LV2/VST/CLAP complet avec GUI — hebergeable sur Pi via un hote LV2 leger
  comme `jalv` + JACK, si vous preferez ne pas coder l'integration vous-meme.
