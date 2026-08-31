// open303_pi_host
// -----------------
// Petit hote "standalone" qui:
//   1) ouvre un port MIDI USB (classe-compliant) via RtMidi
//   2) pilote le moteur DSP Open303 (lib "rosic" de Robin Schmidt)
//   3) sort l'audio via RtAudio (ALSA sur Raspberry Pi)
//
// Ce fichier ne contient PAS le moteur DSP lui-meme (proprietaire au sens
// "code d'un tiers", licence MIT) : il faut cloner RobinSchmidt/Open303 a cote
// (voir README.md) puis pointer CMake dessus.
//
// NOTE IMPORTANTE: la classe rosic::Open303 evolue de temps en temps. Les
// noms de methodes utilises ci-dessous (setCutoff, setResonance, setEnvMod,
// setDecay, setAccent, setVolume, setWaveform, setTuning, setSampleRate,
// noteOn, getSample) sont ceux documentes/utilises publiquement pour ce
// moteur. Si votre version du header differe legerement, ajustez les appels
// en consequence (grep "void set" Source/DSPCode/rosic_Open303.h).

#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#include "RtAudio.h"
#include "RtMidi.h"
#include "rosic_Open303.h"   // Source/DSPCode/rosic_Open303.h dans le repo Open303

namespace {

rosic::Open303 gSynth;
std::atomic<bool> gKeepRunning{true};
std::atomic<bool> gSineTestMode{false};
std::atomic<bool> gEnableRealtime{false};

// Niveau crete du bloc audio, publie par le thread temps reel et lu par la
// boucle principale (cf --meter). Sert a distinguer sans ambiguite deux
// causes de "pas de son" qui se ressemblent de l'exterieur :
//   - le moteur DSP ne produit rien (niveau a 0 alors que des notes
//     arrivent) -> probleme de parametres/MIDI ;
//   - le moteur produit bien du signal mais on n'entend rien -> probleme
//     en aval (carte son, cablage, volume materiel).
// Un simple max de valeurs absolues : pas d'allocation ni de verrou, donc
// sans danger dans le callback audio.
std::atomic<float> gPeakLevel{0.0f};

// ---------------------------------------------------------------------
// Overdrive (saturation analogique)
// ---------------------------------------------------------------------
// Inspire de la section OVERDRIVE de jc303, mais PAS un portage : jc303
// utilise un reseau LSTM (RTNeural/GuitarML, hidden=40) evalue a chaque
// echantillon. Mesure sur ce Pi 3B+ : 14,6 us par echantillon, soit 64 % d'un
// coeur en continu, pour un budget de 22,7 us a 44,1 kHz -- incompatible avec
// notre buffer de 64 frames (0,93 ms consommes sur 1,45 ms), sans compter
// l'ajout de JUCE + chowdsp + RTNeural comme dependances.
//
// A la place : un waveshaper tanh classique (soft clipping), qui est de toute
// facon le type de saturation qu'on entend sur la plupart des disques acid.
// Cout : quelques operations par echantillon, aucune allocation, aucun verrou.
//
// Les trois parametres sont lus par le thread audio et ecrits par le thread
// MIDI : atomiques relaxed, aucune dependance entre eux (une valeur lue "en
// retard" d'un bloc est sans consequence audible).
std::atomic<float> gDriveAmount{0.0f};  // 0 = desactive (bypass exact)
std::atomic<float> gDriveTone{1.0f};    // 0 = sombre, 1 = brillant
std::atomic<float> gDriveMix{1.0f};     // 0 = dry, 1 = 100 % sature

// Etat du filtre de tonalite (passe-bas 1 pole), touche uniquement par le
// thread audio.
float gDriveToneState = 0.0f;

// Coefficients derives des trois parametres, recalcules UNE FOIS PAR BLOC
// (et non par echantillon) : les atomiques ne sont lues qu'une fois, et les
// tanh de normalisation sortent de la boucle interne.
struct DriveCoeffs {
  bool  active;
  float gain;
  float bias;
  float tanhBias;
  float norm;
  float toneCoeff;  // < 1 => filtrage actif
  float mix;
};

DriveCoeffs computeDriveCoeffs() {
  DriveCoeffs d;
  const float amount = gDriveAmount.load(std::memory_order_relaxed);
  d.active = amount > 0.0f;
  if (!d.active) return d;

  // Gain plafonne a 6.5x, valeur choisie d'apres une mesure sur le SIGNAL
  // REEL du moteur (tools/overdrive_probe.cpp) et non sur une sinusoide.
  // Deux enseignements de cette mesure, invisibles autrement :
  //  - la sortie du 303 est DEJA tres riche en harmoniques sans overdrive
  //    (H2/H1 = 0.425 a drive nul) : la saturation ne fait donc que decaler
  //    des rapports deja eleves, d'ou un effet bien moins spectaculaire que
  //    sur une sinusoide pure ;
  //  - c'est la densite (RMS) qui s'entend le plus, et elle saturait des 70 %
  //    de course avec un gain de 12x (+0.073 puis +0.023 puis +0.010) : le
  //    dernier tiers du potard ne servait a rien.
  // A 6.5x, le RMS progresse de 0.121 a 0.317 de facon nettement plus reguliere
  // sur toute la course.
  d.gain = 1.0f + 5.5f * amount * amount;

  // La progression en haut de course vient donc de l'ASYMETRIE, pas du gain.
  // Decaler l'onde avant l'ecretage change le rapport cyclique : le carre
  // devient une impulsion, ce qui fait basculer le spectre des harmoniques
  // impaires vers les paires -- un changement de NATURE, que le gain seul ne
  // peut pas produire une fois la saturation atteinte.
  // Courbe quadratique, volontairement moderee. Une version plus agressive
  // (2.6*a^3) a ete essayee : elle ecrasait l'alternance positive a 0.01 en
  // butee, produisant une impulsion filiforme, maigre et plus faible
  // (fondamentale a 0.566 contre 0.813). Ici l'onde garde du corps tout en
  // s'asymetrisant : entre 70 % et 100 % de drive, H2/H1 va de 0.097 a 0.114,
  // H3/H1 de 0.235 a 0.287 et H4/H1 de 0.061 a 0.096 -- l'evolution se
  // poursuit jusqu'en butee, sans degenerer.
  d.bias     = 0.8f * amount * amount;
  d.tanhBias = std::tanh(d.bias);
  // Normalisation sur la plus grande des DEUX cretes. Ne considerer que la
  // positive (premiere version) faisait exploser l'alternance negative des
  // que le bias montait : pour une entree +/-0.5, la sortie atteignait -6.3 a
  // 70 % de drive, -24 a 85 %, -181 a 100 %. Tout cela etait ecrete
  // brutalement par la carte son, si bien que les positions hautes du potard
  // sonnaient toutes pareil -- non par limite harmonique, mais parce
  // qu'elles produisaient la meme bouillie saturee.
  const float peakPos = std::fabs(std::tanh(d.gain + d.bias) - d.tanhBias);
  const float peakNeg = std::fabs(std::tanh(-d.gain + d.bias) - d.tanhBias);
  d.norm = 1.0f / std::fmax(peakPos, peakNeg);

  const float tone = gDriveTone.load(std::memory_order_relaxed);
  d.toneCoeff = (tone < 1.0f) ? (0.05f + 0.95f * tone) : 1.0f;
  d.mix       = gDriveMix.load(std::memory_order_relaxed);
  return d;
}

// Applique la saturation a un echantillon. Renvoie l'entree telle quelle
// quand le drive est a zero, pour que "overdrive eteint" soit un bypass
// strict et non une approximation.
inline float applyOverdrive(float x, const DriveCoeffs& d) {
  if (!d.active) return x;

  float y = (std::tanh(x * d.gain + d.bias) - d.tanhBias) * d.norm;

  // Tonalite : passe-bas 1 pole pour adoucir les harmoniques hautes que la
  // saturation vient de creer. tone=1 laisse passer tel quel.
  if (d.toneCoeff < 1.0f) {
    gDriveToneState += d.toneCoeff * (y - gDriveToneState);
    y = gDriveToneState;
  }

  return x + d.mix * (y - x);
}
double gSinePhase = 0.0;
// Fixe avant l'ouverture du flux (donc avant que le thread audio existe) :
// pas de synchronisation necessaire pour cette lecture depuis audioCallback.
double gSampleRate = 44100.0;

// Canal MIDI ecoute (0-15, -1 = tous les canaux). Fixe une fois dans main()
// avant l'ouverture du port MIDI (donc avant que le thread MIDI existe) :
// pas de synchronisation necessaire pour cette lecture depuis
// handleMidiMessage. Reglable via --channel N (cf --help), utile avec un
// sequenceur multi-pistes (ex: Digitakt) pour dedier une piste au 303.
int gMidiChannel = -1;

// Plage de pitch bend standard (+/- 2 demi-tons), comme la plupart des
// synthes/DAW par defaut. setPitchBend() de rosic::Open303 attend un decalage
// en demi-tons.
constexpr double kPitchBendRangeSemitones = 2.0;

void onSignal(int) { gKeepRunning = false; }

// ---------------------------------------------------------------------
// File d'evenements MIDI lock-free (un producteur / un consommateur)
// ---------------------------------------------------------------------
// Le callback audio ne doit JAMAIS prendre de verrou. Le thread MIDI tourne
// a priorite normale : s'il est preempte par l'ordonnanceur alors qu'il
// detient le mutex, le thread audio temps reel se retrouve bloque a
// l'attendre le temps d'une tranche complete d'ordonnancement -> inversion
// de priorite, et donc des craquements periodiques d'autant plus marques que
// la priorite RT du thread audio est elevee (cf README, section Depannage :
// c'est le symptome "son hache, pire avec --rt").
//
// A la place : le thread MIDI pousse les octets bruts dans un ring buffer,
// et le thread audio les consomme au debut de chaque bloc. Aucune allocation,
// aucun verrou, aucun appel systeme dans le chemin temps reel.
struct MidiEvent {
  unsigned char status;
  unsigned char data1;
  unsigned char data2;
};

constexpr size_t kMidiQueueSize = 256;  // doit etre une puissance de 2
MidiEvent gMidiQueue[kMidiQueueSize];
std::atomic<size_t> gMidiWrite{0};
std::atomic<size_t> gMidiRead{0};
std::atomic<unsigned> gMidiDropped{0};

// Appele depuis le thread MIDI uniquement.
void pushMidiEvent(const MidiEvent& ev) {
  const size_t w    = gMidiWrite.load(std::memory_order_relaxed);
  const size_t next = (w + 1) & (kMidiQueueSize - 1);
  if (next == gMidiRead.load(std::memory_order_acquire)) {
    // File pleine (le thread audio n'a pas encore consomme) : on jette
    // l'evenement plutot que de bloquer. Signale dans la boucle principale.
    gMidiDropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  gMidiQueue[w] = ev;
  gMidiWrite.store(next, std::memory_order_release);
}

// ---------------------------------------------------------------------
// Traduction MIDI -> parametres Open303 (executee dans le thread audio)
// ---------------------------------------------------------------------
void applyMidiEvent(const MidiEvent& ev) {
  switch (ev.status & 0xF0) {
    case 0x90:  // Note On (velocite 0 == Note Off, convention MIDI standard)
      // Accent et slide sont geres par le moteur lui-meme : une velocite
      // >= 100 declenche l'accent, et deux notes qui se recouvrent (jeu
      // legato) declenchent un slide de 60 ms. Rien a mapper ici.
      gSynth.noteOn(ev.data1, ev.data2, 0.0);
      break;

    case 0x80:  // Note Off
      gSynth.noteOn(ev.data1, 0, 0.0);
      break;

    case 0xB0: {  // Control Change
      const int    cc  = ev.data1;
      const double v01 = ev.data2 / 127.0;  // 0..1

      switch (cc) {
        case 74: {  // Cutoff, mapping log borne au domaine mesure par
                     // calculateEnvModScalerAndOffset() (313.8-2394.4 Hz,
                     // cf rosic_Open303.cpp) : au-dela, envScaler/envOffset
                     // sont extrapoles lineairement hors des mesures faites
                     // sur un vrai 303, et le suivi de l'env mod se degrade.
          constexpr double kCutoffMinHz = 313.8152786059267;
          constexpr double kCutoffMaxHz = 2394.411986817546;
          gSynth.setCutoff(kCutoffMinHz * std::pow(kCutoffMaxHz / kCutoffMinHz, v01));
          break;
        }
        case 71:  // Resonance, en POURCENTS (0..100)
          gSynth.setResonance(100.0 * v01);
          break;
        case 73:  // Decay de l'enveloppe filtre (ms) - plage du 303 d'origine
          gSynth.setDecay(200.0 + 1800.0 * v01);
          break;
        case 5:   // Portamento Time (CC standard) -> temps de slide en ms
                  // (defaut du moteur: 60 ms, declenche par legato)
          gSynth.setSlideTime(5.0 + 495.0 * v01);
          break;
        case 1:   // Mod wheel -> env mod. ATTENTION: setEnvMod attend des
                  // POURCENTS (0..100), pas 0..1 : en interne le moteur fait
                  // linToLin(envMod, 0, 100, 0, 1). Envoyer 0..1 revient a
                  // n'avoir quasiment aucune modulation d'enveloppe, or c'est
                  // le parametre signature du 303.
          gSynth.setEnvMod(100.0 * v01);
          break;
        case 7:   // Volume (en dB, -40..0)
          gSynth.setVolume(-40.0 + 40.0 * v01);
          break;
        case 70:  // Waveform (0 = saw, 1 = square, cf doc Open303)
          gSynth.setWaveform(v01);
          break;

        // CC20-27 : plage "undefined" du spec MIDI 1.0 (aucune collision avec
        // un usage standard), utilisee pour les parametres Open303 restants
        // qui sont RT-safe (simples mises a jour de coefficient, pas de
        // reallocation ni de recalcul de table). setSquarePhaseShift,
        // setTanhShaperDrive et setTanhShaperOffset sont volontairement
        // EXCLUS d'ici : ils regenerent la wavetable mip-mappee entiere
        // (fillWithSquare303 -> generateMipMap, FFT) a chaque appel, ce qui
        // depuis ce thread audio provoquerait le meme type de craquement que
        // l'ancienne inversion de priorite (cf recap-open303-pi.md). Ils sont
        // reglables uniquement au demarrage via --square-phase/--tanh-drive/
        // --tanh-offset (cf --help).
        case 20:  // Attaque enveloppe filtre, notes non accentuees (ms).
                  // Plage Devil Fish : 0.3-30 ms (defaut moteur : 3 ms).
          gSynth.setNormalAttack(0.3 + 29.7 * v01);
          break;
        case 21:  // Attaque enveloppe filtre, notes accentuees (ms). Sur le
                  // 303 d'origine, fixe a 3 ms ; ici reglable comme sur la
                  // Devil Fish (meme plage 0.3-30 ms).
          gSynth.setAccentAttack(0.3 + 29.7 * v01);
          break;
        case 22:  // Highpass avant le filtre principal (Hz). Defaut moteur :
                  // ~44.5 Hz.
          gSynth.setPreFilterHighpass(500.0 * v01);
          break;
        case 23:  // Highpass dans la boucle de feedback du filtre (Hz).
                  // Defaut moteur : 150 Hz. Plage large (0-500 Hz) VOULUE,
                  // malgre un knob peu progressif a l'oreille.
                  //
                  // Ce filtre tourne dans TeeBeeFilter, donc au taux
                  // SUR-ECHANTILLONNE x4 (176.4 kHz a 44.1 kHz de sortie).
                  // Or OnePoleFilter calcule x = exp(-2*pi*fc/sampleRate),
                  // et la part du signal reellement filtree vaut ~(1-x) :
                  //    15 Hz -> 0.05 %,  60 Hz -> 0.21 %,
                  //   150 Hz -> 0.53 %, 500 Hz -> 1.77 %
                  // Autrement dit, meme en butee l'effet reste tenu, et il
                  // est concentre dans les tout premiers Hz (passage
                  // "filtre inactif -> actif"). Des tests a l'oreille ont
                  // confirme qu'on n'entend une difference qu'entre 0 et le
                  // premier palier, quelle que soit la plage.
                  //
                  // Resserrer la plage est donc CONTRE-PRODUCTIF (essaye :
                  // 0-150 puis 0-60 Hz -> effet maximal divise par 3 puis
                  // par 8, sans gagner en progressivite). On garde 0-500 Hz,
                  // qui offre le maximum d'effet disponible.
          gSynth.setFeedbackHighpass(500.0 * v01);
          break;
        case 24:  // Highpass apres le filtre principal (Hz). Defaut moteur :
                  // ~24.2 Hz.
          gSynth.setPostFilterHighpass(500.0 * v01);
          break;
        case 25:  // Sustain de l'enveloppe d'amplitude (dB). Sur le 303
                  // d'origine, fixe a 0 (silence, enveloppe percussive) ;
                  // la Devil Fish l'ouvre en pourcentage du volume plein.
                  // Ici : -60 dB (quasi silence) .. 0 dB (plein volume tenu).
          gSynth.setAmpSustain(-60.0 + 60.0 * v01);
          break;
        case 26:  // Decay de l'enveloppe d'amplitude (ms). Plage Devil Fish :
                  // 16-3000 ms (defaut moteur : ~1230 ms).
          gSynth.setAmpDecay(16.0 + 2984.0 * v01);
          break;
        case 27:  // Release de l'enveloppe d'amplitude (ms), notes non
                  // accentuees uniquement (pas de setter public pour le
                  // release des notes accentuees). Defaut moteur tres court
                  // (~1 ms, comportement percussif d'origine) ; plage choisie
                  // 1-500 ms pour un balayage musicalement utile.
          gSynth.setAmpRelease(1.0 + 499.0 * v01);
          break;
        case 28:  // Tuning, frequence de reference de La4 (Hz). Plage
                  // +/-1 demi-ton autour de 440 Hz (defaut moteur : 440 Hz),
                  // comme un knob de tune classique.
          gSynth.setTuning(415.0 + 51.0 * v01);
          break;
        case 29:  // Intensite de l'accent (%), distincte du DECLENCHEMENT de
                  // l'accent (vélocite >= 100, deja gere par le moteur sans
                  // mapping). Defaut moteur : 50%.
          gSynth.setAccent(100.0 * v01);
          break;

        // CC30-32 : overdrive (waveshaper maison, cf applyOverdrive).
        // Ecrits ici depuis le thread audio via des atomiques relaxed ; le
        // traitement lui-meme coute quelques operations par echantillon,
        // contre 64 % d'un coeur pour le modele neuronal de jc303 (mesure).
        case 30:  // Drive : 0 = bypass strict, 1 = saturation maximale.
          gDriveAmount.store(static_cast<float>(v01), std::memory_order_relaxed);
          break;
        case 31:  // Tonalite apres saturation : 0 = sombre, 1 = brillant.
          gDriveTone.store(static_cast<float>(v01), std::memory_order_relaxed);
          break;
        case 32:  // Dry/wet : 0 = signal propre, 1 = 100 % sature.
          gDriveMix.store(static_cast<float>(v01), std::memory_order_relaxed);
          break;

        case 123:  // All Notes Off (CC standard MIDI). Emis aussi en interne
                   // lors d'un rebranchement de port MIDI : si la session
                   // reseau tombe pendant qu'une note est tenue, le note-off
                   // n'arrive jamais et la note reste bloquee -- le moteur
                   // tourne alors indefiniment (constate en pratique : CPU
                   // anormalement eleve au repos).
                   //
                   // NB: allNotesOff() vide une std::list, donc liberation
                   // memoire dans le thread audio. Ce n'est pas ideal en
                   // temps reel, mais ca n'introduit pas de nouveau probleme :
                   // noteOn() y fait deja un push_front() (allocation) a
                   // chaque note dans ce meme thread.
          gSynth.allNotesOff();
          break;

        default:
          break;  // CC non mappe : ignore
      }
      break;
    }

    case 0xE0: {  // Pitch bend, 14 bits (data1=LSB, data2=MSB), centre=8192
      const int    raw14 = (static_cast<int>(ev.data2) << 7) | ev.data1;
      const double norm  = (raw14 - 8192) / 8192.0;  // -1..~1
      gSynth.setPitchBend(norm * kPitchBendRangeSemitones);
      break;
    }

    default:
      break;  // aftertouch, sysex... ignores pour l'instant
  }
}

// Appele depuis le thread audio uniquement, en debut de bloc.
void drainMidiQueue() {
  size_t       r = gMidiRead.load(std::memory_order_relaxed);
  const size_t w = gMidiWrite.load(std::memory_order_acquire);
  while (r != w) {
    applyMidiEvent(gMidiQueue[r]);
    r = (r + 1) & (kMidiQueueSize - 1);
  }
  gMidiRead.store(r, std::memory_order_release);
}

// ---------------------------------------------------------------------
// Callback RtMidi : ne touche PAS au synthe, se contente d'empiler
// ---------------------------------------------------------------------
// Journalise un evenement MIDI recu (canal deja filtre) sur stdout. Ce
// thread (RtMidi, priorite normale) n'est pas RT-sensible -- contrairement a
// applyMidiEvent()/audioCallback(), un printf ici ne pose aucun probleme de
// temps reel. Utile pour verifier ce qui arrive reellement (ex: debogage
// d'une source reseau rtpmidid) sans outil externe (aseqdump).
void logMidiEvent(const MidiEvent& ev) {
  const int channel = ev.status & 0x0F;
  switch (ev.status & 0xF0) {
    case 0x90:
      if (ev.data2 == 0) {
        std::printf("[midi] Note Off ch=%d note=%d\n", channel, ev.data1);
      } else {
        std::printf("[midi] Note On  ch=%d note=%d vel=%d%s\n", channel, ev.data1, ev.data2,
                    ev.data2 >= 100 ? " (accent)" : "");
      }
      break;
    case 0x80:
      std::printf("[midi] Note Off ch=%d note=%d\n", channel, ev.data1);
      break;
    case 0xB0:
      std::printf("[midi] CC       ch=%d cc=%d val=%d\n", channel, ev.data1, ev.data2);
      break;
    case 0xE0:
      std::printf("[midi] Pitch Bend ch=%d value=%d\n", channel,
                  (static_cast<int>(ev.data2) << 7) | ev.data1);
      break;
    default:
      std::printf("[midi] status=0x%02X data1=%d data2=%d\n", ev.status, ev.data1, ev.data2);
      break;
  }
}

void handleMidiMessage(double /*deltatime*/, std::vector<unsigned char>* message, void* /*userData*/) {
  if (!message || message->size() < 2) return;
  const auto& msg = *message;

  // Filtrage de canal (gMidiChannel = -1 : tous les canaux).
  if (gMidiChannel >= 0 && (msg[0] & 0x0F) != static_cast<unsigned char>(gMidiChannel)) return;

  MidiEvent ev;
  ev.status = msg[0];
  ev.data1  = msg[1];
  ev.data2  = (msg.size() > 2) ? msg[2] : 0;
  logMidiEvent(ev);
  pushMidiEvent(ev);
}

// ---------------------------------------------------------------------
// Callback audio RtAudio : appele par le thread temps reel
// ---------------------------------------------------------------------
int audioCallback(void* outputBuffer, void* /*inputBuffer*/, unsigned int nFrames,
                   double /*streamTime*/, RtAudioStreamStatus status, void* /*userData*/) {
  if (status) {
    std::fprintf(stderr, "[audio] xrun detecte\n");
  }

  float* out = static_cast<float*>(outputBuffer);

  // Consommation des evenements MIDI en attente (sans verrou).
  drainMidiQueue();

  if (gSineTestMode.load()) {
    // Mode diagnostic : sinus 440 Hz genere directement ici, sans passer
    // par Open303. Sert a isoler si un souci audio vient du callback/RtAudio
    // ou du moteur DSP.
    constexpr double kFreq = 440.0;
    const double phaseInc = 2.0 * M_PI * kFreq / gSampleRate;
    for (unsigned int i = 0; i < nFrames; ++i) {
      float s = static_cast<float>(0.2 * std::sin(gSinePhase));
      out[2 * i + 0] = s;
      out[2 * i + 1] = s;
      gSinePhase += phaseInc;
      if (gSinePhase > 2.0 * M_PI) gSinePhase -= 2.0 * M_PI;
    }
    return 0;
  }

  // Une seule lecture des atomiques et des tanh de normalisation par bloc.
  const DriveCoeffs drive = computeDriveCoeffs();

  float peak = 0.0f;
  for (unsigned int i = 0; i < nFrames; ++i) {
    float s = applyOverdrive(static_cast<float>(gSynth.getSample()), drive);
    out[2 * i + 0] = s;  // gauche
    out[2 * i + 1] = s;  // droite (mono duplique)
    const float a = std::fabs(s);
    if (a > peak) peak = a;
  }
  // Publication du niveau crete pour --meter. On garde le maximum observe
  // depuis la derniere lecture (la boucle principale remet a zero), sinon un
  // bloc silencieux effacerait la crete d'une note jouee juste avant.
  float previous = gPeakLevel.load(std::memory_order_relaxed);
  while (peak > previous &&
         !gPeakLevel.compare_exchange_weak(previous, peak, std::memory_order_relaxed)) {
  }
  return 0;
}

void printUsage(const char* progName) {
  std::printf(
      "Usage: %s [sample_rate] [buffer_frames] [options]\n"
      "\n"
      "Hote MIDI USB standalone pour le moteur DSP Open303 (clone TB-303).\n"
      "\n"
      "Arguments positionnels (optionnels, dans cet ordre) :\n"
      "  sample_rate      Frequence d'echantillonnage en Hz (defaut: 44100)\n"
      "  buffer_frames    Taille de buffer audio en frames (defaut: 256 ;\n"
      "                   sur Pi 3B+, ne descendez sous 128 qu'en l'absence\n"
      "                   de xruns, cf README section 5bis)\n"
      "\n"
      "Options :\n"
      "  --rt             Active le scheduling temps reel. Deconseille sur la\n"
      "                   sortie jack embarquee de certains Pi (driver bcm2835),\n"
      "                   generalement benefique avec une carte son USB.\n"
      "                   Cf README section Depannage.\n"
      "  --sine           Remplace Open303 par un sinus 440 Hz de diagnostic,\n"
      "                   utile pour isoler un souci audio du moteur DSP.\n"
      "  --channel N      Filtre les evenements MIDI recus sur le canal N\n"
      "                   (0-15). Par defaut : tous les canaux sont ecoutes.\n"
      "  --square-phase D Dephasage de l'onde carree par rapport au saw, en\n"
      "                   degres (defaut: 180). Reglable uniquement ici (pas\n"
      "                   de CC) : regenere la wavetable, trop couteux pour\n"
      "                   etre appele depuis le thread audio.\n"
      "  --tanh-drive DB  Drive (dB) du shaper tanh de l'onde carree 303\n"
      "                   (defaut: 36.9). Meme contrainte que --square-phase.\n"
      "  --tanh-offset V  Offset DC du shaper tanh de l'onde carree 303\n"
      "                   (defaut: 4.37). Meme contrainte que --square-phase.\n"
      "  --audio-device S Force le peripherique de sortie dont le nom contient\n"
      "                   la sous-chaine S (ex: \"USB\"). Sans correspondance,\n"
      "                   repli sur la selection automatique avec avertissement.\n"
      "                   Par defaut : le premier peripherique qui n'est pas\n"
      "                   une sortie embarquee du Pi.\n"
      "  --list-devices   Affiche les peripheriques audio disponibles et quitte\n"
      "                   (n'ouvre ni MIDI ni flux audio).\n"
      "  --midi-port S    Force le port MIDI dont le nom contient la sous-chaine\n"
      "                   S (ex: \"Digitakt\", ou le nom d'une session rtpmidid).\n"
      "                   Sans correspondance, repli sur la selection\n"
      "                   automatique avec avertissement. Par defaut : le\n"
      "                   premier port qui n'est pas \"Midi Through\".\n"
      "  --midi-source S  Politique de choix quand --midi-port n'est pas donne :\n"
      "                   auto (defaut) = controleur USB si present, sinon\n"
      "                   session reseau ; usb = uniquement un controleur\n"
      "                   materiel ; network = uniquement une session rtpmidid.\n"
      "                   Utile parce qu'un port USB n'existe que si l'appareil\n"
      "                   est branche : le mode reste valable et prend effet au\n"
      "                   branchement (topologie resondee toutes les 400 ms).\n"
      "  --list-midi-ports Affiche les ports MIDI disponibles et quitte.\n"
      "  --meter          Affiche le niveau crete de la sortie audio toutes les\n"
      "                   200 ms. Diagnostic : distingue \"le moteur ne produit\n"
      "                   rien\" (silence alors que des notes arrivent) de \"le\n"
      "                   moteur produit du signal mais on n'entend rien\"\n"
      "                   (probleme en aval : carte son, cablage, volume).\n"
      "  --meter-file P   Ecrit ce meme niveau crete dans le fichier P toutes\n"
      "                   les 200 ms (sans l'afficher). Prevu pour /run (tmpfs),\n"
      "                   lu par l'interface web. Ecriture faite depuis la\n"
      "                   boucle principale, jamais depuis le thread audio.\n"
      "  -h, --help       Affiche cette aide et quitte.\n"
      "\n"
      "Exemples :\n"
      "  %s\n"
      "  %s 44100 128 --rt\n"
      "  %s 48000 256 --channel 0\n"
      "  %s --list-devices\n"
      "  %s --audio-device USB\n"
      "  %s --midi-port Digitakt\n",
      progName, progName, progName, progName, progName, progName, progName);
}

// preferredSubstring (optionnel, cf --midi-port) force le choix du premier
// port dont le nom la contient -- utile pour choisir explicitement entre un
// controleur USB et une session reseau rtpmidid (cf README section MIDI
// sans fil). Repli silencieux sur la logique automatique ci-dessous si rien
// ne correspond, comme pour --audio-device.
// verbose == false : selection silencieuse, utilisee par la surveillance
// periodique de la topologie ALSA (cf boucle principale) qui tourne toutes
// les 2 s et ne doit pas inonder les logs.
// Politique de selection quand aucun port precis n'est impose (--midi-port).
// Utile parce qu'un port USB n'existe QUE si un controleur est branche : on ne
// peut donc pas le choisir a l'avance par son nom. Choisir un MODE permet de
// dire "prends l'USB des qu'il arrive", la surveillance de topologie (toutes
// les 400 ms) faisant le reste au branchement.
enum class MidiSource { Auto, Usb, Network };

int pickMidiPort(RtMidiIn& midiin, const std::string& preferredSubstring = "",
                 MidiSource source = MidiSource::Auto, bool verbose = true) {
  unsigned int nPorts = midiin.getPortCount();
  if (nPorts == 0) {
    if (verbose) {
      std::fprintf(stderr, "Aucun port MIDI trouve. Branchez votre interface USB MIDI.\n");
    }
    return -1;
  }
  if (verbose) std::printf("Ports MIDI disponibles:\n");
  int preferredMatch = -1;
  for (unsigned int i = 0; i < nPorts; ++i) {
    const std::string name = midiin.getPortName(i);
    if (verbose) std::printf("  [%u] %s\n", i, name.c_str());
    if (!preferredSubstring.empty() && preferredMatch < 0 &&
        name.find(preferredSubstring) != std::string::npos) {
      preferredMatch = static_cast<int>(i);
    }
  }
  if (!preferredSubstring.empty()) {
    if (preferredMatch >= 0) return preferredMatch;
    if (verbose) {
      std::fprintf(stderr,
                   "--midi-port: aucun port ne contient \"%s\", repli sur la selection "
                   "automatique.\n",
                   preferredSubstring.c_str());
    }
  }
  // "Midi Through" est un port virtuel cree par le noyau ALSA, toujours
  // present en position 0, et ne correspond a aucun materiel branche. Le
  // prendre par defaut (comme le faisait un simple "return 0") revient a
  // ignorer silencieusement le vrai controleur USB des qu'il en existe un.
  // On prend donc le premier port qui n'est ni "Midi Through" ni l'un des
  // DEUX ports de service internes de rtpmidid, "Network Export" et
  // "Announcements" -- ils existent toujours des que rtpmidid tourne, mais
  // n'acheminent rien vers un abonne tiers (verifie empiriquement : aucune
  // activite du synthe lors d'un test aseqsend cible sur "Midi Through"
  // alors que le client ecoutait "rtpmidid:Network Export").
  //
  // ATTENTION : ne PAS exclure tout nom contenant juste "rtpmidid" -- ses
  // ports par-pair reels (ex: "rtpmidid:NimH-PC", crees par
  // rtpmidi_discover/rtpmidi_announce des qu'un pair reseau se connecte ou
  // est decouvert) contiennent aussi ce prefixe et sont, eux, de vraies
  // sources MIDI fonctionnelles a ne pas ignorer (bug constate en pratique :
  // un pair reseau reellement connecte, visible dans les logs rtpmidid,
  // restait invisible a la selection automatique).
  //
  // Parmi les candidats restants on distingue deux familles, car elles ne se
  // choisissent pas de la meme facon :
  //
  //  - un vrai peripherique materiel (ex: "Elektron Digitakt II") : on prend
  //    le PREMIER, et il reste prioritaire sur le reseau ;
  //  - un pair reseau rtpmidid (ex: "rtpmidid:PyMIDI") : on prend le DERNIER.
  //    rtpmidid cree un nouveau port ALSA a chaque session SANS supprimer
  //    ceux des sessions terminees ; les numeros de port ALSA etant
  //    croissants, le dernier de la liste est donc le plus recent, seul
  //    susceptible d'etre vivant. Prendre le premier revenait a s'abonner a
  //    un port mort -- bug constate en pratique : relancer deux fois de suite
  //    un script de test faisait disparaitre le son a la 2e execution, la
  //    session #2 emettant sur un nouveau port pendant qu'on ecoutait encore
  //    celui de la session #1.
  int hardwareMatch    = -1;  // premier materiel reel
  int newestNetworkPeer = -1;  // dernier pair rtpmidid vu
  for (unsigned int i = 0; i < nPorts; ++i) {
    const std::string name = midiin.getPortName(i);
    if (name.find("Midi Through") != std::string::npos ||
        name.find("Network Export") != std::string::npos ||
        name.find("Announcements") != std::string::npos) {
      continue;
    }
    if (name.find("rtpmidid") != std::string::npos) {
      newestNetworkPeer = static_cast<int>(i);
    } else if (hardwareMatch < 0) {
      hardwareMatch = static_cast<int>(i);
    }
  }
  switch (source) {
    case MidiSource::Usb:
      // On ignore volontairement le reseau : l'utilisateur veut son
      // controleur materiel. S'il n'est pas encore branche, on tombe sur
      // "Midi Through" plus bas (inerte mais valide) et la surveillance de
      // topologie basculera dessus des qu'il apparait.
      if (hardwareMatch >= 0) return hardwareMatch;
      break;
    case MidiSource::Network:
      // Symetrique : on ignore un eventuel controleur USB branche.
      if (newestNetworkPeer >= 0) return newestNetworkPeer;
      break;
    case MidiSource::Auto:
      if (hardwareMatch >= 0) return hardwareMatch;
      if (newestNetworkPeer >= 0) return newestNetworkPeer;
      break;
  }
  // Repli commun : "Midi Through". Rien d'autre d'utilisable, ou le mode
  // demande n'a pas (encore) de candidat.
  for (unsigned int i = 0; i < nPorts; ++i) {
    if (midiin.getPortName(i).find("Midi Through") != std::string::npos) {
      return static_cast<int>(i);
    }
  }
  return 0;
}

// Choisit le device de sortie audio. Meme logique que pickMidiPort() :
// dac.getDefaultOutputDevice() renvoie le device "par defaut" tel que defini
// par ALSA, qui reste la sortie embarquee du Pi (bcm2835/vc4hdmi) meme
// lorsqu'une carte son USB est branchee -- ALSA ne priorise pas le materiel
// externe automatiquement. Or le DAC embarque est nettement moins bon (cf
// README) : on prefere donc explicitement tout device qui n'est PAS l'une
// des sorties embarquees connues du Pi, si un tel device existe.
//
// "Default ALSA Device" est une entree synthetique ajoutee par le backend
// ALSA de RtAudio (constatee en pratique sur RtAudio 6.0.1, listee en
// premier) qui redirige elle-meme vers le device ALSA "default" -- donc vers
// bcm2835 sur ce Pi. Elle ne contient ni "bcm2835" ni "vc4hdmi" dans son nom
// et etait donc choisie a tort avant meme d'atteindre la vraie carte USB :
// exclue explicitement, comme les sorties embarquees.
//
// preferredSubstring (optionnel, cf --audio-device) force le choix du
// premier device dont le nom la contient, prioritaire sur la logique
// d'exclusion automatique ci-dessus. Repli silencieux sur cette derniere si
// rien ne correspond (juste un avertissement), plutot que d'echouer : une
// config perimee (materiel debranche/renomme) ne doit pas empecher le
// service de demarrer.
unsigned int pickAudioOutputDevice(RtAudio& dac, const std::string& preferredSubstring = "") {
  std::vector<unsigned int> ids = dac.getDeviceIds();
  std::printf("Peripheriques audio disponibles:\n");
  int chosen = -1;
  int preferredMatch = -1;
  for (unsigned int id : ids) {
    RtAudio::DeviceInfo info = dac.getDeviceInfo(id);
    if (info.outputChannels == 0) continue;  // entree seule (micro...)
    std::printf("  [%u] %s\n", id, info.name.c_str());
    if (!preferredSubstring.empty() && preferredMatch < 0 &&
        info.name.find(preferredSubstring) != std::string::npos) {
      preferredMatch = static_cast<int>(id);
    }
    const bool isBuiltIn = info.name.find("bcm2835") != std::string::npos ||
                            info.name.find("vc4hdmi") != std::string::npos ||
                            info.name.find("vc4-hdmi") != std::string::npos ||
                            info.name == "Default ALSA Device";
    if (!isBuiltIn && chosen < 0) {
      chosen = static_cast<int>(id);
    }
  }
  if (!preferredSubstring.empty()) {
    if (preferredMatch >= 0) return static_cast<unsigned int>(preferredMatch);
    std::fprintf(stderr,
                 "--audio-device: aucun peripherique ne contient \"%s\", repli sur la selection "
                 "automatique.\n",
                 preferredSubstring.c_str());
  }
  if (chosen >= 0) return static_cast<unsigned int>(chosen);
  // Rien d'externe trouve : on retombe sur le device par defaut ALSA (donc
  // la sortie embarquee, dans la plupart des cas sur un Pi).
  return dac.getDefaultOutputDevice();
}

}  // namespace

int main(int argc, char** argv) {
  // stdout n'est pas un TTY sous systemd (redirige vers un pipe/journald) :
  // glibc bufferise alors par blocs de 4 Ko au lieu de ligne par ligne, donc
  // les printf restent coincees en memoire jusqu'a ce que le process
  // s'arrete (flush a la sortie normale de main()) au lieu d'apparaitre en
  // temps reel dans `journalctl -f`. Force le mode ligne par ligne dans tous
  // les cas.
  std::setvbuf(stdout, nullptr, _IOLBF, 0);

  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);

  double sampleRate = 44100.0;
  // Pi 3B+ : commencez a 256, ne descendez a 128 que si aucun xrun n'apparait
  // (cf README, section reglages specifiques Pi 3B+).
  unsigned int bufferFrames = 256;

  // Valeurs par defaut du moteur (rosic::MipMappedWaveTable) : reprises ici
  // telles quelles pour que ne rien passer sur ces flags soit un no-op.
  double squarePhaseShift = 180.0;
  double tanhShaperDrive  = 36.9;
  double tanhShaperOffset = 4.37;

  bool listDevices = false;
  std::string audioDeviceFilter;
  bool listMidiPorts = false;
  bool showMeter = false;
  std::string meterFile;
  std::string midiPortFilter;
  MidiSource midiSource = MidiSource::Auto;

  // Args positionnels (sampleRate, bufferFrames) et flags (--xxx) sont
  // distingues explicitement : sans ca, "open303_pi_host --sine" envoyait
  // "--sine" dans std::atof, qui renvoie silencieusement 0.0.
  int positionalIndex = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      printUsage(argv[0]);
      return 0;
    } else if (std::strcmp(argv[i], "--sine") == 0) {
      gSineTestMode = true;
      std::printf("Mode diagnostic: sinus 440 Hz (bypass Open303)\n");
    } else if (std::strcmp(argv[i], "--rt") == 0) {
      gEnableRealtime = true;
      std::printf("Scheduling temps reel ACTIVE (--rt)\n");
    } else if (std::strcmp(argv[i], "--channel") == 0) {
      if (++i >= argc) {
        std::fprintf(stderr, "--channel attend un numero de canal (0-15)\n");
        return 1;
      }
      const int channel = std::atoi(argv[i]);
      if (channel < 0 || channel > 15) {
        std::fprintf(stderr, "Canal MIDI invalide: %s (attendu: 0-15)\n", argv[i]);
        return 1;
      }
      gMidiChannel = channel;
      std::printf("Canal MIDI filtre: %d\n", gMidiChannel);
    } else if (std::strcmp(argv[i], "--audio-device") == 0) {
      if (++i >= argc) {
        std::fprintf(stderr, "--audio-device attend une sous-chaine du nom du peripherique\n");
        return 1;
      }
      audioDeviceFilter = argv[i];
    } else if (std::strcmp(argv[i], "--list-devices") == 0) {
      listDevices = true;
    } else if (std::strcmp(argv[i], "--midi-port") == 0) {
      if (++i >= argc) {
        std::fprintf(stderr, "--midi-port attend une sous-chaine du nom du port\n");
        return 1;
      }
      midiPortFilter = argv[i];
    } else if (std::strcmp(argv[i], "--midi-source") == 0) {
      if (++i >= argc) {
        std::fprintf(stderr, "--midi-source attend: auto | usb | network\n");
        return 1;
      }
      if (std::strcmp(argv[i], "auto") == 0) {
        midiSource = MidiSource::Auto;
      } else if (std::strcmp(argv[i], "usb") == 0) {
        midiSource = MidiSource::Usb;
      } else if (std::strcmp(argv[i], "network") == 0) {
        midiSource = MidiSource::Network;
      } else {
        std::fprintf(stderr, "--midi-source invalide: %s (attendu: auto|usb|network)\n", argv[i]);
        return 1;
      }
    } else if (std::strcmp(argv[i], "--list-midi-ports") == 0) {
      listMidiPorts = true;
    } else if (std::strcmp(argv[i], "--meter") == 0) {
      showMeter = true;
    } else if (std::strcmp(argv[i], "--meter-file") == 0) {
      if (++i >= argc) {
        std::fprintf(stderr, "--meter-file attend un chemin\n");
        return 1;
      }
      meterFile = argv[i];
    } else if (std::strcmp(argv[i], "--square-phase") == 0) {
      if (++i >= argc) {
        std::fprintf(stderr, "--square-phase attend une valeur en degres\n");
        return 1;
      }
      squarePhaseShift = std::atof(argv[i]);
    } else if (std::strcmp(argv[i], "--tanh-drive") == 0) {
      if (++i >= argc) {
        std::fprintf(stderr, "--tanh-drive attend une valeur en dB\n");
        return 1;
      }
      tanhShaperDrive = std::atof(argv[i]);
    } else if (std::strcmp(argv[i], "--tanh-offset") == 0) {
      if (++i >= argc) {
        std::fprintf(stderr, "--tanh-offset attend une valeur\n");
        return 1;
      }
      tanhShaperOffset = std::atof(argv[i]);
    } else if (positionalIndex == 0) {
      sampleRate = std::atof(argv[i]);
      if (sampleRate <= 0.0) {
        std::fprintf(stderr, "Sample rate invalide: %s\n", argv[i]);
        return 1;
      }
      positionalIndex = 1;
    } else if (positionalIndex == 1) {
      int frames = std::atoi(argv[i]);
      if (frames <= 0) {
        std::fprintf(stderr, "Taille de buffer invalide: %s\n", argv[i]);
        return 1;
      }
      bufferFrames = static_cast<unsigned int>(frames);
      positionalIndex = 2;
    } else {
      std::fprintf(stderr, "Argument inattendu: %s\n", argv[i]);
      return 1;
    }
  }

  // --- Mode diagnostic : liste les peripheriques audio et quitte ---
  // (reutilise pickAudioOutputDevice() pour son effet d'affichage ; sert a
  // peupler un menu de selection -- ex: une interface web -- avec exactement
  // les noms que verrait le vrai lancement du service.)
  if (listDevices) {
    RtAudio dac;
    if (dac.getDeviceCount() == 0) {
      std::fprintf(stderr, "Aucune carte audio detectee.\n");
      return 1;
    }
    pickAudioOutputDevice(dac);
    return 0;
  }

  // --- Mode diagnostic : liste les ports MIDI et quitte ---
  // Meme principe que --list-devices : les noms affiches sont exactement
  // ceux que verrait le vrai lancement (y compris un port rtpmidid une fois
  // qu'une session reseau est etablie -- cf README section MIDI sans fil).
  if (listMidiPorts) {
    RtMidiIn midiinList;
    pickMidiPort(midiinList);
    return 0;
  }

  // --- Init du moteur DSP ---
  // (avant l'ouverture du flux audio : aucun autre thread ne touche encore
  // gSynth a ce stade, donc pas de synchronisation necessaire)
  //
  // NB: setEnvMod, setResonance et setAccent attendent des POURCENTS (0..100),
  // pas une valeur normalisee 0..1 (cf rosic_Open303.cpp).
  gSampleRate = sampleRate;
  gSynth.setSampleRate(sampleRate);
  gSynth.setTuning(440.0);
  gSynth.setCutoff(1000.0);
  gSynth.setResonance(50.0);   // 50 %
  gSynth.setEnvMod(50.0);      // 50 %
  gSynth.setDecay(600.0);      // ms
  gSynth.setAccent(50.0);      // 50 %
  gSynth.setVolume(-6.0);      // dB
  gSynth.setWaveform(0.0);     // saw

  // Parametres "back-panel" qui regenerent la wavetable mip-mappee a chaque
  // appel (FFT) : appliques ici une seule fois, avant l'ouverture du flux
  // audio, jamais depuis le thread audio (cf --help et applyMidiEvent()).
  gSynth.setSquarePhaseShift(squarePhaseShift);
  gSynth.setTanhShaperDrive(tanhShaperDrive);
  gSynth.setTanhShaperOffset(tanhShaperOffset);

  // --- Ouverture du port MIDI USB ---
  RtMidiIn midiin;
  int portIndex = pickMidiPort(midiin, midiPortFilter, midiSource);
  if (portIndex < 0) return 1;
  midiin.openPort(portIndex);
  midiin.setCallback(&handleMidiMessage);
  midiin.ignoreTypes(true, true, true);  // ignore sysex/timing/sensing

  std::printf("MIDI ouvert sur: %s\n", midiin.getPortName(portIndex).c_str());

  // --- Ouverture du flux audio (ALSA sur Raspberry Pi) ---
  RtAudio dac;
  if (dac.getDeviceCount() == 0) {
    std::fprintf(stderr, "Aucune carte audio detectee.\n");
    return 1;
  }

  RtAudio::StreamParameters outParams;
  outParams.deviceId = pickAudioOutputDevice(dac, audioDeviceFilter);
  outParams.nChannels = 2;
  std::printf("Sortie audio: %s\n", dac.getDeviceInfo(outParams.deviceId).name.c_str());

  RtAudio::StreamOptions options;
  // Le scheduling temps reel est OPT-IN (--rt), pas active par defaut :
  // sur la sortie jack embarquee de certains Pi (driver bcm2835), une
  // priorite RT trop elevee sur le thread audio peut perturber le thread
  // noyau qui gere les interruptions du DMA audio et provoquer des
  // craquements periodiques, sans que ca remonte comme un xrun ALSA
  // classique. Testez avec/sans selon votre materiel (cf README, section
  // Depannage). Avec une carte son USB, --rt est generalement sans souci
  // et permet de descendre a un buffer plus petit.
  if (gEnableRealtime.load()) {
    options.flags = RTAUDIO_SCHEDULE_REALTIME | RTAUDIO_MINIMIZE_LATENCY;
    options.priority = 90;
  }

  // RtAudio 6 ne leve plus d'exception sur echec d'ouverture/demarrage du
  // flux : openStream/startStream renvoient un RtAudioErrorType. Sans ce
  // controle explicite, un echec passe inapercu ("Open303 pret" s'affiche
  // quand meme, puis silence).
  if (dac.openStream(&outParams, nullptr, RTAUDIO_FLOAT32, static_cast<unsigned int>(sampleRate),
                      &bufferFrames, &audioCallback, nullptr, &options) != RTAUDIO_NO_ERROR) {
    std::fprintf(stderr, "Erreur audio (ouverture): %s\n", dac.getErrorText().c_str());
    return 1;
  }
  if (dac.startStream() != RTAUDIO_NO_ERROR) {
    std::fprintf(stderr, "Erreur audio (demarrage): %s\n", dac.getErrorText().c_str());
    return 1;
  }

  std::printf("Open303 pret. Buffer=%u frames @ %.0f Hz (latence ~%.1f ms). Ctrl+C pour quitter.\n",
              bufferFrames, sampleRate, 1000.0 * bufferFrames / sampleRate);

  // --- Surveillance de la topologie MIDI ALSA ---
  // rtpmidid cree un NOUVEAU port ALSA a chaque connexion/decouverte reseau
  // et ne nettoie pas toujours les anciens. Un abonnement pris une seule fois
  // au demarrage devient donc silencieusement orphelin des que la session
  // reseau bouge : plus aucune note recue, sans le moindre message d'erreur
  // (constate en pratique apres chaque reconnexion Wi-Fi -- il fallait
  // relancer le service a la main). On resonde donc periodiquement la liste
  // des ports et on se rebranche des que le port retenu change.
  //
  // Effet de bord bienvenu : gere aussi le hot-plug d'un controleur USB, qui
  // ne dependait jusqu'ici que de la regle udev redemarrant tout le service.
  std::string currentPortName = midiin.getPortName(portIndex);
  int currentPortIndex = portIndex;
  int pollTicks = 0;
  // 2 x 200 ms = ~400 ms. Descendu depuis 2 s pour qu'une session reseau qui
  // vient de s'ouvrir soit prise en compte quasi immediatement (les outils de
  // test n'ont plus besoin d'attendre plusieurs secondes avant d'emettre).
  // Cout mesure sur le Pi 3B+ : indiscernable du bruit de fond (l'enumeration
  // ALSA est une poignee d'ioctl, et elle a lieu dans la boucle principale,
  // jamais dans le thread audio).
  constexpr int kPollEveryTicks = 2;

  unsigned lastDropped = 0;
  while (gKeepRunning) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    // Rapport hors thread temps reel : si ca monte, c'est que la file MIDI
    // deborde (bloc audio trop long, ou rafale de CC tres dense).
    const unsigned dropped = gMidiDropped.load(std::memory_order_relaxed);
    if (dropped != lastDropped) {
      std::fprintf(stderr, "[midi] %u evenement(s) MIDI perdu(s) (file pleine)\n", dropped);
      lastDropped = dropped;
    }

    if (showMeter || !meterFile.empty()) {
      // Remise a zero atomique : on lit ET on reinitialise, pour que chaque
      // releve porte sur les 200 dernieres ms seulement.
      //
      // IMPORTANT : tout ce bloc s'execute dans la boucle principale, JAMAIS
      // dans le callback audio. Le thread temps reel se contente d'un max de
      // valeurs absolues + un CAS atomique (deja deploye et mesure sans le
      // moindre xrun) ; l'ecriture fichier et le formatage restent ici.
      const float peak = gPeakLevel.exchange(0.0f, std::memory_order_relaxed);

      if (!meterFile.empty()) {
        // Destination attendue : /run (tmpfs, donc en RAM -- aucune usure de
        // la carte SD malgre une ecriture toutes les 200 ms). Lu par
        // l'interface web, qui evite ainsi de lancer un `journalctl` en
        // boucle : sur un Pi 3B+, un sous-processus 2 fois par seconde
        // volerait du CPU au thread audio.
        if (FILE* f = std::fopen(meterFile.c_str(), "w")) {
          std::fprintf(f, "%.6f\n", peak);
          std::fclose(f);
        }
      }

      if (showMeter) {
        if (peak > 0.0f) {
          std::printf("[meter] crete %.4f (%.1f dBFS)\n", peak, 20.0 * std::log10(peak));
        } else {
          std::printf("[meter] silence (0.0000)\n");
        }
      }
    }

    if (++pollTicks < kPollEveryTicks) continue;
    pollTicks = 0;

    const int desired = pickMidiPort(midiin, midiPortFilter, midiSource, /*verbose=*/false);
    if (desired < 0) continue;  // plus aucun port : on garde l'abonnement actuel
    const std::string desiredName = midiin.getPortName(static_cast<unsigned int>(desired));
    if (desired == currentPortIndex && desiredName == currentPortName) continue;

    std::printf("[midi] topologie ALSA modifiee : rebranchement sur \"%s\" (index %d) "
                "-- etait \"%s\" (index %d)\n",
                desiredName.c_str(), desired, currentPortName.c_str(), currentPortIndex);

    midiin.closePort();
    // Le thread de callback RtMidi est arrete par closePort() : nous sommes
    // ici momentanement le SEUL producteur de la file, on peut donc y pousser
    // sans casser l'hypothese SPSC du ring buffer.
    MidiEvent panic;
    panic.status = 0xB0;
    panic.data1  = 123;  // All Notes Off : evite une note bloquee (cf case 123)
    panic.data2  = 0;
    pushMidiEvent(panic);
    midiin.openPort(static_cast<unsigned int>(desired));
    // Ni setCallback() ni ignoreTypes() a refaire : RtMidi les stocke sur
    // l'objet et non sur le port, ils survivent au closePort/openPort (les
    // rappeler declencherait meme un avertissement "callback already set").
    currentPortIndex = desired;
    currentPortName  = desiredName;
  }

  dac.stopStream();
  if (dac.isStreamOpen()) dac.closeStream();
  midiin.closePort();
  return 0;
}
