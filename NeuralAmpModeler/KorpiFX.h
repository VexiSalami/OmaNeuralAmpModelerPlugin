// KorpiFX.h
// Korpi-pluginin omat efektit:
//   Salo  = stereokaiku kolmella tyypillä (Hall, Plate, Shimmer)
//   Metso = viive (mono)
//
// Hall perustuu Freeverb-algoritmiin (Jezar at Dreampoint, public domain).
// Plate ja Shimmer perustuvat Jon Dattorron julkaisemaan levykaikurakenteeseen
// ("Effect Design, Part 1", J. Audio Eng. Soc., 1997).

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace korpi
{

constexpr double kPi = 3.14159265358979323846;

// Tasainen siirtymä nupin arvosta toiseen, ettei nupin kääntäminen rätise.
class Smoother
{
public:
  void Reset(double sampleRate, double timeSeconds = 0.03)
  {
    mCoeff = std::exp(-1.0 / (timeSeconds * sampleRate));
  }
  void SetTarget(double target) { mTarget = target; }
  void Jump(double value) { mValue = mTarget = value; }
  double Next()
  {
    mValue = mTarget + mCoeff * (mValue - mTarget);
    return mValue;
  }

private:
  double mCoeff = 0.0;
  double mValue = 0.0;
  double mTarget = 0.0;
};

// ---------------------------------------------------------------------------
// Rakennuspalikat
// ---------------------------------------------------------------------------

// Viivelinja. Tap(0) = viimeisin kirjoitettu näyte.
class DelayLine
{
public:
  void Init(int maxDelay)
  {
    mBuffer.assign((size_t)std::max(2, maxDelay + 2), 0.0);
    mIndex = 0;
  }
  void Clear() { std::fill(mBuffer.begin(), mBuffer.end(), 0.0); }
  void Push(double x)
  {
    if (++mIndex >= mBuffer.size())
      mIndex = 0;
    mBuffer[mIndex] = x;
  }
  double Tap(int d) const
  {
    const size_t n = mBuffer.size();
    d = std::clamp(d, 0, (int)n - 1);
    return mBuffer[(mIndex + n - (size_t)d) % n];
  }
  double TapFrac(double d) const
  {
    const size_t n = mBuffer.size();
    d = std::clamp(d, 0.0, (double)n - 2.0);
    const int i = (int)d;
    const double f = d - i;
    return Tap(i) * (1.0 - f) + Tap(i + 1) * f;
  }

private:
  std::vector<double> mBuffer;
  size_t mIndex = 0;
};

// Kiinteä viive (D näytettä)
class FixedDelay
{
public:
  void Init(int delay)
  {
    mDelay = std::max(1, delay);
    mLine.Init(mDelay);
  }
  void Clear() { mLine.Clear(); }
  double Process(double x)
  {
    const double out = mLine.Tap(mDelay - 1);
    mLine.Push(x);
    return out;
  }
  double Tap(int d) const { return mLine.Tap(d); }
  // Seuraavaksi ulos tuleva näyte, ilman kirjoittamista
  double Peek() const { return mLine.Tap(mDelay - 1); }

private:
  DelayLine mLine;
  int mDelay = 1;
};

// Schroeder-allpass, valinnaisella modulaatiolla
class Allpass
{
public:
  void Init(int delay, int excursion = 0)
  {
    mDelay = std::max(1, delay);
    mExcursion = excursion;
    mLine.Init(mDelay + excursion + 2);
  }
  void Clear() { mLine.Clear(); }
  double Process(double x, double g, double mod = 0.0)
  {
    const double d = (mExcursion > 0) ? mLine.TapFrac(mDelay - 1 + mod * mExcursion) : mLine.Tap(mDelay - 1);
    const double v = x + g * d;
    mLine.Push(v);
    return -g * v + d;
  }
  double Tap(int d) const { return mLine.Tap(d); }

private:
  DelayLine mLine;
  int mDelay = 1;
  int mExcursion = 0;
};

// Yksinapainen alipäästö
struct OnePoleLP
{
  double z = 0.0;
  double Process(double x, double coeff) // coeff 0..1, isompi = tummempi
  {
    z = x * (1.0 - coeff) + z * coeff;
    return z;
  }
};

// Yksinapainen ylipäästö
struct OnePoleHP
{
  double coeff = 0.0;
  double prevIn = 0.0, prevOut = 0.0;
  void Set(double sampleRate, double freq) { coeff = std::exp(-2.0 * kPi * freq / sampleRate); }
  void Clear() { prevIn = prevOut = 0.0; }
  double Process(double x)
  {
    prevOut = coeff * (prevOut + x - prevIn);
    prevIn = x;
    return prevOut;
  }
};

// ---------------------------------------------------------------------------
// Hall: stereo-Freeverb, iso ja pehmeä
// ---------------------------------------------------------------------------
class HallReverb
{
public:
  void Init(double sampleRate)
  {
    const double scale = sampleRate / 44100.0;
    static const int combs[kNumCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int allpasses[kNumAllpasses] = {556, 441, 341, 225};
    const int spread = 23;
    for (int c = 0; c < 2; c++)
    {
      for (int i = 0; i < kNumCombs; i++)
      {
        // Hall: vähän pidemmät kammiot kuin alkuperäisessä Freeverbissä = isompi tila
        mCombs[c][i].Init((int)((combs[i] + c * spread) * scale * 1.25));
        mCombStore[c][i] = 0.0;
      }
      for (int i = 0; i < kNumAllpasses; i++)
        mAllpasses[c][i].Init((int)((allpasses[i] + c * spread) * scale));
    }
    mPreDelay.Init((int)(0.045 * sampleRate));
  }
  void Clear()
  {
    for (int c = 0; c < 2; c++)
    {
      for (int i = 0; i < kNumCombs; i++)
      {
        mCombs[c][i].Clear();
        mCombStore[c][i] = 0.0;
      }
      for (int i = 0; i < kNumAllpasses; i++)
        mAllpasses[c][i].Clear();
    }
    mPreDelay.Clear();
  }
  void Process(double in, double& outL, double& outR)
  {
    const double x = mPreDelay.Process(in) * 0.015;
    double out[2] = {0.0, 0.0};
    // Kammiosuodattimet (comb) vaimennetulla takaisinkytkennällä
    for (int c = 0; c < 2; c++)
    {
      for (int i = 0; i < kNumCombs; i++)
      {
        FixedDelay& d = mCombs[c][i];
        const double y = d.Peek();
        mCombStore[c][i] = y * (1.0 - kDamp) + mCombStore[c][i] * kDamp;
        d.Process(x + mCombStore[c][i] * kRoom);
        out[c] += y;
      }
      for (int i = 0; i < kNumAllpasses; i++)
        out[c] = mAllpasses[c][i].Process(out[c], 0.5);
    }
    outL = out[0];
    outR = out[1];
  }

private:
  static constexpr int kNumCombs = 8;
  static constexpr int kNumAllpasses = 4;
  static constexpr double kRoom = 0.93;
  static constexpr double kDamp = 0.42;

  FixedDelay mCombs[2][kNumCombs];
  double mCombStore[2][kNumCombs] = {};
  Allpass mAllpasses[2][kNumAllpasses];
  FixedDelay mPreDelay;
};

// ---------------------------------------------------------------------------
// Dattorron levykaiku (Plate). Shimmer käyttää samaa rakennetta.
// ---------------------------------------------------------------------------
class PlateReverb
{
public:
  struct Settings
  {
    double preDelaySeconds = 0.010;
    double bandwidth = 0.9995; // Sisääntulon kirkkaus (1 = kirkkain)
    double decay = 0.72; // Hännän pituus (0..~0.95)
    double damping = 0.12; // Hännän tummuminen (0 = kirkas)
  };

  void Init(double sampleRate, const Settings& settings)
  {
    mSettings = settings;
    const double k = sampleRate / 29761.0; // Dattorron alkuperäinen näytetaajuus
    auto n = [k](double samples) { return std::max(1, (int)std::lround(samples * k)); };
    mScale = k;
    mPreDelay.Init(std::max(1, (int)(settings.preDelaySeconds * sampleRate)));
    mInAP[0].Init(n(142));
    mInAP[1].Init(n(107));
    mInAP[2].Init(n(379));
    mInAP[3].Init(n(277));
    mModAP[0].Init(n(672), n(16));
    mModAP[1].Init(n(908), n(16));
    mDelayA[0].Init(n(4453));
    mDelayA[1].Init(n(4217));
    mDecayAP[0].Init(n(1800));
    mDecayAP[1].Init(n(2656));
    mDelayB[0].Init(n(3720));
    mDelayB[1].Init(n(3163));
    mLfoInc = 2.0 * kPi * 0.8 / sampleRate; // n. 0,8 Hz hidas huojunta
    Clear();
  }

  void Clear()
  {
    mPreDelay.Clear();
    for (auto& ap : mInAP)
      ap.Clear();
    for (int c = 0; c < 2; c++)
    {
      mModAP[c].Clear();
      mDelayA[c].Clear();
      mDecayAP[c].Clear();
      mDelayB[c].Clear();
      mDampLP[c].z = 0.0;
      mTankOut[c] = 0.0;
    }
    mBandLP.z = 0.0;
    mLfoPhase = 0.0;
  }

  void Process(double in, double& outL, double& outR)
  {
    const Settings& p = mSettings;
    // Sisääntulo: esiviive, kaistanrajaus ja diffuusio
    double x = mPreDelay.Process(in);
    x = mBandLP.Process(x, 1.0 - p.bandwidth);
    x = mInAP[0].Process(x, 0.75);
    x = mInAP[1].Process(x, 0.75);
    x = mInAP[2].Process(x, 0.625);
    x = mInAP[3].Process(x, 0.625);

    // Hidas huojunta tekee hännästä elävän eikä metallisen
    mLfoPhase += mLfoInc;
    if (mLfoPhase > 2.0 * kPi)
      mLfoPhase -= 2.0 * kPi;
    const double mod[2] = {std::sin(mLfoPhase), std::cos(mLfoPhase)};

    // Kaksi ristiinkytkettyä "tankkia"
    double newOut[2];
    for (int c = 0; c < 2; c++)
    {
      double t = x + p.decay * mTankOut[1 - c];
      t = mModAP[c].Process(t, -0.70, mod[c]);
      t = mDelayA[c].Process(t);
      t = mDampLP[c].Process(t, p.damping);
      t *= p.decay;
      t = mDecayAP[c].Process(t, 0.50);
      t = mDelayB[c].Process(t);
      newOut[c] = t;
    }
    mTankOut[0] = newOut[0];
    mTankOut[1] = newOut[1];

    // Ulostulot luetaan useasta kohdasta tankkia (Dattorron taulukko)
    auto n = [this](int samples) { return std::max(0, (int)std::lround(samples * mScale)); };
    outL = 0.6
           * (mDelayA[1].Tap(n(266)) + mDelayA[1].Tap(n(2974)) - mDecayAP[1].Tap(n(1913)) + mDelayB[1].Tap(n(1996))
              - mDelayA[0].Tap(n(1990)) - mDecayAP[0].Tap(n(187)) - mDelayB[0].Tap(n(1066)));
    outR = 0.6
           * (mDelayA[0].Tap(n(353)) + mDelayA[0].Tap(n(3627)) - mDecayAP[0].Tap(n(1228)) + mDelayB[0].Tap(n(2673))
              - mDelayA[1].Tap(n(2111)) - mDecayAP[1].Tap(n(335)) - mDelayB[1].Tap(n(121)));
  }

private:
  Settings mSettings;
  double mScale = 1.0;
  FixedDelay mPreDelay;
  OnePoleLP mBandLP;
  Allpass mInAP[4];
  Allpass mModAP[2];
  FixedDelay mDelayA[2];
  OnePoleLP mDampLP[2];
  Allpass mDecayAP[2];
  FixedDelay mDelayB[2];
  double mTankOut[2] = {0.0, 0.0};
  double mLfoPhase = 0.0;
  double mLfoInc = 0.0;
};

// ---------------------------------------------------------------------------
// Oktaavin nostin Shimmeriä varten (kahden lukupään ristihäivytys)
// ---------------------------------------------------------------------------
class OctaveUp
{
public:
  void Init(double sampleRate)
  {
    mWindow = 0.060 * sampleRate; // 60 ms ikkuna
    mLine.Init((int)mWindow + 4);
    mPhase = 0.0;
  }
  void Clear()
  {
    mLine.Clear();
    mPhase = 0.0;
  }
  double Process(double x)
  {
    mLine.Push(x);
    // Lukupää liikkuu kaksinkertaisella nopeudella = oktaavi ylös
    mPhase += 1.0 / mWindow;
    if (mPhase >= 1.0)
      mPhase -= 1.0;
    double out = 0.0;
    for (int h = 0; h < 2; h++)
    {
      double ph = mPhase + 0.5 * h;
      if (ph >= 1.0)
        ph -= 1.0;
      const double delay = (1.0 - ph) * mWindow;
      const double gain = std::sin(kPi * ph); // Häivytys estää naksumisen
      out += gain * mLine.TapFrac(delay);
    }
    return out * 0.7071;
  }

private:
  DelayLine mLine;
  double mWindow = 1.0;
  double mPhase = 0.0;
};

// ---------------------------------------------------------------------------
// Salo: kolmen kaikutyypin stereokaiku
// ---------------------------------------------------------------------------
class Salo
{
public:
  enum Type
  {
    kHall = 0,
    kPlate,
    kShimmer
  };

  void Reset(double sampleRate)
  {
    mHall.Init(sampleRate);

    PlateReverb::Settings plate;
    plate.preDelaySeconds = 0.012;
    plate.bandwidth = 0.9995;
    plate.decay = 0.74;
    plate.damping = 0.10;
    mPlate.Init(sampleRate, plate);

    PlateReverb::Settings shimmer;
    shimmer.preDelaySeconds = 0.030;
    shimmer.bandwidth = 0.85;
    shimmer.decay = 0.86;
    shimmer.damping = 0.30;
    mShimmerTank.Init(sampleRate, shimmer);
    mShifter.Init(sampleRate);
    mShimmerHP.Set(sampleRate, 400.0);
    mShimmerFeedback = 0.0;

    mInputHP.Set(sampleRate, 150.0); // Kaikuun ei bassoa: soundi pysyy selkeänä
    mMix.Reset(sampleRate);
    mType = -1;
    mReady = true;
  }

  // io: ulostulokanavat (1 tai 2), amount 0..1, type 0..2
  void Process(double** io, size_t numChannels, size_t nFrames, double amount, int type)
  {
    if (!mReady || numChannels == 0)
      return;
    type = std::clamp(type, 0, 2);
    if (type != mType)
    {
      // Tyhjennetään uusi kaiku, ettei vanhaa häntää kuulu yllättäen
      switch (type)
      {
        case kHall: mHall.Clear(); break;
        case kPlate: mPlate.Clear(); break;
        case kShimmer:
          mShimmerTank.Clear();
          mShifter.Clear();
          mShimmerHP.Clear();
          mShimmerFeedback = 0.0;
          break;
      }
      mType = type;
    }
    mMix.SetTarget(amount);

    for (size_t s = 0; s < nFrames; s++)
    {
      const double mix = mMix.Next();
      const double dry = (numChannels >= 2) ? 0.5 * (io[0][s] + io[1][s]) : io[0][s];
      const double in = mInputHP.Process(dry);
      double wetL = 0.0, wetR = 0.0;

      switch (mType)
      {
        case kHall:
          mHall.Process(in, wetL, wetR);
          wetL *= kHallGain;
          wetR *= kHallGain;
          break;
        case kPlate:
          mPlate.Process(in, wetL, wetR);
          wetL *= kPlateGain;
          wetR *= kPlateGain;
          break;
        case kShimmer:
        {
          // Kaiun oma ääni nostetaan oktaavilla ja syötetään takaisin:
          // jokainen kierros soi korkeammalta ja pehmeämmin
          const double shifted = mShimmerHP.Process(mShifter.Process(mShimmerFeedback));
          const double tankIn = in + std::tanh(kShimmerAmount * shifted);
          mShimmerTank.Process(tankIn, wetL, wetR);
          mShimmerFeedback = 0.5 * (wetL + wetR);
          wetL *= kShimmerGain;
          wetR *= kShimmerGain;
          break;
        }
      }

      if (numChannels >= 2)
      {
        io[0][s] += mix * wetL;
        io[1][s] += mix * wetR;
        for (size_t c = 2; c < numChannels; c++)
          io[c][s] += mix * 0.5 * (wetL + wetR);
      }
      else
      {
        io[0][s] += mix * 0.5 * (wetL + wetR);
      }
    }
  }

private:
  // Tasot säädetty niin, että tyypit kuulostavat suunnilleen yhtä voimakkailta
  static constexpr double kHallGain = 10.0;
  static constexpr double kPlateGain = 0.39;
  static constexpr double kShimmerGain = 0.39;
  static constexpr double kShimmerAmount = 0.45;

  HallReverb mHall;
  PlateReverb mPlate;
  PlateReverb mShimmerTank;
  OctaveUp mShifter;
  OnePoleHP mShimmerHP;
  double mShimmerFeedback = 0.0;
  OnePoleHP mInputHP;
  Smoother mMix;
  int mType = -1;
  bool mReady = false;
};

// ---------------------------------------------------------------------------
// Metso: lämmin, hieman tummuva viive (analogisen viiveen henkeen)
// ---------------------------------------------------------------------------
class Metso
{
public:
  void Reset(double sampleRate)
  {
    mSampleRate = sampleRate;
    mBuffer.assign((size_t)(kMaxDelaySeconds * sampleRate) + 4, 0.0);
    mWriteIndex = 0;
    mLowPassState = 0.0;
    mHighPassState = 0.0;
    mHighPassPrevIn = 0.0;
    // Toistojen tummennus n. 3,2 kHz ja ohennus alapäästä n. 120 Hz
    const double pi = 3.14159265358979323846;
    mLowPassCoeff = std::exp(-2.0 * pi * 3200.0 / sampleRate);
    mHighPassCoeff = std::exp(-2.0 * pi * 120.0 / sampleRate);
    mMix.Reset(sampleRate);
    mDelaySamples = kDelaySeconds * sampleRate;
  }

  // amount: 0..1 (nupin arvo)
  void Process(double* io, size_t nFrames, double amount)
  {
    if (mBuffer.empty())
      return;
    mMix.SetTarget(amount);
    const size_t size = mBuffer.size();
    for (size_t s = 0; s < nFrames; s++)
    {
      const double mix = mMix.Next();
      const double dry = io[s];

      // Luetaan viivästetty ääni (lineaarinen interpolointi)
      double readPos = (double)mWriteIndex - mDelaySamples;
      while (readPos < 0.0)
        readPos += (double)size;
      const size_t i0 = (size_t)readPos;
      const size_t i1 = (i0 + 1) % size;
      const double frac = readPos - (double)i0;
      const double delayed = mBuffer[i0] * (1.0 - frac) + mBuffer[i1] * frac;

      // Takaisinkytkennän suodatus: jokainen toisto vähän tummempi ja ohuempi
      mLowPassState = delayed * (1.0 - mLowPassCoeff) + mLowPassState * mLowPassCoeff;
      const double hp = mHighPassCoeff * (mHighPassState + mLowPassState - mHighPassPrevIn);
      mHighPassPrevIn = mLowPassState;
      mHighPassState = hp;

      // Pehmeä leikkaus estää viiveen karkaamisen käsistä
      mBuffer[mWriteIndex] = std::tanh(dry + kFeedback * hp);
      if (++mWriteIndex >= size)
        mWriteIndex = 0;

      io[s] = dry + mix * kWetGain * delayed;
    }
  }

private:
  static constexpr double kMaxDelaySeconds = 2.0;
  static constexpr double kDelaySeconds = 0.42; // Klassinen lead-viive
  static constexpr double kFeedback = 0.42; // Muutama hiipuva toisto
  static constexpr double kWetGain = 0.7;

  double mSampleRate = 48000.0;
  std::vector<double> mBuffer;
  size_t mWriteIndex = 0;
  double mDelaySamples = 0.0;
  double mLowPassCoeff = 0.0;
  double mLowPassState = 0.0;
  double mHighPassCoeff = 0.0;
  double mHighPassState = 0.0;
  double mHighPassPrevIn = 0.0;
  Smoother mMix;
};

} // namespace korpi
