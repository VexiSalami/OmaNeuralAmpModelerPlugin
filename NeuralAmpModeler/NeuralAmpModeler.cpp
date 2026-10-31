#include <algorithm> // std::clamp, std::min
#include <cmath> // pow
#include <cstdlib> // getenv
#include <filesystem>
#include <iostream>
#include <utility>

#include "Colors.h"
#include "../NeuralAmpModelerCore/NAM/activations.h"
#include "../NeuralAmpModelerCore/NAM/get_dsp.h"
// clang-format off
// These includes need to happen in this order or else the latter won't know
// a bunch of stuff.
#include "NeuralAmpModeler.h"
#include "IPlug_include_in_plug_src.h"
// clang-format on
#include "architecture.hpp"

#include "NeuralAmpModelerControls.h"
#include "KorpiControls.h"
#include <chrono>
#include <fstream>

#ifdef _WIN32
  #ifndef NOMINMAX
    #define NOMINMAX
  #endif
  #include <windows.h>
  #include <shlobj.h>
  #include <knownfolders.h>
  #pragma comment(lib, "shell32.lib")
  #pragma comment(lib, "ole32.lib")
  #pragma comment(lib, "uuid.lib")
#endif

// Korpi: UTF-8 string -> filesystem path (handles non-ASCII user names on Windows)
static std::filesystem::path KorpiFsPath(const std::string& utf8)
{
#if defined(__cpp_char8_t)
  return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
#else
  return std::filesystem::u8path(utf8);
#endif
}

// Korpi: returns Documents/Korpi/<sub> (created if missing), or "" if that fails.
static std::string KorpiUserFolder(const char* sub)
{
  std::string docs;
#ifdef _WIN32
  PWSTR widePath = nullptr;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_CREATE, nullptr, &widePath)) && widePath)
  {
    const int len = WideCharToMultiByte(CP_UTF8, 0, widePath, -1, nullptr, 0, nullptr, nullptr);
    if (len > 1)
    {
      docs.resize((size_t)len - 1);
      WideCharToMultiByte(CP_UTF8, 0, widePath, -1, &docs[0], len, nullptr, nullptr);
    }
  }
  if (widePath)
    CoTaskMemFree(widePath);
  const char* sep = "\\";
#else
  if (const char* home = std::getenv("HOME"))
    docs = std::string(home) + "/Documents";
  const char* sep = "/";
#endif
  if (docs.empty())
    return "";
  const std::string dir = docs + sep + "Korpi" + sep + sub;
  try
  {
#if defined(__cpp_char8_t)
    const std::filesystem::path fsPath(std::u8string(dir.begin(), dir.end()));
#else
    const std::filesystem::path fsPath = std::filesystem::u8path(dir);
#endif
    std::filesystem::create_directories(fsPath);
  }
  catch (...)
  {
    return "";
  }
  return dir;
}

using namespace iplug;
using namespace igraphics;

const double kDCBlockerFrequency = 5.0;

// Styles
const IVColorSpec colorSpec{
  DEFAULT_BGCOLOR, // Background
  PluginColors::NAM_THEMECOLOR, // Foreground
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.3f), // Pressed
  PluginColors::NAM_THEMECOLOR.WithOpacity(0.4f), // Frame
  PluginColors::MOUSEOVER, // Highlight
  DEFAULT_SHCOLOR, // Shadow
  PluginColors::NAM_THEMECOLOR, // Extra 1
  COLOR_RED, // Extra 2 --> color for clipping in meters
  PluginColors::NAM_THEMECOLOR.WithContrast(0.1f), // Extra 3
};

const IVStyle style =
  IVStyle{true, // Show label
          true, // Show value
          colorSpec,
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Middle, PluginColors::NAM_THEMEFONTCOLOR}, // Knob label text5
          {DEFAULT_TEXT_SIZE + 3.f, EVAlign::Bottom, PluginColors::NAM_THEMEFONTCOLOR}, // Knob value text
          DEFAULT_HIDE_CURSOR,
          DEFAULT_DRAW_FRAME,
          false,
          DEFAULT_EMBOSS,
          0.2f,
          2.f,
          DEFAULT_SHADOW_OFFSET,
          DEFAULT_WIDGET_FRAC,
          DEFAULT_WIDGET_ANGLE};
const IVStyle titleStyle =
  DEFAULT_STYLE.WithValueText(IText(30, COLOR_WHITE, "Michroma-Regular")).WithDrawFrame(false).WithShadowOffset(2.f);
const IVStyle radioButtonStyle =
  style
    .WithColor(EVColor::kON, PluginColors::NAM_THEMECOLOR) // Pressed buttons and their labels
    .WithColor(EVColor::kOFF, PluginColors::NAM_THEMECOLOR.WithOpacity(0.1f)) // Unpressed buttons
    .WithColor(EVColor::kX1, PluginColors::NAM_THEMECOLOR.WithOpacity(0.6f)); // Unpressed buttons' labels

EMsgBoxResult _ShowMessageBox(iplug::igraphics::IGraphics* pGraphics, const char* str, const char* caption,
                              EMsgBoxType type)
{
#ifdef OS_MAC
  // macOS is backwards?
  return pGraphics->ShowMessageBox(caption, str, type);
#else
  return pGraphics->ShowMessageBox(str, caption, type);
#endif
}

const std::string kCalibrateInputParamName = "CalibrateInput";
const bool kDefaultCalibrateInput = false;
const std::string kInputCalibrationLevelParamName = "InputCalibrationLevel";
const double kDefaultInputCalibrationLevel = 12.0;


NeuralAmpModeler::NeuralAmpModeler(const InstanceInfo& info)
: Plugin(info, MakeConfig(kNumParams, kNumPresets))
{
  _InitToneStack();
  nam::activations::Activation::enable_fast_tanh();
  GetParam(kInputLevel)->InitGain("Input", 0.0, -20.0, 20.0, 0.1);
  GetParam(kToneBass)->InitDouble("Juuret", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneMid)->InitDouble("Kaarna", 5.0, 0.0, 10.0, 0.1);
  GetParam(kToneTreble)->InitDouble("Neulaset", 5.0, 0.0, 10.0, 0.1);
  GetParam(kSalo)->InitDouble("Salo", 0.0, 0.0, 100.0, 1.0, "%");
  GetParam(kMetso)->InitDouble("Käki", 0.0, 0.0, 100.0, 1.0, "%");
  GetParam(kSaloType)->InitEnum("SaloType", 0, {"Hall", "Plate", "Shimmer"});
  GetParam(kSaloActive)->InitBool("SaloActive", true);
  GetParam(kMetsoActive)->InitBool("KakiActive", true);
  GetParam(kMetroOn)->InitBool("MetroOn", false);
  GetParam(kMetroBPM)->InitDouble("MetroBPM", 120.0, 30.0, 300.0, 1.0, "BPM");
  GetParam(kMetroVolume)->InitGain("MetroVolume", -12.0, -40.0, 0.0, 0.5);
  GetParam(kOutputLevel)->InitGain("Output", 0.0, -40.0, 40.0, 0.1);
  GetParam(kNoiseGateThreshold)->InitGain("Threshold", -80.0, -100.0, 0.0, 0.1);
  GetParam(kNoiseGateActive)->InitBool("NoiseGateActive", true);
  GetParam(kEQActive)->InitBool("ToneStack", true);
  GetParam(kOutputMode)->InitEnum("OutputMode", 1, {"Raw", "Normalized", "Calibrated"}); // TODO DRY w/ control
  GetParam(kIRToggle)->InitBool("IRToggle", true);
  GetParam(kCalibrateInput)->InitBool(kCalibrateInputParamName.c_str(), kDefaultCalibrateInput);
  GetParam(kInputCalibrationLevel)
    ->InitDouble(kInputCalibrationLevelParamName.c_str(), kDefaultInputCalibrationLevel, -60.0, 60.0, 0.1, "dBu");
  GetParam(kSlim)->InitDouble("Slim", 0.0, 0.0, 1.0, 0.01);

  mNoiseGateTrigger.AddListener(&mNoiseGateGain);

  mMakeGraphicsFunc = [&]() {

#ifdef OS_IOS
    auto scaleFactor = GetScaleForScreen(PLUG_WIDTH, PLUG_HEIGHT) * 0.85f;
#else
    auto scaleFactor = 1.0f;
#endif

    return MakeGraphics(*this, PLUG_WIDTH, PLUG_HEIGHT, PLUG_FPS, scaleFactor);
  };

  mLayoutFunc = [&](IGraphics* pGraphics) {
    pGraphics->AttachCornerResizer(EUIResizerMode::Scale, false);
    pGraphics->AttachTextEntryControl();
    pGraphics->EnableMouseOver(true);
    pGraphics->EnableTooltips(true);
    pGraphics->EnableMultiTouch(true);

    pGraphics->LoadFont("Roboto-Regular", ROBOTO_FN);
    pGraphics->LoadFont("Michroma-Regular", MICHROMA_FN);

    const auto gearSVG = pGraphics->LoadSVG(GEAR_FN);
    const auto fileSVG = pGraphics->LoadSVG(FILE_FN);
    const auto globeSVG = pGraphics->LoadSVG(GLOBE_ICON_FN);
    const auto crossSVG = pGraphics->LoadSVG(CLOSE_BUTTON_FN);
    const auto rightArrowSVG = pGraphics->LoadSVG(RIGHT_ARROW_FN);
    const auto leftArrowSVG = pGraphics->LoadSVG(LEFT_ARROW_FN);
    const auto modelIconSVG = pGraphics->LoadSVG(MODEL_ICON_FN);
    const auto irIconOnSVG = pGraphics->LoadSVG(IR_ICON_ON_FN);
    const auto irIconOffSVG = pGraphics->LoadSVG(IR_ICON_OFF_FN);
    const auto slimIconSVG = pGraphics->LoadSVG(SLIMMABLE_ICON_FN);

    const auto backgroundBitmap = pGraphics->LoadBitmap(BACKGROUND_FN);
    const auto fileBackgroundBitmap = pGraphics->LoadBitmap(FILEBACKGROUND_FN);
    const auto inputLevelBackgroundBitmap = pGraphics->LoadBitmap(INPUTLEVELBACKGROUND_FN);
    const auto knobBackgroundBitmap = pGraphics->LoadBitmap(KNOBBACKGROUND_FN);
    const auto switchHandleBitmap = pGraphics->LoadBitmap(SLIDESWITCHHANDLE_FN);
    const auto meterBackgroundBitmap = pGraphics->LoadBitmap(METERBACKGROUND_FN);

    const auto b = pGraphics->GetBounds();
    const auto mainArea = b.GetPadded(-20);
    const auto contentArea = mainArea.GetPadded(-10);
    const auto titleHeight = 50.0f;
    const auto titleArea = contentArea.GetFromTop(titleHeight);

    // Areas for knobs
    const auto knobsPad = 20.0f;
    const auto knobsExtraSpaceBelowTitle = 25.0f;
    const auto singleKnobPad = -2.0f;
    const auto knobsArea = contentArea.GetFromTop(NAM_KNOB_HEIGHT)
                             .GetReducedFromLeft(knobsPad)
                             .GetReducedFromRight(knobsPad)
                             .GetVShifted(titleHeight + knobsExtraSpaceBelowTitle);
    const auto inputKnobArea = knobsArea.GetGridCell(0, kInputLevel, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto noiseGateArea = knobsArea.GetGridCell(0, kNoiseGateThreshold, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto bassKnobArea = knobsArea.GetGridCell(0, kToneBass, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto midKnobArea = knobsArea.GetGridCell(0, kToneMid, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto trebleKnobArea = knobsArea.GetGridCell(0, kToneTreble, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto saloKnobArea = knobsArea.GetGridCell(0, kSalo, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto metsoKnobArea = knobsArea.GetGridCell(0, kMetso, 1, numKnobs).GetPadded(-singleKnobPad);
    const auto outputKnobArea = knobsArea.GetGridCell(0, kOutputLevel, 1, numKnobs).GetPadded(-singleKnobPad);

    const auto ngToggleArea =
      noiseGateArea.GetVShifted(noiseGateArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto eqToggleArea = midKnobArea.GetVShifted(midKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    // Korpi: on/off switches under Salo and Metso, reverb type selector below them
    const auto saloToggleArea = saloKnobArea.GetVShifted(saloKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto metsoToggleArea =
      metsoKnobArea.GetVShifted(metsoKnobArea.H()).SubRectVertical(2, 0).GetReducedFromTop(10.0f);
    const auto saloTypeArea =
      IRECT(saloKnobArea.L, saloToggleArea.B + 4.0f, metsoKnobArea.R, saloToggleArea.B + 32.0f).GetHPadded(-2.0f);

    // Areas for model and IR
    const auto fileWidth = 200.0f;
    const auto fileHeight = 30.0f;
    const auto irYOffset = 38.0f;
    const auto modelArea =
      contentArea.GetFromBottom((2.0f * fileHeight)).GetFromTop(fileHeight).GetMidHPadded(fileWidth).GetVShifted(-1);
    const auto slimIconArea =
      IRECT(modelArea.R + 6.f, modelArea.MH() - 14.f, modelArea.R + 6.f + 2.f * 28.f, modelArea.MH() + 14.f);
    const auto modelIconArea = modelArea.GetFromLeft(30).GetTranslated(-40, 10);
    const auto irArea = modelArea.GetVShifted(irYOffset);
    const auto irSwitchArea = irArea.GetFromLeft(30.0f).GetHShifted(-40.0f).GetScaledAboutCentre(0.6f);

    // Areas for meters
    const auto inputMeterArea = contentArea.GetFromLeft(30).GetHShifted(-20).GetMidVPadded(100).GetVShifted(-25);
    const auto outputMeterArea = contentArea.GetFromRight(30).GetHShifted(20).GetMidVPadded(100).GetVShifted(-25);

    // Misc Areas
    const auto settingsButtonArea = CornerButtonArea(b);

    // Model loader button
    auto loadModelCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        // Sets mNAMPath and mStagedNAM
        const std::string msg = _StageModel(fileName);
        // TODO error messages like the IR loader.
        if (msg.size())
        {
          std::stringstream ss;
          ss << "Failed to load NAM model. Message:\n\n" << msg;
          _ShowMessageBox(GetUI(), ss.str().c_str(), "Failed to load model!", kMB_OK);
        }
        std::cout << "Loaded: " << fileName.Get() << std::endl;
      }
    };

    // IR loader button
    auto loadIRCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength())
      {
        mIRPath = fileName;
        const dsp::wav::LoadReturnCode retCode = _StageIR(fileName);
        if (retCode != dsp::wav::LoadReturnCode::SUCCESS)
        {
          std::stringstream message;
          message << "Failed to load IR file " << fileName.Get() << ":\n";
          message << dsp::wav::GetMsgForLoadReturnCode(retCode);

          _ShowMessageBox(GetUI(), message.str().c_str(), "Failed to load IR!", kMB_OK);
        }
      }
    };

    pGraphics->AttachBackground(BACKGROUND_FN);
    pGraphics->AttachControl(new IVLabelControl(titleArea, "KORPI", titleStyle));
    pGraphics->AttachControl(new ISVGControl(modelIconArea, modelIconSVG));

#ifdef NAM_PICK_DIRECTORY
    const std::string defaultNamFileString = "Select model directory...";
    const std::string defaultIRString = "Select IR directory...";
#else
    const std::string defaultNamFileString = "Valitse soundi...";
    const std::string defaultIRString = "Valitse kaappi...";
#endif
    // Getting started page listing additional resources
    const char* const getUrl = "https://www.neuralampmodeler.com/users#comp-marb84o5";
    pGraphics->AttachControl(
      new NAMFileBrowserControl(modelArea, kMsgTagClearModel, defaultNamFileString.c_str(), "nam",
                                loadModelCompletionHandler, style, fileSVG, crossSVG, leftArrowSVG, rightArrowSVG,
                                fileBackgroundBitmap, globeSVG, "Get NAM Models", getUrl),
      kCtrlTagModelFileBrowser);
    {
      const std::string dir = KorpiUserFolder("Soundit");
      if (!dir.empty())
        static_cast<NAMFileBrowserControl*>(pGraphics->GetControlWithTag(kCtrlTagModelFileBrowser))
          ->SetKorpiDefaultDirectory(dir.c_str());
    }

    auto hideSlimOverlay = [](IControl* pCaller) {
      IGraphics* ui = pCaller->GetUI();
      if (auto* backdrop = ui->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        backdrop->Hide(true);
      if (auto* knob = ui->GetControlWithTag(kCtrlTagSlimKnob))
        knob->Hide(true);
      ui->SetAllControlsDirty();
    };
    auto showSlimOverlay = [](IControl* pCaller) {
      IGraphics* ui = pCaller->GetUI();
      if (auto* backdrop = ui->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        backdrop->Hide(false);
      if (auto* knob = ui->GetControlWithTag(kCtrlTagSlimKnob))
        knob->Hide(false);
      ui->SetAllControlsDirty();
    };

    pGraphics
      ->AttachControl(
        new NAMSquareButtonControl(slimIconArea, DefaultClickActionFunc, slimIconSVG), kCtrlTagSlimmableIcon)
      ->SetAnimationEndActionFunction(showSlimOverlay)
      ->Hide(true);

    pGraphics->AttachControl(new ISVGSwitchControl(irSwitchArea, {irIconOffSVG, irIconOnSVG}, kIRToggle));
    pGraphics->AttachControl(
      new NAMFileBrowserControl(irArea, kMsgTagClearIR, defaultIRString.c_str(), "wav", loadIRCompletionHandler, style,
                                fileSVG, crossSVG, leftArrowSVG, rightArrowSVG, fileBackgroundBitmap, globeSVG,
                                "Get IRs", getUrl),
      kCtrlTagIRFileBrowser);
    {
      const std::string dir = KorpiUserFolder("Kaapit");
      if (!dir.empty())
        static_cast<NAMFileBrowserControl*>(pGraphics->GetControlWithTag(kCtrlTagIRFileBrowser))
          ->SetKorpiDefaultDirectory(dir.c_str());
    }
    pGraphics->AttachControl(
      new NAMSwitchControl(ngToggleArea, kNoiseGateActive, "Noise Gate", style, switchHandleBitmap));
    pGraphics->AttachControl(new NAMSwitchControl(eqToggleArea, kEQActive, "EQ", style, switchHandleBitmap));

    // The knobs
    pGraphics->AttachControl(new NAMKnobControl(inputKnobArea, kInputLevel, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(noiseGateArea, kNoiseGateThreshold, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(
      new NAMKnobControl(bassKnobArea, kToneBass, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(midKnobArea, kToneMid, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(
      new NAMKnobControl(trebleKnobArea, kToneTreble, "", style, knobBackgroundBitmap), -1, "EQ_KNOBS");
    pGraphics->AttachControl(new NAMKnobControl(saloKnobArea, kSalo, "", style, knobBackgroundBitmap));
    pGraphics
      ->AttachControl(new IVTabSwitchControl(saloTypeArea, kSaloType, {"Hall", "Plate", "Shimmer"}, "",
                                             radioButtonStyle.WithShowLabel(false)
                                               .WithRoundness(0.5f)
                                               .WithValueText(IText(DEFAULT_TEXT_SIZE - 1.f, EVAlign::Middle,
                                                                    PluginColors::NAM_THEMEFONTCOLOR))))
      ->SetTooltip("Salon kaikutyyppi");
    pGraphics->AttachControl(new NAMSwitchControl(saloToggleArea, kSaloActive, "", style, switchHandleBitmap))
      ->SetTooltip("Salo päälle / pois (säädöt säilyvät)");
    pGraphics->AttachControl(new NAMSwitchControl(metsoToggleArea, kMetsoActive, "", style, switchHandleBitmap))
      ->SetTooltip("Käki päälle / pois (säädöt säilyvät)");
    pGraphics->AttachControl(new NAMKnobControl(metsoKnobArea, kMetso, "", style, knobBackgroundBitmap));
    pGraphics->AttachControl(new NAMKnobControl(outputKnobArea, kOutputLevel, "", style, knobBackgroundBitmap));

    // The meters
    pGraphics->AttachControl(new NAMMeterControl(inputMeterArea, meterBackgroundBitmap, style), kCtrlTagInputMeter);
    pGraphics->AttachControl(new NAMMeterControl(outputMeterArea, meterBackgroundBitmap, style), kCtrlTagOutputMeter);

    // ---------------- Korpi: favourites, tuner and metronome ----------------
    const auto saveSVG = pGraphics->LoadSVG(KORPI_SAVE_FN);
    const auto tunerSVG = pGraphics->LoadSVG(KORPI_TUNER_FN);
    const auto metronomeSVG = pGraphics->LoadSVG(KORPI_METRONOME_FN);

    const IRECT presetArea(36.f, 40.f, 276.f, 70.f);
    const IRECT saveButtonArea(281.f, 45.f, 301.f, 65.f);
    const IRECT tunerButtonArea = settingsButtonArea.GetHShifted(-80.f);
    const IRECT metroButtonArea = settingsButtonArea.GetHShifted(-40.f);

    auto loadPresetCompletionHandler = [&](const WDL_String& fileName, const WDL_String& path) {
      if (fileName.GetLength() && !_KorpiLoadPreset(fileName))
        _ShowMessageBox(GetUI(), "Suosikin lataaminen ei onnistunut.", "Korpi", kMB_OK);
    };
    pGraphics->AttachControl(new NAMFileBrowserControl(presetArea, kMsgTagKorpiNoop, "Suosikit...", "korpi",
                                                       loadPresetCompletionHandler, style, fileSVG, crossSVG,
                                                       leftArrowSVG, rightArrowSVG, fileBackgroundBitmap, globeSVG,
                                                       "", getUrl),
                             kCtrlTagPresetBrowser);
    {
      auto* presetBrowser = static_cast<NAMFileBrowserControl*>(pGraphics->GetControlWithTag(kCtrlTagPresetBrowser));
      presetBrowser->KorpiHideGetButton();
      const std::string dir = KorpiUserFolder("Suosikit");
      if (!dir.empty())
        presetBrowser->SetKorpiDefaultDirectory(dir.c_str());
    }

    auto savePresetFunc = [&](IControl* pCaller) {
      WDL_String fileName("Uusi suosikki.korpi");
      WDL_String path(KorpiUserFolder("Suosikit").c_str());
      pCaller->GetUI()->PromptForFile(
        fileName, path, EFileAction::Save, "korpi", [&](const WDL_String& chosen, const WDL_String& chosenPath) {
          if (!chosen.GetLength())
            return; // Cancelled
          WDL_String full(chosen);
          const char* ext = full.get_fileext();
          if (!ext || strcmp(ext, ".korpi") != 0)
            full.Append(".korpi");
          if (_KorpiSavePreset(full))
          {
            if (auto* browser = GetUI()->GetControlWithTag(kCtrlTagPresetBrowser))
              browser->OnMsgFromDelegate(kMsgTagLoadedIR, full.GetLength(), full.Get());
          }
          else
            _ShowMessageBox(GetUI(), "Suosikin tallentaminen ei onnistunut.", "Korpi", kMB_OK);
        });
    };
    pGraphics->AttachControl(new NAMSquareButtonControl(saveButtonArea, DefaultClickActionFunc, saveSVG))
      ->SetAnimationEndActionFunction(savePresetFunc);
    pGraphics->GetControl(pGraphics->NControls() - 1)->SetTooltip("Tallenna nykyinen soundi suosikiksi");

    pGraphics
      ->AttachControl(new NAMCircleButtonControl(
        tunerButtonArea,
        [&](IControl* pCaller) {
          mTunerOpen = true;
          pCaller->GetUI()->GetControlWithTag(kCtrlTagTunerPanel)->Hide(false);
          pCaller->GetUI()->SetAllControlsDirty();
        },
        tunerSVG))
      ->SetTooltip("Otava");
    pGraphics
      ->AttachControl(new NAMCircleButtonControl(
        metroButtonArea,
        [](IControl* pCaller) {
          pCaller->GetUI()->GetControlWithTag(kCtrlTagMetroPanel)->Hide(false);
          pCaller->GetUI()->SetAllControlsDirty();
        },
        metronomeSVG))
      ->SetTooltip("Tikka");

    // Settings/help/about box
    pGraphics->AttachControl(new NAMCircleButtonControl(
      settingsButtonArea,
      [pGraphics](IControl* pCaller) {
        pGraphics->GetControlWithTag(kCtrlTagSettingsBox)->As<NAMSettingsPageControl>()->HideAnimated(false);
      },
      gearSVG));

    pGraphics
      ->AttachControl(new NAMSettingsPageControl(b, backgroundBitmap, inputLevelBackgroundBitmap, switchHandleBitmap,
                                                 crossSVG, style, radioButtonStyle),
                      kCtrlTagSettingsBox)
      ->Hide(true);

    const auto slimKnobArea = b.GetCentredInside(100.f, NAM_KNOB_HEIGHT + 24.f);
    pGraphics->AttachControl(new NAMSlimOverlayBackdropControl(b, hideSlimOverlay), kCtrlTagSlimOverlayBackdrop)
      ->Hide(true);
    pGraphics
      ->AttachControl(new NAMKnobControl(slimKnobArea, kSlim, "Slim", style, knobBackgroundBitmap), kCtrlTagSlimKnob)
      ->Hide(true);

    // Korpi: metronome panel
    {
      auto* panel = new KorpiPanelControl(b, backgroundBitmap, "TIKKA");
      pGraphics->AttachControl(panel, kCtrlTagMetroPanel);
      const IRECT row = b.GetCentredInside(450.f, NAM_KNOB_HEIGHT).GetVShifted(-10.f);
      const IRECT onArea = row.GetGridCell(0, 0, 1, 3).GetMidVPadded(30.f);
      const IRECT bpmArea = row.GetGridCell(0, 1, 1, 3);
      const IRECT volArea = row.GetGridCell(0, 2, 1, 3);
      panel->AddChildControl(new NAMSwitchControl(onArea, kMetroOn, "Päällä", style, switchHandleBitmap));
      panel->AddChildControl(new NAMKnobControl(bpmArea, kMetroBPM, "Tempo", style, knobBackgroundBitmap));
      panel->AddChildControl(new NAMKnobControl(volArea, kMetroVolume, "Voimakkuus", style, knobBackgroundBitmap));
      const IRECT tapArea = bpmArea.GetVShifted(bpmArea.H() + 8.f).GetFromTop(32.f).GetHPadded(10.f);
      panel->AddChildControl(new IVButtonControl(
        tapArea,
        [&](IControl* pCaller) {
          SplashClickActionFunc(pCaller);
          using clock = std::chrono::steady_clock;
          const double now = std::chrono::duration<double>(clock::now().time_since_epoch()).count();
          if (!mTapTimes.empty() && now - mTapTimes.back() > 2.0)
            mTapTimes.clear(); // Long pause: start a new tap sequence
          mTapTimes.push_back(now);
          if (mTapTimes.size() > 5)
            mTapTimes.erase(mTapTimes.begin());
          if (mTapTimes.size() >= 2)
          {
            const double avg = (mTapTimes.back() - mTapTimes.front()) / (double)(mTapTimes.size() - 1);
            const double bpm = std::clamp(std::round(60.0 / avg), 30.0, 300.0);
            const double norm = GetParam(kMetroBPM)->ToNormalized(bpm);
            BeginInformHostOfParamChangeFromUI(kMetroBPM);
            SendParameterValueFromUI(kMetroBPM, norm);
            EndInformHostOfParamChangeFromUI(kMetroBPM);
            SendParameterValueFromDelegate(kMetroBPM, norm, true);
          }
        },
        "Naputa tempo", style.WithShowValue(false)));
      panel->AddChildControl(new NAMSquareButtonControl(
        CornerButtonArea(b),
        [](IControl* pCaller) {
          pCaller->GetUI()->GetControlWithTag(kCtrlTagMetroPanel)->Hide(true);
          pCaller->GetUI()->SetAllControlsDirty();
        },
        crossSVG));
      panel->Hide(true);
    }

    // Korpi: tuner panel
    {
      auto* panel = new KorpiPanelControl(b, backgroundBitmap, "OTAVA");
      pGraphics->AttachControl(panel, kCtrlTagTunerPanel);
      panel->AddChildControl(
        new KorpiTunerDisplayControl(b.GetCentredInside(480.f, 220.f).GetVShifted(10.f)), kCtrlTagTunerDisplay);
      panel->AddChildControl(new ITextControl(b.GetFromBottom(60.f).GetVShifted(-10.f),
                                              "Ääni on mykistetty virityksen ajaksi.",
                                              IText(15.f, PluginColors::NAM_THEMEFONTCOLOR.WithOpacity(0.7f)),
                                              COLOR_TRANSPARENT));
      panel->AddChildControl(new NAMSquareButtonControl(
        CornerButtonArea(b),
        [&](IControl* pCaller) {
          mTunerOpen = false;
          pCaller->GetUI()->GetControlWithTag(kCtrlTagTunerPanel)->Hide(true);
          pCaller->GetUI()->SetAllControlsDirty();
        },
        crossSVG));
      panel->Hide(true);
    }

    pGraphics->ForAllControlsFunc([](IControl* pControl) {
      pControl->SetMouseEventsWhenDisabled(true);
      pControl->SetMouseOverWhenDisabled(true);
    });

    // pGraphics->GetControlWithTag(kCtrlTagOutNorm)->SetMouseEventsWhenDisabled(false);
    // pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetMouseEventsWhenDisabled(false);
  };
}

NeuralAmpModeler::~NeuralAmpModeler()
{
  _DeallocateIOPointers();
}

void NeuralAmpModeler::ProcessBlock(iplug::sample** inputs, iplug::sample** outputs, int nFrames)
{
  const size_t numChannelsExternalIn = (size_t)NInChansConnected();
  const size_t numChannelsExternalOut = (size_t)NOutChansConnected();
  const size_t numChannelsInternal = kNumChannelsInternal;
  const size_t numFrames = (size_t)nFrames;
  const double sampleRate = GetSampleRate();

  // Disable floating point denormals
  std::fenv_t fe_state;
  std::feholdexcept(&fe_state);
  disable_denormals();

  _PrepareBuffers(numChannelsInternal, numFrames);
  // Input is collapsed to mono in preparation for the NAM.
  _ProcessInput(inputs, numFrames, numChannelsExternalIn, numChannelsInternal);
  _ApplyDSPStaging();
  // Korpi: feed the tuner while its panel is open
  if (mTunerOpen)
    mTunerBuffer.Write(mInputPointers[0], numFrames);
  const bool noiseGateActive = GetParam(kNoiseGateActive)->Value();
  const bool toneStackActive = GetParam(kEQActive)->Value();

  // Noise gate trigger
  sample** triggerOutput = mInputPointers;
  if (noiseGateActive)
  {
    const double time = 0.01;
    const double threshold = GetParam(kNoiseGateThreshold)->Value(); // GetParam...
    const double ratio = 0.1; // Quadratic...
    const double openTime = 0.005;
    const double holdTime = 0.01;
    const double closeTime = 0.05;
    const dsp::noise_gate::TriggerParams triggerParams(time, threshold, ratio, openTime, holdTime, closeTime);
    mNoiseGateTrigger.SetParams(triggerParams);
    mNoiseGateTrigger.SetSampleRate(sampleRate);
    triggerOutput = mNoiseGateTrigger.Process(mInputPointers, numChannelsInternal, numFrames);
  }

  if (mModel != nullptr)
  {
    mModel->process(triggerOutput, mOutputPointers, nFrames);
  }
  else
  {
    _FallbackDSP(triggerOutput, mOutputPointers, numChannelsInternal, numFrames);
  }
  // Apply the noise gate after the NAM
  sample** gateGainOutput =
    noiseGateActive ? mNoiseGateGain.Process(mOutputPointers, numChannelsInternal, numFrames) : mOutputPointers;

  sample** toneStackOutPointers = (toneStackActive && mToneStack != nullptr)
                                    ? mToneStack->Process(gateGainOutput, numChannelsInternal, nFrames)
                                    : gateGainOutput;

  sample** irPointers = toneStackOutPointers;
  if (mIR != nullptr && GetParam(kIRToggle)->Value())
    irPointers = mIR->Process(toneStackOutPointers, numChannelsInternal, numFrames);

  // And the HPF for DC offset (Issue 271)
  const double highPassCutoffFreq = kDCBlockerFrequency;
  // const double lowPassCutoffFreq = 20000.0;
  const recursive_linear_filter::HighPassParams highPassParams(sampleRate, highPassCutoffFreq);
  // const recursive_linear_filter::LowPassParams lowPassParams(sampleRate, lowPassCutoffFreq);
  mHighPass.SetParams(highPassParams);
  // mLowPass.SetParams(lowPassParams);
  sample** hpfPointers = mHighPass.Process(irPointers, numChannelsInternal, numFrames);

  // Korpi: Metso (delay) here in mono; Salo (stereo reverb) is added after the output stage.
  mMetso.Process(hpfPointers[0], numFrames, GetParam(kMetso)->Value() / 100.0, GetParam(kMetsoActive)->Bool());
  // sample** lpfPointers = mLowPass.Process(hpfPointers, numChannelsInternal, numFrames);

  // restore previous floating point state
  std::feupdateenv(&fe_state);

  // Let's get outta here
  // This is where we exit mono for whatever the output requires.
  _ProcessOutput(hpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // Korpi: Salo works in stereo on the final outputs so the reverb can spread left/right.
  mSalo.Process(outputs, numChannelsExternalOut, numFrames, GetParam(kSalo)->Value() / 100.0,
                GetParam(kSaloType)->Int(), GetParam(kSaloActive)->Bool());
  // Korpi: metronome click on top of everything (not through the reverb)
  mMetronome.Process(outputs, numChannelsExternalOut, numFrames, GetParam(kMetroOn)->Bool(),
                     GetParam(kMetroBPM)->Value(), DBToAmp(GetParam(kMetroVolume)->Value()));
  // Korpi: silent while tuning
  if (mTunerOpen)
    for (size_t c = 0; c < numChannelsExternalOut; c++)
      std::fill(outputs[c], outputs[c] + numFrames, 0.0);
#ifdef APP_API // Keep the standalone app's output within range
  for (size_t c = 0; c < numChannelsExternalOut; c++)
    for (size_t s = 0; s < numFrames; s++)
      outputs[c][s] = std::clamp(outputs[c][s], -1.0, 1.0);
#endif
  // _ProcessOutput(lpfPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
  // * Output of input leveling (inputs -> mInputPointers),
  // * Output of output leveling (mOutputPointers -> outputs)
  _UpdateMeters(mInputPointers, outputs, numFrames, numChannelsInternal, numChannelsExternalOut);
}

void NeuralAmpModeler::OnReset()
{
  const auto sampleRate = GetSampleRate();
  const int maxBlockSize = GetBlockSize();

  // Tail is because the HPF DC blocker has a decay.
  // 10 cycles should be enough to pass the VST3 tests checking tail behavior.
  // I'm ignoring the model & IR, but it's not the end of the world.
  const int tailCycles = 10;
  // Korpi: Salo and Metso ring out longer than the DC blocker, so allow ~5 s of tail.
  SetTailSize(std::max(tailCycles * (int)(sampleRate / kDCBlockerFrequency), (int)(5.0 * sampleRate)));
  mSalo.Reset(sampleRate);
  mMetso.Reset(sampleRate);
  mMetronome.Reset(sampleRate);
  mTunerBuffer.Reset(sampleRate);
  mInputSender.Reset(sampleRate);
  mOutputSender.Reset(sampleRate);
  // If there is a model or IR loaded, they need to be checked for resampling.
  _ResetModelAndIR(sampleRate, GetBlockSize());
  mToneStack->Reset(sampleRate, maxBlockSize);
  _UpdateLatency();
}

void NeuralAmpModeler::OnIdle()
{
  mInputSender.TransmitData(*this);
  mOutputSender.TransmitData(*this);

  // Korpi: tuner pitch detection runs here (UI thread), not in the audio thread
  if (mTunerOpen)
  {
    if (auto* pGraphics = GetUI())
    {
      if (auto* pDisplay = pGraphics->GetControlWithTag(kCtrlTagTunerDisplay))
      {
        double freq = 0.0;
        if (mTunerBuffer.ReadLatest(mTunerScratch, 2048))
          freq = korpi::DetectPitch(mTunerScratch, mTunerBuffer.Rate());
        static_cast<KorpiTunerDisplayControl*>(pDisplay)->SetFrequency(freq);
      }
    }
  }

  if (mNewModelLoadedInDSP)
  {
    if (auto* pGraphics = GetUI())
    {
      _UpdateControlsFromModel();
      mNewModelLoadedInDSP = false;
    }
  }
  if (mModelCleared)
  {
    if (auto* pGraphics = GetUI())
    {
      // FIXME -- need to disable only the "normalized" model
      // pGraphics->GetControlWithTag(kCtrlTagOutputMode)->SetDisabled(false);
      static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->ClearModelInfo();
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimmableIcon))
        p->Hide(true);
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimOverlayBackdrop))
        p->Hide(true);
      if (auto* p = pGraphics->GetControlWithTag(kCtrlTagSlimKnob))
        p->Hide(true);
      pGraphics->SetAllControlsDirty();
      mModelCleared = false;
    }
  }
}

bool NeuralAmpModeler::SerializeState(IByteChunk& chunk) const
{
  // If this isn't here when unserializing, then we know we're dealing with something before v0.8.0.
  WDL_String header("###NeuralAmpModeler###"); // Don't change this!
  chunk.PutStr(header.Get());
  // Plugin version, so we can load legacy serialized states in the future!
  WDL_String version(PLUG_VERSION_STR);
  chunk.PutStr(version.Get());
  // Model directory (don't serialize the model itself; we'll just load it again
  // when we unserialize)
  chunk.PutStr(mNAMPath.Get());
  chunk.PutStr(mIRPath.Get());
  return SerializeParams(chunk);
}

int NeuralAmpModeler::UnserializeState(const IByteChunk& chunk, int startPos)
{
  // Look for the expected header. If it's there, then we'll know what to do.
  WDL_String header;
  int pos = startPos;
  pos = chunk.GetStr(header, pos);

  const char* kExpectedHeader = "###NeuralAmpModeler###";
  if (strcmp(header.Get(), kExpectedHeader) == 0)
  {
    return _UnserializeStateWithKnownVersion(chunk, pos);
  }
  else
  {
    return _UnserializeStateWithUnknownVersion(chunk, startPos);
  }
}

void NeuralAmpModeler::OnUIClose()
{
  // Korpi: never leave the output muted when the window closes with the tuner open
  mTunerOpen = false;
  Plugin::OnUIClose();
}

void NeuralAmpModeler::OnUIOpen()
{
  Plugin::OnUIOpen();

  if (mNAMPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, mNAMPath.GetLength(), mNAMPath.Get());
    // If it's not loaded yet, then mark as failed.
    // If it's yet to be loaded, then the completion handler will set us straight once it runs.
    if (mModel == nullptr && mStagedModel == nullptr)
      SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed);
  }

  if (mIRPath.GetLength())
  {
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
    if (mIR == nullptr && mStagedIR == nullptr)
      SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  if (mModel != nullptr)
  {
    _UpdateControlsFromModel();
  }
}

void NeuralAmpModeler::OnParamChange(int paramIdx)
{
  switch (paramIdx)
  {
    // Changes to the input gain
    case kCalibrateInput:
    case kInputCalibrationLevel:
    case kInputLevel: _SetInputGain(); break;
    // Changes to the output gain
    case kOutputLevel:
    case kOutputMode: _SetOutputGain(); break;
    // Tone stack:
    case kToneBass: mToneStack->SetParam("bass", GetParam(paramIdx)->Value()); break;
    case kToneMid: mToneStack->SetParam("middle", GetParam(paramIdx)->Value()); break;
    case kToneTreble: mToneStack->SetParam("treble", GetParam(paramIdx)->Value()); break;
    case kSlim: _ApplySlimParamToLoadedNAMs(); break;
    default: break;
  }
}

void NeuralAmpModeler::OnParamChangeUI(int paramIdx, EParamSource source)
{
  if (auto pGraphics = GetUI())
  {
    bool active = GetParam(paramIdx)->Bool();

    switch (paramIdx)
    {
      case kNoiseGateActive: pGraphics->GetControlWithParamIdx(kNoiseGateThreshold)->SetDisabled(!active); break;
      case kEQActive:
        pGraphics->ForControlInGroup("EQ_KNOBS", [active](IControl* pControl) { pControl->SetDisabled(!active); });
        break;
      case kIRToggle: pGraphics->GetControlWithTag(kCtrlTagIRFileBrowser)->SetDisabled(!active); break;
      case kSaloActive:
        pGraphics->GetControlWithParamIdx(kSalo)->SetDisabled(!active);
        pGraphics->GetControlWithParamIdx(kSaloType)->SetDisabled(!active);
        break;
      case kMetsoActive: pGraphics->GetControlWithParamIdx(kMetso)->SetDisabled(!active); break;
      default: break;
    }
  }
}

bool NeuralAmpModeler::OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData)
{
  switch (msgTag)
  {
    case kMsgTagClearModel: mShouldRemoveModel = true; return true;
    case kMsgTagClearIR: mShouldRemoveIR = true; return true;
    case kMsgTagHighlightColor:
    {
      mHighLightColor.Set((const char*)pData);

      if (GetUI())
      {
        GetUI()->ForStandardControlsFunc([&](IControl* pControl) {
          if (auto* pVectorBase = pControl->As<IVectorBase>())
          {
            IColor color = IColor::FromColorCodeStr(mHighLightColor.Get());

            pVectorBase->SetColor(kX1, color);
            pVectorBase->SetColor(kPR, color.WithOpacity(0.3f));
            pVectorBase->SetColor(kFR, color.WithOpacity(0.4f));
            pVectorBase->SetColor(kX3, color.WithContrast(0.1f));
          }
          pControl->GetUI()->SetAllControlsDirty();
        });
      }

      return true;
    }
    default: return false;
  }
}

// Private methods ============================================================

void NeuralAmpModeler::_AllocateIOPointers(const size_t nChans)
{
  if (mInputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mInputPointers without freeing");
  mInputPointers = new sample*[nChans];
  if (mInputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Tried to re-allocate mOutputPointers without freeing");
  mOutputPointers = new sample*[nChans];
  if (mOutputPointers == nullptr)
    throw std::runtime_error("Failed to allocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_ApplyDSPStaging()
{
  // Remove marked modules
  if (mShouldRemoveModel)
  {
    mModel = nullptr;
    mNAMPath.Set("");
    mShouldRemoveModel = false;
    mModelCleared = true;
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
  }
  if (mShouldRemoveIR)
  {
    mIR = nullptr;
    mIRPath.Set("");
    mShouldRemoveIR = false;
  }
  // Move things from staged to live
  if (mStagedModel != nullptr)
  {
    mModel = std::move(mStagedModel);
    mStagedModel = nullptr;
    mNewModelLoadedInDSP = true;
    _UpdateLatency();
    _SetInputGain();
    _SetOutputGain();
  }
  if (mStagedIR != nullptr)
  {
    mIR = std::move(mStagedIR);
    mStagedIR = nullptr;
  }
}

void NeuralAmpModeler::_DeallocateIOPointers()
{
  if (mInputPointers != nullptr)
  {
    delete[] mInputPointers;
    mInputPointers = nullptr;
  }
  if (mInputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to input buffer!\n");
  if (mOutputPointers != nullptr)
  {
    delete[] mOutputPointers;
    mOutputPointers = nullptr;
  }
  if (mOutputPointers != nullptr)
    throw std::runtime_error("Failed to deallocate pointer to output buffer!\n");
}

void NeuralAmpModeler::_FallbackDSP(iplug::sample** inputs, iplug::sample** outputs, const size_t numChannels,
                                    const size_t numFrames)
{
  for (auto c = 0; c < numChannels; c++)
    for (auto s = 0; s < numFrames; s++)
      mOutputArray[c][s] = mInputArray[c][s];
}

void NeuralAmpModeler::_ResetModelAndIR(const double sampleRate, const int maxBlockSize)
{
  // Model
  if (mStagedModel != nullptr)
  {
    mStagedModel->Reset(sampleRate, maxBlockSize);
  }
  else if (mModel != nullptr)
  {
    mModel->Reset(sampleRate, maxBlockSize);
  }

  // IR
  if (mStagedIR != nullptr)
  {
    const double irSampleRate = mStagedIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mStagedIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
  else if (mIR != nullptr)
  {
    const double irSampleRate = mIR->GetSampleRate();
    if (irSampleRate != sampleRate)
    {
      const auto irData = mIR->GetData();
      mStagedIR = std::make_unique<dsp::ImpulseResponse>(irData, sampleRate);
    }
  }
}

void NeuralAmpModeler::_SetInputGain()
{
  iplug::sample inputGainDB = GetParam(kInputLevel)->Value();
  // Input calibration
  if ((mModel != nullptr) && (mModel->HasInputLevel()) && GetParam(kCalibrateInput)->Bool())
  {
    inputGainDB += GetParam(kInputCalibrationLevel)->Value() - mModel->GetInputLevel();
  }
  mInputGain = DBToAmp(inputGainDB);
}

void NeuralAmpModeler::_SetOutputGain()
{
  double gainDB = GetParam(kOutputLevel)->Value();
  if (mModel != nullptr)
  {
    const int outputMode = GetParam(kOutputMode)->Int();
    switch (outputMode)
    {
      case 1: // Normalized
        if (mModel->HasLoudness())
        {
          const double loudness = mModel->GetLoudness();
          const double targetLoudness = -18.0;
          gainDB += (targetLoudness - loudness);
        }
        break;
      case 2: // Calibrated
        if (mModel->HasOutputLevel())
        {
          const double inputLevel = GetParam(kInputCalibrationLevel)->Value();
          const double outputLevel = mModel->GetOutputLevel();
          gainDB += (outputLevel - inputLevel);
        }
        break;
      case 0: // Raw
      default: break;
    }
  }
  mOutputGain = DBToAmp(gainDB);
}

void NeuralAmpModeler::_ApplySlimParamToLoadedNAMs()
{
  const double v = GetParam(kSlim)->Value();
  auto apply = [v](ResamplingNAM* p) {
    if (p == nullptr)
      return;
    if (nam::SlimmableModel* s = p->GetSlimmableModel())
      s->SetSlimmableSize(v);
  };
  apply(mModel.get());
  apply(mStagedModel.get());
}

std::string NeuralAmpModeler::_StageModel(const WDL_String& modelPath)
{
  WDL_String previousNAMPath = mNAMPath;
  try
  {
    auto dspPath = std::filesystem::u8path(modelPath.Get());
    std::unique_ptr<nam::DSP> model = nam::get_dsp(dspPath);

    // Check that the model has 1 input and 1 output channel
    if (model->NumInputChannels() != 1)
    {
      throw std::runtime_error("Model must have 1 input channel, but has " + std::to_string(model->NumInputChannels()));
    }
    if (model->NumOutputChannels() != 1)
    {
      throw std::runtime_error("Model must have 1 output channel, but has "
                               + std::to_string(model->NumOutputChannels()));
    }

    std::unique_ptr<ResamplingNAM> temp = std::make_unique<ResamplingNAM>(std::move(model), GetSampleRate());
    temp->Reset(GetSampleRate(), GetBlockSize());
    if (nam::SlimmableModel* slimmable = temp->GetSlimmableModel())
    {
      slimmable->SetSlimmableSize(GetParam(kSlim)->Value());
    }
    mStagedModel = std::move(temp);
    mNAMPath = modelPath;
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadedModel, mNAMPath.GetLength(), mNAMPath.Get());
  }
  catch (std::runtime_error& e)
  {
    SendControlMsgFromDelegate(kCtrlTagModelFileBrowser, kMsgTagLoadFailed);

    if (mStagedModel != nullptr)
    {
      mStagedModel = nullptr;
    }
    mNAMPath = previousNAMPath;
    std::cerr << "Failed to read DSP module" << std::endl;
    std::cerr << e.what() << std::endl;
    return e.what();
  }
  return "";
}

dsp::wav::LoadReturnCode NeuralAmpModeler::_StageIR(const WDL_String& irPath)
{
  // FIXME it'd be better for the path to be "staged" as well. Just in case the
  // path and the model got caught on opposite sides of the fence...
  WDL_String previousIRPath = mIRPath;
  const double sampleRate = GetSampleRate();
  dsp::wav::LoadReturnCode wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
  try
  {
    mStagedIR = std::make_unique<dsp::ImpulseResponse>(irPath.Get(), sampleRate);
    wavState = mStagedIR->GetWavState();
  }
  catch (std::runtime_error& e)
  {
    wavState = dsp::wav::LoadReturnCode::ERROR_OTHER;
    std::cerr << "Caught unhandled exception while attempting to load IR:" << std::endl;
    std::cerr << e.what() << std::endl;
  }

  if (wavState == dsp::wav::LoadReturnCode::SUCCESS)
  {
    mIRPath = irPath;
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadedIR, mIRPath.GetLength(), mIRPath.Get());
  }
  else
  {
    if (mStagedIR != nullptr)
    {
      mStagedIR = nullptr;
    }
    mIRPath = previousIRPath;
    SendControlMsgFromDelegate(kCtrlTagIRFileBrowser, kMsgTagLoadFailed);
  }

  return wavState;
}

size_t NeuralAmpModeler::_GetBufferNumChannels() const
{
  // Assumes input=output (no mono->stereo effects)
  return mInputArray.size();
}

size_t NeuralAmpModeler::_GetBufferNumFrames() const
{
  if (_GetBufferNumChannels() == 0)
    return 0;
  return mInputArray[0].size();
}

void NeuralAmpModeler::_InitToneStack()
{
  // If you want to customize the tone stack, then put it here!
  mToneStack = std::make_unique<dsp::tone_stack::BasicNamToneStack>();
}
void NeuralAmpModeler::_PrepareBuffers(const size_t numChannels, const size_t numFrames)
{
  const bool updateChannels = numChannels != _GetBufferNumChannels();
  const bool updateFrames = updateChannels || (_GetBufferNumFrames() != numFrames);
  //  if (!updateChannels && !updateFrames)  // Could we do this?
  //    return;

  if (updateChannels)
  {
    _PrepareIOPointers(numChannels);
    mInputArray.resize(numChannels);
    mOutputArray.resize(numChannels);
  }
  if (updateFrames)
  {
    for (auto c = 0; c < mInputArray.size(); c++)
    {
      mInputArray[c].resize(numFrames);
      std::fill(mInputArray[c].begin(), mInputArray[c].end(), 0.0);
    }
    for (auto c = 0; c < mOutputArray.size(); c++)
    {
      mOutputArray[c].resize(numFrames);
      std::fill(mOutputArray[c].begin(), mOutputArray[c].end(), 0.0);
    }
  }
  // Would these ever get changed by something?
  for (auto c = 0; c < mInputArray.size(); c++)
    mInputPointers[c] = mInputArray[c].data();
  for (auto c = 0; c < mOutputArray.size(); c++)
    mOutputPointers[c] = mOutputArray[c].data();
}

void NeuralAmpModeler::_PrepareIOPointers(const size_t numChannels)
{
  _DeallocateIOPointers();
  _AllocateIOPointers(numChannels);
}

void NeuralAmpModeler::_ProcessInput(iplug::sample** inputs, const size_t nFrames, const size_t nChansIn,
                                     const size_t nChansOut)
{
  // We'll assume that the main processing is mono for now. We'll handle dual amps later.
  if (nChansOut != 1)
  {
    std::stringstream ss;
    ss << "Expected mono output, but " << nChansOut << " output channels are requested!";
    throw std::runtime_error(ss.str());
  }

  // On the standalone, we can probably assume that the user has plugged into only one input and they expect it to be
  // carried straight through. Don't apply any division over nChansIn because we're just "catching anything out there."
  // However, in a DAW, it's probably something providing stereo, and we want to take the average in order to avoid
  // doubling the loudness. (This would change w/ double mono processing)
  double gain = mInputGain;
#ifndef APP_API
  gain /= (float)nChansIn;
#endif
  // Assume _PrepareBuffers() was already called
  for (size_t c = 0; c < nChansIn; c++)
    for (size_t s = 0; s < nFrames; s++)
      if (c == 0)
        mInputArray[0][s] = gain * inputs[c][s];
      else
        mInputArray[0][s] += gain * inputs[c][s];
}

void NeuralAmpModeler::_ProcessOutput(iplug::sample** inputs, iplug::sample** outputs, const size_t nFrames,
                                      const size_t nChansIn, const size_t nChansOut)
{
  const double gain = mOutputGain;
  // Assume _PrepareBuffers() was already called
  if (nChansIn != 1)
    throw std::runtime_error("Plugin is supposed to process in mono.");
  // Broadcast the internal mono stream to all output channels.
  const size_t cin = 0;
  for (auto cout = 0; cout < nChansOut; cout++)
    for (auto s = 0; s < nFrames; s++)
#ifdef APP_API // Ensure valid output to interface
      outputs[cout][s] = std::clamp(gain * inputs[cin][s], -1.0, 1.0);
#else // In a DAW, other things may come next and should be able to handle large
      // values.
      outputs[cout][s] = gain * inputs[cin][s];
#endif
}

void NeuralAmpModeler::_UpdateControlsFromModel()
{
  if (mModel == nullptr)
  {
    return;
  }
  if (auto* pGraphics = GetUI())
  {
    ModelInfo modelInfo;
    modelInfo.sampleRate.known = true;
    modelInfo.sampleRate.value = mModel->GetEncapsulatedSampleRate();
    modelInfo.inputCalibrationLevel.known = mModel->HasInputLevel();
    modelInfo.inputCalibrationLevel.value = mModel->HasInputLevel() ? mModel->GetInputLevel() : 0.0;
    modelInfo.outputCalibrationLevel.known = mModel->HasOutputLevel();
    modelInfo.outputCalibrationLevel.value = mModel->HasOutputLevel() ? mModel->GetOutputLevel() : 0.0;

    static_cast<NAMSettingsPageControl*>(pGraphics->GetControlWithTag(kCtrlTagSettingsBox))->SetModelInfo(modelInfo);

    const bool disableInputCalibrationControls = !mModel->HasInputLevel();
    pGraphics->GetControlWithTag(kCtrlTagCalibrateInput)->SetDisabled(disableInputCalibrationControls);
    pGraphics->GetControlWithTag(kCtrlTagInputCalibrationLevel)->SetDisabled(disableInputCalibrationControls);
    {
      auto* c = static_cast<OutputModeControl*>(pGraphics->GetControlWithTag(kCtrlTagOutputMode));
      c->SetNormalizedDisable(!mModel->HasLoudness());
      c->SetCalibratedDisable(!mModel->HasOutputLevel());
    }

    if (auto* pSlimIcon = pGraphics->GetControlWithTag(kCtrlTagSlimmableIcon))
    {
      const bool show = mModel->GetSlimmableModel() != nullptr;
      pSlimIcon->Hide(!show);
    }
  }
}

void NeuralAmpModeler::_UpdateLatency()
{
  int latency = 0;
  if (mModel)
  {
    latency += mModel->GetLatency();
  }
  // Other things that add latency here...

  // Feels weird to have to do this.
  if (GetLatency() != latency)
  {
    SetLatency(latency);
  }
}

void NeuralAmpModeler::_UpdateMeters(sample** inputPointer, sample** outputPointer, const size_t nFrames,
                                     const size_t nChansIn, const size_t nChansOut)
{
  // Right now, we didn't specify MAXNC when we initialized these, so it's 1.
  const int nChansHack = 1;
  mInputSender.ProcessBlock(inputPointer, (int)nFrames, kCtrlTagInputMeter, nChansHack);
  mOutputSender.ProcessBlock(outputPointer, (int)nFrames, kCtrlTagOutputMeter, nChansHack);
}

// HACK
#include "Unserialization.cpp"

// ---------------------------------------------------------------------------
// Korpi: favourites (.korpi files, JSON). Called from the UI thread.
// ---------------------------------------------------------------------------
namespace
{
// Settings that belong to a "sound". Metronome and calibration settings are left out on purpose.
const char* const kKorpiPresetParams[] = {"Input",    "Threshold",       "Juuret",   "Kaarna",     "Neulaset",
                                          "Salo",     "Käki",            "Output",   "NoiseGateActive",
                                          "ToneStack", "IRToggle",       "SaloType", "SaloActive", "KakiActive",
                                          "Slim"};

// If a saved path doesn't exist (e.g. a favourite shared by a friend), look for the same file name
// in Korpi's own folder.
std::string KorpiResolvePath(const std::string& savedPath, const char* korpiSubfolder)
{
  if (savedPath.empty())
    return savedPath;
  std::error_code ec;
  if (std::filesystem::exists(KorpiFsPath(savedPath), ec))
    return savedPath;
  const std::string folder = KorpiUserFolder(korpiSubfolder);
  if (folder.empty())
    return savedPath;
  const auto name = KorpiFsPath(savedPath).filename();
  const auto candidate = KorpiFsPath(folder) / name;
  if (std::filesystem::exists(candidate, ec))
  {
    const auto u8 = candidate.u8string();
    return std::string(u8.begin(), u8.end());
  }
  return savedPath;
}
} // namespace

bool NeuralAmpModeler::_KorpiSavePreset(const WDL_String& filePath)
{
  try
  {
    nlohmann::json j;
    j["korpi_preset_version"] = 1;
    j["NAMPath"] = std::string(mNAMPath.Get());
    j["IRPath"] = std::string(mIRPath.Get());
    nlohmann::json params = nlohmann::json::object();
    for (const char* name : kKorpiPresetParams)
    {
      for (int i = 0; i < kNumParams; i++)
      {
        if (strcmp(GetParam(i)->GetName(), name) == 0)
        {
          params[name] = GetParam(i)->Value();
          break;
        }
      }
    }
    j["params"] = params;
    std::ofstream out(KorpiFsPath(filePath.Get()), std::ios::binary | std::ios::trunc);
    if (!out)
      return false;
    out << j.dump(2);
    return (bool)out;
  }
  catch (...)
  {
    return false;
  }
}

bool NeuralAmpModeler::_KorpiLoadPreset(const WDL_String& filePath)
{
  nlohmann::json j;
  try
  {
    std::ifstream in(KorpiFsPath(filePath.Get()), std::ios::binary);
    if (!in)
      return false;
    in >> j;
  }
  catch (...)
  {
    return false;
  }
  if (!j.is_object())
    return false;

  // Knobs and switches
  if (j.contains("params") && j["params"].is_object())
  {
    for (auto it = j["params"].begin(); it != j["params"].end(); ++it)
    {
      if (!it->is_number())
        continue;
      // Favourites saved before the Metso -> Käki rename
      std::string key = it.key();
      if (key == "Metso")
        key = "Käki";
      else if (key == "MetsoActive")
        key = "KakiActive";
      for (int i = 0; i < kNumParams; i++)
      {
        if (key == GetParam(i)->GetName())
        {
          const double norm = GetParam(i)->ToNormalized(it->get<double>());
          BeginInformHostOfParamChangeFromUI(i);
          SendParameterValueFromUI(i, norm);
          EndInformHostOfParamChangeFromUI(i);
          SendParameterValueFromDelegate(i, norm, true);
          break;
        }
      }
    }
  }

  // Amp model and cabinet
  const std::string namPath = KorpiResolvePath(j.value("NAMPath", std::string()), "Soundit");
  const std::string irPath = KorpiResolvePath(j.value("IRPath", std::string()), "Kaapit");
  bool ok = true;
  if (!namPath.empty())
  {
    if (namPath != mNAMPath.Get())
      ok = _StageModel(WDL_String(namPath.c_str())).empty() && ok;
  }
  else if (mNAMPath.GetLength())
    mShouldRemoveModel = true;

  if (!irPath.empty())
  {
    if (irPath != mIRPath.Get())
      ok = (_StageIR(WDL_String(irPath.c_str())) == dsp::wav::LoadReturnCode::SUCCESS) && ok;
  }
  else if (mIRPath.GetLength())
    mShouldRemoveIR = true;

  if (!ok)
    _ShowMessageBox(GetUI(),
                    "Suosikki ladattiin, mutta soundia tai kaappia ei löytynyt. Tarkista, että tiedostot ovat "
                    "Korpi-kansioissa.",
                    "Korpi", kMB_OK);
  return true;
}
