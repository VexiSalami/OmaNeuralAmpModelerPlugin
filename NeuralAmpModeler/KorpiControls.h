// KorpiControls.h
// Korpi-pluginin omat näkymät: metronomi- ja viritysnäkymän pohja sekä viritysmittarin näyttö.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "IControl.h"
#include "Colors.h"

using namespace iplug;
using namespace igraphics;

// Koko ikkunan peittävä näkymä. Taustalla on Korpin taustakuva himmennettynä,
// ja klikkaukset eivät mene läpi alla oleviin säätimiin.
class KorpiPanelControl : public IContainerBase
{
public:
  KorpiPanelControl(const IRECT& bounds, const IBitmap& background, const char* title)
  : IContainerBase(bounds)
  , mBackground(background)
  , mTitle(title)
  {
    mIgnoreMouse = false;
  }

  void OnRescale() override { mBackground = GetUI()->GetScaledBitmap(mBackground); }

  void Draw(IGraphics& g) override
  {
    g.DrawFittedBitmap(mBackground, mRECT);
    g.FillRect(COLOR_BLACK.WithOpacity(0.35f), mRECT);
    const IRECT titleRect = mRECT.GetPadded(-30.f).GetFromTop(50.f);
    g.DrawText(IText(30.f, PluginColors::NAM_THEMEFONTCOLOR, "Michroma-Regular"), mTitle.Get(), titleRect);
  }

  // Klikkaukset taustaan eivät tee mitään (eivätkä mene läpi)
  void OnMouseDown(float x, float y, const IMouseMod& mod) override {}

private:
  IBitmap mBackground;
  WDL_String mTitle;
};

// Viritysmittarin näyttö: sävelen nimi, poikkeama senteissä ja viisari.
class KorpiTunerDisplayControl : public IControl
{
public:
  KorpiTunerDisplayControl(const IRECT& bounds)
  : IControl(bounds)
  {
    mIgnoreMouse = true;
  }

  // freq <= 0 tarkoittaa, ettei säveltä tunnistettu
  void SetFrequency(double freq)
  {
    if (freq <= 0.0)
    {
      // Pidetään edellinen lukema hetken, ettei näyttö välky
      if (++mSilentFrames > 20)
        mHasNote = false;
    }
    else
    {
      mSilentFrames = 0;
      const double midi = 69.0 + 12.0 * std::log2(freq / 440.0);
      const int nearest = (int)std::lround(midi);
      const double cents = (midi - nearest) * 100.0;
      if (mHasNote && nearest == mNote)
        mCents = 0.6 * mCents + 0.4 * cents; // Pehmennys
      else
        mCents = cents;
      mNote = nearest;
      mFreq = freq;
      mHasNote = true;
    }
    SetDirty(false);
  }

  void Draw(IGraphics& g) override
  {
    static const char* names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const IColor fg = PluginColors::NAM_THEMEFONTCOLOR;
    const IColor green = PluginColors::NAM_THEMECOLOR;
    const bool inTune = mHasNote && std::fabs(mCents) <= 3.0;

    // Asteikko
    const IRECT scale = mRECT.GetFromBottom(60.f).GetPadded(-20.f, 0.f, -20.f, -20.f);
    const float cy = scale.MH();
    g.DrawLine(fg.WithOpacity(0.4f), scale.L, cy, scale.R, cy, nullptr, 2.f);
    for (int c = -50; c <= 50; c += 10)
    {
      const float x = scale.L + (c + 50) / 100.f * scale.W();
      const float h = (c == 0) ? 14.f : 7.f;
      g.DrawLine(fg.WithOpacity(c == 0 ? 0.9f : 0.4f), x, cy - h, x, cy + h, nullptr, 2.f);
    }
    g.DrawText(IText(13.f, fg.WithOpacity(0.6f)), "-50", IRECT(scale.L - 20.f, cy + 12.f, scale.L + 20.f, cy + 30.f));
    g.DrawText(IText(13.f, fg.WithOpacity(0.6f)), "+50", IRECT(scale.R - 20.f, cy + 12.f, scale.R + 20.f, cy + 30.f));

    const IRECT noteRect = mRECT.GetReducedFromBottom(60.f).GetReducedFromBottom(30.f);
    const IRECT infoRect = mRECT.GetReducedFromBottom(60.f).GetFromBottom(30.f);

    if (!mHasNote)
    {
      g.DrawText(IText(22.f, fg.WithOpacity(0.6f)), "Soita kieli...", noteRect);
      return;
    }

    // Viisari
    const float cents = (float)std::clamp(mCents, -50.0, 50.0);
    const float nx = scale.L + (cents + 50.f) / 100.f * scale.W();
    g.FillRoundRect(inTune ? green : IColor(255, 230, 170, 80), IRECT(nx - 4.f, cy - 18.f, nx + 4.f, cy + 18.f), 3.f);

    // Sävelen nimi ja oktaavi (esim. "E2")
    char noteStr[16];
    const int octave = mNote / 12 - 1;
    std::snprintf(noteStr, sizeof(noteStr), "%s%d", names[((mNote % 12) + 12) % 12], octave);
    g.DrawText(IText(64.f, inTune ? green : fg, "Michroma-Regular"), noteStr, noteRect);

    char info[64];
    std::snprintf(info, sizeof(info), "%+.0f senttiä   ·   %.1f Hz", mCents, mFreq);
    g.DrawText(IText(15.f, fg.WithOpacity(0.75f)), info, infoRect);
  }

private:
  bool mHasNote = false;
  int mNote = 69;
  double mCents = 0.0;
  double mFreq = 0.0;
  int mSilentFrames = 0;
};
