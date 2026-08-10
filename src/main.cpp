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

#include "RtAudio.h"
#include "RtMidi.h"
#include "rosic_Open303.h"   // Source/DSPCode/rosic_Open303.h dans le repo Open303

namespace {

rosic::Open303 gSynth;
std::atomic<bool> gKeepRunning{true};
std::atomic<bool> gSineTestMode{false};
std::atomic<bool> gEnableRealtime{false};
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
                  // Defaut moteur : 150 Hz.
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
void handleMidiMessage(double /*deltatime*/, std::vector<unsigned char>* message, void* /*userData*/) {
  if (!message || message->size() < 2) return;
  const auto& msg = *message;

  // Filtrage de canal (gMidiChannel = -1 : tous les canaux).
  if (gMidiChannel >= 0 && (msg[0] & 0x0F) != static_cast<unsigned char>(gMidiChannel)) return;

  MidiEvent ev;
  ev.status = msg[0];
  ev.data1  = msg[1];
  ev.data2  = (msg.size() > 2) ? msg[2] : 0;
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

  for (unsigned int i = 0; i < nFrames; ++i) {
    float s = static_cast<float>(gSynth.getSample());
    out[2 * i + 0] = s;  // gauche
    out[2 * i + 1] = s;  // droite (mono duplique)
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
      "  -h, --help       Affiche cette aide et quitte.\n"
      "\n"
      "Exemples :\n"
      "  %s\n"
      "  %s 44100 128 --rt\n"
      "  %s 48000 256 --channel 0\n",
      progName, progName, progName, progName);
}

int pickMidiPort(RtMidiIn& midiin) {
  unsigned int nPorts = midiin.getPortCount();
  if (nPorts == 0) {
    std::fprintf(stderr, "Aucun port MIDI trouve. Branchez votre interface USB MIDI.\n");
    return -1;
  }
  std::printf("Ports MIDI disponibles:\n");
  for (unsigned int i = 0; i < nPorts; ++i) {
    std::printf("  [%u] %s\n", i, midiin.getPortName(i).c_str());
  }
  // "Midi Through" est un port virtuel cree par le noyau ALSA, toujours
  // present en position 0, et ne correspond a aucun materiel branche. Le
  // prendre par defaut (comme le faisait un simple "return 0") revient a
  // ignorer silencieusement le vrai controleur USB des qu'il en existe un.
  // On prend donc le premier port qui n'est PAS "Midi Through".
  for (unsigned int i = 0; i < nPorts; ++i) {
    if (midiin.getPortName(i).find("Midi Through") == std::string::npos) {
      return static_cast<int>(i);
    }
  }
  // Rien d'autre que des ports "Midi Through" : on s'y connecte quand meme,
  // au cas ou un logiciel MIDI local y enverrait des evenements via ALSA.
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
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
  int portIndex = pickMidiPort(midiin);
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
  outParams.deviceId = dac.getDefaultOutputDevice();
  outParams.nChannels = 2;

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
  }

  dac.stopStream();
  if (dac.isStreamOpen()) dac.closeStream();
  midiin.closePort();
  return 0;
}
