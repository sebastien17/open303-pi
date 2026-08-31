// Banc d'essai de l'overdrive, sur le SIGNAL REEL du moteur Open303.
//
// Motivation : les premiers reglages ont ete faits sur une sinusoide
// theorique, et trois iterations d'affilee ont sonne "pareil en haut de
// course" a l'oreille malgre des mesures qui promettaient une evolution. La
// sinusoide n'est pas representative : la sortie du 303 est une dent de scie
// filtree par un ladder resonant, avec une enveloppe qui decroit -- donc un
// niveau d'entree tres variable, alors que la saturation depend justement du
// niveau.
//
// Ce programme joue une vraie note, applique l'overdrive a differents
// reglages, et rapporte pour chacun : crete, RMS, et rapports harmoniques
// mesures sur la fondamentale de la note. Aucun materiel audio requis.
//
// Compilation (depuis la racine du depot) :
//   g++ -O2 -std=c++17 -include cstring -include climits \
//       -I third_party/Open303/Source/DSPCode \
//       tools/overdrive_probe.cpp third_party/Open303/Source/DSPCode/*.cpp \
//       third_party/Open303/Source/DSPCode/*.c -o /tmp/overdrive_probe
#include <cmath>
#include <cstdio>
#include <vector>

#include "rosic_Open303.h"

namespace {

constexpr double kSampleRate = 44100.0;

// --- Copie EXACTE de la logique de src/main.cpp (garder les deux en phase) ---
struct DriveCoeffs {
  bool  active;
  float gain, bias, tanhBias, norm, toneCoeff, mix;
};

DriveCoeffs computeDriveCoeffs(float amount, float tone, float mix) {
  DriveCoeffs d{};
  d.active = amount > 0.0f;
  if (!d.active) return d;
  d.gain     = 1.0f + 11.0f * amount * amount;
  d.bias     = 0.8f * amount * amount;
  d.tanhBias = std::tanh(d.bias);
  const float peakPos = std::fabs(std::tanh(d.gain + d.bias) - d.tanhBias);
  const float peakNeg = std::fabs(std::tanh(-d.gain + d.bias) - d.tanhBias);
  d.norm      = 1.0f / std::fmax(peakPos, peakNeg);
  d.toneCoeff = (tone < 1.0f) ? (0.05f + 0.95f * tone) : 1.0f;
  d.mix       = mix;
  return d;
}

float gToneState = 0.0f;

float applyOverdrive(float x, const DriveCoeffs& d) {
  if (!d.active) return x;
  float y = (std::tanh(x * d.gain + d.bias) - d.tanhBias) * d.norm;
  if (d.toneCoeff < 1.0f) {
    gToneState += d.toneCoeff * (y - gToneState);
    y = gToneState;
  }
  return x + d.mix * (y - x);
}
// --- fin de la copie ---

// Amplitude d'une harmonique par correlation directe (Goertzel simplifie).
double harmonicAmplitude(const std::vector<float>& x, double freq) {
  double re = 0.0, im = 0.0;
  const double w = 2.0 * M_PI * freq / kSampleRate;
  for (size_t n = 0; n < x.size(); ++n) {
    re += x[n] * std::cos(w * n);
    im += x[n] * std::sin(w * n);
  }
  return 2.0 * std::hypot(re, im) / static_cast<double>(x.size());
}

}  // namespace

int main() {
  const int note = 36;                       // C2, comme dans les tests d'ecoute
  const double f0 = 440.0 * std::pow(2.0, (note - 69) / 12.0);

  std::printf("Signal reel Open303, note %d (%.1f Hz), reglages acid typiques\n\n", note, f0);
  std::printf(" drive |  crete    RMS   | H2/H1  H3/H1  H4/H1  H5/H1\n");
  std::printf("-------|-----------------|----------------------------\n");

  for (int ccDrive : {0, 32, 50, 64, 90, 108, 127}) {
    rosic::Open303 synth;
    synth.setSampleRate(kSampleRate);
    synth.setTuning(440.0);
    synth.setCutoff(700.0);
    synth.setResonance(80.0);
    synth.setEnvMod(75.0);
    synth.setDecay(600.0);
    synth.setAccent(50.0);
    synth.setVolume(-6.0);
    synth.setWaveform(0.0);

    gToneState = 0.0f;
    const DriveCoeffs d = computeDriveCoeffs(ccDrive / 127.0f, 1.0f, 1.0f);

    synth.noteOn(note, 80, 0.0);

    // On analyse une fenetre APRES l'attaque : c'est la partie tenue de la
    // note, celle qu'on entend le plus longtemps.
    const int skip = static_cast<int>(0.05 * kSampleRate);
    const int len  = static_cast<int>(0.30 * kSampleRate);
    for (int i = 0; i < skip; ++i) applyOverdrive((float)synth.getSample(), d);

    std::vector<float> buf(len);
    float peak = 0.0f;
    double sumSq = 0.0;
    for (int i = 0; i < len; ++i) {
      const float s = applyOverdrive((float)synth.getSample(), d);
      buf[i] = s;
      peak = std::fmax(peak, std::fabs(s));
      sumSq += static_cast<double>(s) * s;
    }
    synth.noteOn(note, 0, 0.0);

    const double h1 = harmonicAmplitude(buf, f0);
    const double h2 = harmonicAmplitude(buf, 2 * f0);
    const double h3 = harmonicAmplitude(buf, 3 * f0);
    const double h4 = harmonicAmplitude(buf, 4 * f0);
    const double h5 = harmonicAmplitude(buf, 5 * f0);
    const double rms = std::sqrt(sumSq / len);

    std::printf("  %3d  | %6.3f  %6.3f  | %.3f  %.3f  %.3f  %.3f\n",
                ccDrive, peak, rms,
                h1 > 0 ? h2 / h1 : 0.0, h1 > 0 ? h3 / h1 : 0.0,
                h1 > 0 ? h4 / h1 : 0.0, h1 > 0 ? h5 / h1 : 0.0);
  }
  return 0;
}
