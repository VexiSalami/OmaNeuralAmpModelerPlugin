// KorpiFX.h
// Korpi-pluginin omat efektit: Salo (kaiku / reverb) ja Metso (viive / delay).
// Molemmat ovat mono-efektejä, koska plugin käsittelee äänen sisäisesti monona.
//
// Salo perustuu Freeverb-algoritmiin (Jezar at Dreampoint, public domain).

#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace korpi
{

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
// Salo: Freeverb-tyylinen mono-kaiku
// ---------------------------------------------------------------------------
class Salo
{
public:
  void Reset(double sampleRate)
  {
    mSampleRate = sampleRate;
    const double scale = sampleRate / 44100.0;
    static const int combTunings[kNumCombs] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static const int allpassTunings[kNumAllpasses] = {556, 441, 341, 225};
    for (int i = 0; i < kNumCombs; i++)
    {
      mCombs[i].buffer.assign(std::max(1, (int)(combTunings[i] * scale)), 0.0);
      mCombs[i].index = 0;
      mCombs[i].filterStore = 0.0;
    }
    for (int i = 0; i < kNumAllpasses; i++)
    {
      mAllpasses[i].buffer.assign(std::max(1, (int)(allpassTunings[i] * scale)), 0.0);
      mAllpasses[i].index = 0;
    }
    // Esiviive (pre-delay) n. 25 ms, jotta kaiku ei puuroudu suoraan soittoon.
    mPreDelay.assign(std::max(1, (int)(0.025 * sampleRate)), 0.0);
    mPreDelayIndex = 0;
    mMix.Reset(sampleRate);
  }

  // amount: 0..1 (nupin arvo)
  void Process(double* io, size_t nFrames, double amount)
  {
    if (mPreDelay.empty())
      return;
    mMix.SetTarget(amount);
    for (size_t s = 0; s < nFrames; s++)
    {
      const double mix = mMix.Next();
      const double dry = io[s];

      // Esiviive
      const double delayed = mPreDelay[mPreDelayIndex];
      mPreDelay[mPreDelayIndex] = dry;
      if (++mPreDelayIndex >= mPreDelay.size())
        mPreDelayIndex = 0;

      const double input = delayed * kFixedGain;
      double out = 0.0;
      for (int i = 0; i < kNumCombs; i++)
        out += mCombs[i].Process(input, kRoomSize, kDamp);
      for (int i = 0; i < kNumAllpasses; i++)
        out = mAllpasses[i].Process(out);

      // Kuiva ääni pysyy ennallaan, kaikua lisätään päälle.
      io[s] = dry + mix * kWetGain * out;
    }
  }

private:
  static constexpr int kNumCombs = 8;
  static constexpr int kNumAllpasses = 4;
  static constexpr double kFixedGain = 0.015;
  static constexpr double kRoomSize = 0.86; // Iso tila, pitkä hännä
  static constexpr double kDamp = 0.35; // Diskantin vaimennus: lämmin, ei kirkas
  static constexpr double kWetGain = 2.2;

  struct Comb
  {
    std::vector<double> buffer;
    size_t index = 0;
    double filterStore = 0.0;
    double Process(double input, double feedback, double damp)
    {
      const double output = buffer[index];
      filterStore = output * (1.0 - damp) + filterStore * damp;
      buffer[index] = input + filterStore * feedback;
      if (++index >= buffer.size())
        index = 0;
      return output;
    }
  };

  struct Allpass
  {
    std::vector<double> buffer;
    size_t index = 0;
    double Process(double input)
    {
      const double bufout = buffer[index];
      const double output = -input + bufout;
      buffer[index] = input + bufout * 0.5;
      if (++index >= buffer.size())
        index = 0;
      return output;
    }
  };

  double mSampleRate = 48000.0;
  Comb mCombs[kNumCombs];
  Allpass mAllpasses[kNumAllpasses];
  std::vector<double> mPreDelay;
  size_t mPreDelayIndex = 0;
  Smoother mMix;
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
