// Mesure objective de l'effet de CHAQUE CC sur le signal reel du moteur.
//
// Motivation : juger un parametre a l'oreille est trompeur. Sur l'overdrive,
// trois reglages successifs ont sonne "pareil" alors que les mesures
// theoriques promettaient une evolution -- il a fallu mesurer le VRAI signal
// pour comprendre (cf tools/overdrive_probe.cpp). Ce banc applique la meme
// discipline a tout le mapping : pour chaque CC, il joue une note avec le CC
// au minimum puis au maximum, et rapporte ce qui change.
//
// Indicateurs (cf discussion crete/RMS/harmoniques) :
//   crete     -> ecretage eventuel
//   RMS       -> volume/densite percus
//   centroide -> brillance (barycentre du spectre, en rang d'harmonique)
//   H2/H1     -> caractere "pair" (chaleur, asymetrie)
//   H3/H1     -> caractere "impair" (creux, carre)
//
// Compilation (depuis la racine du depot) :
//   g++ -O2 -std=c++17 -include cstring -include climits \
//       -I third_party/Open303/Source/DSPCode \
//       tools/cc_impact.cpp third_party/Open303/Source/DSPCode/*.cpp \
//       -o /tmp/cc_impact
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "rosic_Open303.h"

namespace {

constexpr double kSampleRate = 44100.0;
constexpr int    kNote       = 36;   // C2

// --- overdrive : copie de src/main.cpp (garder en phase) -------------------
struct Drive { bool active; float gain, bias, tanhBias, norm, toneCoeff, mix; };
float gToneState = 0.0f;

Drive makeDrive(float amount, float tone, float mix) {
  Drive d{};
  d.active = amount > 0.0f;
  if (!d.active) return d;
  d.gain     = 1.0f + 5.5f * amount * amount;
  d.bias     = 0.8f * amount * amount;
  d.tanhBias = std::tanh(d.bias);
  d.norm = 1.0f / std::fmax(std::fabs(std::tanh(d.gain + d.bias) - d.tanhBias),
                            std::fabs(std::tanh(-d.gain + d.bias) - d.tanhBias));
  d.toneCoeff = (tone < 1.0f) ? (0.05f + 0.95f * tone) : 1.0f;
  d.mix = mix;
  return d;
}

float shape(float x, const Drive& d) {
  if (!d.active) return x;
  float y = (std::tanh(x * d.gain + d.bias) - d.tanhBias) * d.norm;
  if (d.toneCoeff < 1.0f) { gToneState += d.toneCoeff * (y - gToneState); y = gToneState; }
  return x + d.mix * (y - x);
}
// --------------------------------------------------------------------------

struct Metrics { double peak, rms, centroid, h2, h3; };

double harm(const std::vector<float>& x, double f) {
  double re = 0, im = 0;
  const double w = 2.0 * M_PI * f / kSampleRate;
  for (size_t n = 0; n < x.size(); ++n) { re += x[n] * std::cos(w * n); im += x[n] * std::sin(w * n); }
  return 2.0 * std::hypot(re, im) / static_cast<double>(x.size());
}

// Reglages de base : un son "acid" ordinaire, pour que chaque CC soit teste
// dans un contexte musical realiste plutot qu'aux extremes.
void baseline(rosic::Open303& s) {
  s.setSampleRate(kSampleRate);
  s.setTuning(440.0);   s.setCutoff(700.0);  s.setResonance(60.0);
  s.setEnvMod(60.0);    s.setDecay(600.0);   s.setAccent(50.0);
  s.setVolume(-6.0);    s.setWaveform(0.0);
}

// Applique un CC comme le fait applyMidiEvent() dans src/main.cpp.
void applyCC(rosic::Open303& s, int cc, double v01, float& drv, float& tone, float& mix) {
  switch (cc) {
    case 74: s.setCutoff(313.8152786059267 * std::pow(2394.411986817546 / 313.8152786059267, v01)); break;
    case 71: s.setResonance(100.0 * v01); break;
    case 73: s.setDecay(200.0 + 1800.0 * v01); break;
    case  1: s.setEnvMod(100.0 * v01); break;
    case  7: s.setVolume(-40.0 + 40.0 * v01); break;
    case 70: s.setWaveform(v01); break;
    case 20: s.setNormalAttack(0.3 + 29.7 * v01); break;
    case 21: s.setAccentAttack(0.3 + 29.7 * v01); break;
    case 22: s.setPreFilterHighpass(500.0 * v01); break;
    case 23: s.setFeedbackHighpass(500.0 * v01); break;
    case 24: s.setPostFilterHighpass(500.0 * v01); break;
    case 25: s.setAmpSustain(-60.0 + 60.0 * v01); break;
    case 26: s.setAmpDecay(16.0 + 2984.0 * v01); break;
    case 27: s.setAmpRelease(1.0 + 499.0 * v01); break;
    case 28: s.setTuning(415.0 + 51.0 * v01); break;
    case 29: s.setAccent(100.0 * v01); break;
    case 30: drv  = static_cast<float>(v01); break;
    case 31: tone = static_cast<float>(v01); break;
    case 32: mix  = static_cast<float>(v01); break;
    default: break;
  }
}

// Fenetre d'observation. Un parametre ne peut se mesurer que la ou il agit :
// mesurer l'attaque apres 50 ms ou le release note tenue donne un faux
// "aucun effet" -- erreur commise a la premiere version de ce banc.
enum class Window {
  Attack,   // 0-30 ms   : attaques d'enveloppe de filtre (0.3-30 ms)
  Body,     // 50-350 ms : timbre general, cas par defaut
  Late,     // 900-1200 ms : sustain, audible seulement APRES le decay
  Release,  // apres note-off a 300 ms : queue de l'enveloppe d'ampli
};

Metrics measure(int cc, double v01, int velocity, Window win) {
  rosic::Open303 s;
  baseline(s);
  float drv = 0.0f, tone = 1.0f, mix = 1.0f;
  // Les reglages de l'overdrive (tone, mix) n'ont evidemment aucun effet si
  // le drive est nul : on l'engage pour pouvoir les tester.
  if (cc == 31 || cc == 32) drv = 0.8f;
  applyCC(s, cc, v01, drv, tone, mix);
  gToneState = 0.0f;
  const Drive d = makeDrive(drv, tone, mix);

  s.noteOn(kNote, velocity, 0.0);

  double skipSec = 0.05, lenSec = 0.30;
  switch (win) {
    case Window::Attack:  skipSec = 0.000; lenSec = 0.030; break;
    case Window::Body:    skipSec = 0.050; lenSec = 0.300; break;
    case Window::Late:    skipSec = 0.900; lenSec = 0.300; break;
    case Window::Release: skipSec = 0.300; lenSec = 0.300; break;
  }

  const int skip = static_cast<int>(skipSec * kSampleRate);
  const int len  = static_cast<int>(lenSec * kSampleRate);
  for (int i = 0; i < skip; ++i) {
    // Pour le release : on relache la note a 300 ms, puis on observe la queue.
    if (win == Window::Release && i == static_cast<int>(0.300 * kSampleRate) - 1) {
      s.noteOn(kNote, 0, 0.0);
    }
    shape((float)s.getSample(), d);
  }
  if (win == Window::Release) s.noteOn(kNote, 0, 0.0);

  std::vector<float> buf(len);
  double sq = 0; float peak = 0;
  for (int i = 0; i < len; ++i) {
    const float y = shape((float)s.getSample(), d);
    buf[i] = y; peak = std::fmax(peak, std::fabs(y)); sq += (double)y * y;
  }

  const double f0 = 440.0 * std::pow(2.0, (kNote - 69) / 12.0);
  double num = 0, den = 0;
  for (int n = 1; n <= 20; ++n) { const double a = harm(buf, n * f0); num += n * a; den += a; }
  const double h1 = harm(buf, f0);
  return { peak, std::sqrt(sq / len), den > 0 ? num / den : 0.0,
           h1 > 0 ? harm(buf, 2 * f0) / h1 : 0.0,
           h1 > 0 ? harm(buf, 3 * f0) / h1 : 0.0 };
}

struct Entry { int cc; const char* name; int velocity; Window win; };

}  // namespace

int main() {
  // Velocite 110 (>= 100) pour les parametres qui ne concernent QUE les notes
  // accentuees : les tester a velocite normale donnerait un faux "aucun effet".
  const std::vector<Entry> ccs = {
    {74, "cutoff",              80, Window::Body},
    {71, "resonance",           80, Window::Body},
    { 1, "env mod",             80, Window::Body},
    {73, "filter decay",        80, Window::Body},
    {70, "waveform",            80, Window::Body},
    { 7, "volume",              80, Window::Body},
    {28, "tuning",              80, Window::Body},
    {29, "accent amount",      110, Window::Body},
    {20, "filter attack",       80, Window::Attack},
    {21, "filter attack (acc)",110, Window::Attack},
    {22, "highpass pre",        80, Window::Body},
    {23, "highpass feedback",   80, Window::Body},
    {24, "highpass post",       80, Window::Body},
    {25, "amp sustain",         80, Window::Late},
    {26, "amp decay",           80, Window::Late},
    {27, "amp release",         80, Window::Release},
    {30, "overdrive drive",     80, Window::Body},
    {31, "overdrive tone",      80, Window::Body},
    {32, "overdrive mix",       80, Window::Body},
  };
  const char* winName[] = {"attaque", "corps", "tardif", "release"};

  std::printf("Effet de chaque CC sur le signal reel du moteur (note %d)\n", kNote);
  std::printf("Chaque parametre est mesure dans la fenetre ou il agit reellement.\n\n");
  std::printf(" CC  parametre             fenetre | dRMS    dCentroide  dH2/H1  dH3/H1 | verdict\n");
  std::printf("---------------------------------------------------------------------------------\n");

  for (const auto& e : ccs) {
    const Metrics lo = measure(e.cc, 0.0, e.velocity, e.win);
    const Metrics hi = measure(e.cc, 1.0, e.velocity, e.win);

    const double dR = hi.rms - lo.rms;
    const double dC = hi.centroid - lo.centroid;
    const double d2 = hi.h2 - lo.h2;
    const double d3 = hi.h3 - lo.h3;

    // Seuils empiriques : en deca, la difference ne s'entend pas.
    const bool audible = std::fabs(dR) > 0.02 || std::fabs(dC) > 0.3 ||
                         std::fabs(d2) > 0.05 || std::fabs(d3) > 0.05;
    std::printf(" %3d  %-20s %-7s | %+7.3f  %+9.2f  %+6.3f  %+6.3f | %s\n",
                e.cc, e.name, winName[static_cast<int>(e.win)],
                dR, dC, d2, d3, audible ? "net" : "TRES FAIBLE");
  }
  return 0;
}
