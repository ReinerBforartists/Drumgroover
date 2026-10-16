// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once

#include "IPlug_include_in_plug_hdr.h"

#include "dsp/GrooveGenerator.h"
#include "dsp/MidiExportService.h"
#include "dsp/Scheduler.h"
#include "dsp/PreviewScheduler.h"
#include "dsp/Types.h"
#include "dsp/808KickVoice.h"
#include <mutex>
#include "dsp/808SnareVoice.h"
#include "dsp/808HiHatVoice.h"
#include "dsp/808TomVoice.h"
#include "dsp/808InstrumentTable.h"
#include "dsp/808RmVoice.h"
#include "dsp/808HcVoice.h"
#include "dsp/808CbVoice.h"
#include "dsp/808Pc2Voice.h"
// Cymbal plays through 808RideVoice.h's TR808RideVoice (see its
// Articulation::Crash).
#include "dsp/808RideVoice.h"
#include "dsp/LoFiVoice.h"
#include "dsp/Mixer.h"
#include "dsp/VoicePool.h"
#include <array>
#include <atomic>
#include <map>
#include <set>
#include <string>

using namespace iplug;
using namespace igraphics;

// These IParams are for host automation and preset recall; the WebView UI uses separate JS‑side controls.
enum EParams
{
  kKickDensity = 0,
  kSnareDensity,
  kSnareGhostDensity,
  kNumParams
};

// Global routing switch: Internal -> native 808 voices, LoFi -> placeholder synthesis (see LoFiVoice.h).
// Affects all families; it is a routing choice, not a per‑family sound selection.
enum class EngineMode : int { Internal = 0, LoFi = 1 };

namespace dgg
{
// One cache set that a bake order covers. kind: 0=ride, 1=hihat, 2=cymbal, 3=(unused: the kick is synthesised directly), 4=snare, 5=tom/conga, 6=rim shot, 7=claves, 8=hand clap, 9=maracas, 10=(unused: the cowbell is synthesised directly), 11=pc2.
struct VariantBakeTarget { int kind; TR808VariantKey128 key; std::string baseName; };
}

class DrumGroover final : public Plugin
{
public:
  DrumGroover(const InstanceInfo& info);
  // The last instance to be destroyed stops the cache baker's worker threads.
  ~DrumGroover() override;

#if IPLUG_DSP
  void ProcessBlock(sample** inputs, sample** outputs, int nFrames) override;

  // Triggers the corresponding voice on MIDI Note‑On, independent of WebView UI.
  // Note mapping follows DemoTracks().
  void ProcessMidiMsg(const IMidiMsg& msg) override;

  // Overrides OnActivate to stop the Scheduler when the host deactivates the plugin.
  void OnActivate(bool active) override;

  // Hands the host's sample rate to every voice pool.
  void OnReset() override;

  // Overrides to save/load full plugin state (beyond IParams) via host project:
  // includes cached project JSON, voice params, gains, polyphony, engine mode, and mute.
  // UI navigation state is not saved.
  bool SerializeState(IByteChunk& chunk) const override;
  int UnserializeState(const IByteChunk& chunk, int startPos) override;
#endif

#if IPLUG_EDITOR
  // Handles bridge messages from the WebView UI.
  bool OnMessage(int msgTag, int ctrlTag, int dataSize, const void* pData) override;

  void OnWebContentLoaded() override;

  // Overrides to apply aspect‑lock and CSS transform on resize.
  void OnParentWindowResize(int width, int height) override;

  // Never forwards SPACE to the base delegate, which would re-post it into the WebView2 control: the injected keydown listener sends it back as SKPFUI, which re-enters OnKeyDown in an endless loop that starves the host UI thread. A host-routed SPACE (plugin window focused, WebView not) is replayed as a DOM keydown so the UI's own play/stop handler runs; a SPACE that already came from the DOM is left alone.
  bool OnKeyDown(const IKeyPress& key) override;

  // Marks key events that originate from the WebView's DOM so OnKeyDown can tell them from host-routed ones.
  void OnMessageFromWebView(const char* jsonStr) override;

  // Overrides to clean up resize timer window before editor closes.
  void CloseWindow() override;

  // Locks editor resize to 16:9 aspect ratio (PLUG_WIDTH/PLUG_HEIGHT).
  bool ConstrainEditorResize(int& w, int& h) const override;

  // Sends peak levels for all families via 'levelMeter' bridge message.
  void OnIdle() override;
#endif


private:
#if IPLUG_EDITOR
  // Flag to distinguish plugin‑initiated resize from host drag resize.
  bool mAwaitingSelfInitiatedResize = false;

  // True while a message from the WebView is being dispatched (UI thread only).
  bool mInWebViewMessage = false;

  // Hidden window for deferred resize requests to avoid recursion during host drag;
  // stores pending size; coalescing flag.
  void* mResizeTimerWnd = nullptr;
  int mPendingDragW = 0, mPendingDragH = 0;
  bool mDragResizePosted = false;

  void EnsureResizeTimerWindow();
public:
  // Called by the window proc to re‑request pending drag resize.
  void OnDragResizeTimerFired();
private:

  // Handlers for bridge messages; payload is parsed JSON.
  void HandleExportSong(const nlohmann::json& payload);
  void HandleExportBlock(const nlohmann::json& payload);
  void HandleExportBlocks(const nlohmann::json& payload);
  // Exports MIDI via Win32 OLE drag‑drop.
  void HandleStartNativeDragExportBlock(const nlohmann::json& payload);
  // Multi‑selection variant: exports combined MIDI for all selected clips.
  void HandleStartNativeDragExportSelection(const nlohmann::json& payload);
#ifdef OS_WIN
  // Runs OLE DoDragDrop for a file; returns true if dropped. Caller manages file deletion.
  bool RunNativeFileExportDrag(const std::string& path, const char* logPrefix);
#endif
  void HandleSaveProject(const nlohmann::json& payload);

  // Saves the How-To as a standalone HTML file chosen in a save dialog; payload: { html, suggestedFilename }.
  void HandleExportHowTo(const nlohmann::json& payload);
  void HandleRequestLoadProject();

  // Updates the host‑sync scheduler with fullPattern JSON from JS; loopStart/End optional.
  void HandleSyncPlaybackPattern(const nlohmann::json& payload);

  // Handles internal preview play/stop; uses PreviewScheduler with host tempo.
  // Pattern JSON same as sync scheduler.
  void HandlePreviewPlay(const nlohmann::json& payload);
  void HandlePreviewStop(const nlohmann::json& payload);

  // Regenerates kick group data from block and knob values; returns updated tracks.
  void HandleRegenerateKickGroup(const nlohmann::json& payload);

  // Regenerates snare group data (including ghosts and rim variation).
  void HandleRegenerateSnareGroup(const nlohmann::json& payload);

  // Regenerates only the ghost layer for snare without rerolling main pattern.
  void HandleRegenerateSnareGhostLayer(const nlohmann::json& payload);

  // Shared dispatcher for pulse‑based groups (Hihat, Ride, Toms, Cymbals).
  void HandlePulseGroupRegen(const nlohmann::json& payload, const char* responseType,
                              const std::vector<std::string>& trackIds,
                              void (*applyFn)(dgg::Block&, const dgg::Grid&, double, double, int,
                                               dgg::Feel, int, dgg::TurnaroundFillCache&));
  void HandleRegenerateHihatGroup(const nlohmann::json& payload);
  void HandleRegenerateRideGroup(const nlohmann::json& payload);
  void HandleRegenerateTomsGroup(const nlohmann::json& payload);
  void HandleRegenerateCymbalsGroup(const nlohmann::json& payload);

  // Regenerates a custom instrument track (single articulation) from block and knob values.
  void HandleRegenerateCustomGroup(const nlohmann::json& payload);

  // Handlers for regenerating break/fill blocks. They take additional parent block info from JS;
  // may send RegenerateFailed if invalid.
  void HandleRegenerateBreakBlock(const nlohmann::json& payload);
  void HandleRegenerateBreakMain(const nlohmann::json& payload);
  void HandleRegenerateBreakKick(const nlohmann::json& payload);
  void HandleRegenerateFillBlock(const nlohmann::json& payload);

  // Caches project state from JS in C++ so it survives editor WebView recreation.
  void HandleCacheState(const nlohmann::json& payload);

  // Handles MIDI note on/off from UI keyboard; sends to engine via SendMidiMsgFromUI(). Velocity fixed.
  void HandleMidiNoteOn(const nlohmann::json& payload);
  void HandleMidiNoteOff(const nlohmann::json& payload);

  // Resizes plugin window by scaling PLUG_WIDTH/HEIGHT; uses physical window resize, not CSS zoom.
  void HandleSetUIScale(const nlohmann::json& payload);

  // Sets polyphony for given family (tom sets all positions, ride sets all articulations).
  // Hi‑hat and cymbal have no slider.
  void HandleSetPolyphony(const nlohmann::json& payload);

  // Sets voice parameter for a family; index specifies instance for per‑instance parameters
  // (e.g., Tom position, Cymbal tone).
  void HandleSetVoiceParam(const nlohmann::json& payload);

  // Fired once a knob's drag ends (see drum-engine-tab.js deSendVoiceParamCommit).
  // For Ride/HiHat/Crash: enqueues a lazy background bake of fresh render-
  // cache variants (see 808VariantPools.h/808VariantPoolBaker.h) for the
  // instrument's current parameter combination. No-op for other families.
  void HandleCommitVoiceParam(const nlohmann::json& payload);

  // Sets the target variant count (1-20) for the Ride/HiHat/Crash render-
  // cache pools -- see drum-engine-tab.js, Cache panel.
  // Only stores the value (also used when a project is loaded); it never
  // bakes anything.
  void HandleSetCacheVariantCount(const nlohmann::json& payload);

  // Cache panel "Apply…": payload.value is the pending variant count.
  // HandleQueryCacheBakeEstimate() replies "cacheBakeEstimate" with how many
  // sample files applying it would bake (default sets + the current knob
  // values of every Ride/HiHat/Cymbal pool), so the UI can warn first;
  // HandleApplyCacheVariantCount() stores the value and starts baking the
  // missing ones.
  void HandleQueryCacheBakeEstimate(const nlohmann::json& payload);
  void HandleApplyCacheVariantCount(const nlohmann::json& payload);
  void HandleCancelCacheBake(const nlohmann::json& payload);

  // Cache size limit (machine-wide, Cache panel): SendCacheInfo() reports
  // { usedBytes, files, limitGiB, rebakeOnChange }; HandleSetCacheLimit()
  // stores payload.gib and evicts down to it; HandleSetRebakeOnChange()
  // stores payload.on.
  void SendCacheInfo();
  void HandleSetCacheLimit(const nlohmann::json& payload);
  void HandleSetRebakeOnChange(const nlohmann::json& payload);
  // Per-instrument cache switch: payload { instrument: "hihat" | "ride" (Ride and Cymbal), disabled } turns that instrument's cache off
  // (every hit is synthesised live, nothing is baked).
  void HandleSetCacheDisabled(const nlohmann::json& payload);
  void ApplyCacheMode(bool hiHatOff, bool rideOff);
  std::vector<dgg::VariantBakeTarget> CurrentBakeTargets();
  void PollApplyJob();

  // "Clear Cache" actions (Cache panel, drum-engine-tab.js), payload.tier:
  // "cache" deletes every baked set that is not a factory-default set and
  // re-enqueues every Ride/HiHat/Cymbal pool at its current knob values;
  // "default" deletes the factory-default sets (and default.index) and bakes
  // them again from the UI's default tables. See DrumGroover.cpp.
  void HandleClearCache(const nlohmann::json& payload);

  // Sent once by the UI at startup: writes default.index and bakes the factory-default sets (ApplyFactoryDefaults()). The defaults are
  // the instrument table's (dsp/808InstrumentTable.h) -- not the live voices' values -- so the payload is empty.
  void HandleSetFactoryDefaults(const nlohmann::json& payload);

  // Writes default.index from the instrument table's defaults and bakes whichever default-set variants are missing, on a background thread.
  void ApplyFactoryDefaults();

  // Bake whichever variants of the set for pool `poolIndex`'s CURRENT knob
  // values are missing from the cache folder, and load the set into the
  // in-memory pool (see DrumGroover.cpp).
  // rebakeAll: bake a completely new set even if one exists (Cache panel
  // "Rebake at value change").
  void EnqueueRideVariantBake(int poolIndex, bool rebakeAll = false);
  void EnqueueHiHatVariantBake(int poolIndex, bool rebakeAll = false);
  void EnqueueCrashVariantBake(int poolIndex, bool rebakeAll = false);

  // Scans `fp` for which Ride/HiHat/Cymbal pools are actually used (via
  // mTracks' trackId->gm lookup + NoteToVariantPoolTargets(), see
  // DrumGroover.cpp), and enqueues a bake for exactly those -- not every
  // pool unconditionally. Called whenever the played pattern changes
  // (HandleSyncPlaybackPattern).
  void RebakeRequiredVariantPools(const dgg::FullPattern& fp);

  // Stores pad cursor position for later save/restore.
  void HandleSetPadPos(const nlohmann::json& payload);

  // Sets per‑note volume gain applied at trigger time (multiplied into velocity).
  void HandleSetArticulationLevel(const nlohmann::json& payload);

  // Sets global engine mode (Internal/LoFi) from JS.
  void HandleSetEngineMode(const nlohmann::json& payload);

  // Sets user mute state (independent of host activate).
  void HandleSetMuted(const nlohmann::json& payload);

  // Options menu "Developer tools": enables the WebView's right-click Inspect / DevTools.
  void HandleSetDevTools(const nlohmann::json& payload);

  // Sets the full mixer state (15 channels + master + reverb/delay buses,
  // incl. FX slots) from JS — see dsp/Mixer.h and resources/web/mixer-tab.js.
  void HandleMixerState(const nlohmann::json& payload);
  void ApplyDefaultMixerForEngine(int engineMode);   // the mixer state a freshly selected engine starts with
  void SendMixerPresetList();
  void HandleMixerPresetList(const nlohmann::json& payload);
  void HandleMixerPresetSave(const nlohmann::json& payload);
  void HandleMixerPresetLoad(const nlohmann::json& payload);
  void HandleMixerPresetDelete(const nlohmann::json& payload);
  void HandleMixerPresetRename(const nlohmann::json& payload);
  void HandleMixerPresetDefault(const nlohmann::json& payload);

  // Sends a bridge message to JS via SendJSONFromDelegate.
  void SendBridgeMessage(const char* type, const nlohmann::json& payload);

  // Updates WebView bounds to match host window client size; used for plugin‑initiated resize.
  void UpdateWebViewBoundsForHostWindow();

  // Tracks max JS string length; grows as needed.
  size_t mMaxJSBufferSize = 8 * 1024 * 1024;

  // Cached project JSON from JS; survives editor recreation.
  std::string mCachedProjectJson;

  // Stores pad cursor positions (x,y) and synced flag per family:index.
  struct PadPos { double x = 0.5, y = 0.5; bool synced = true; };
  std::map<std::string, PadPos> mPadPositions;

  // Per‑instance ID for localStorage namespacing.
  std::string mInstanceId;
#endif

  // Flag set by UnserializeState to request UI state refresh on the UI thread (via OnIdle).
  std::atomic<bool> mPendingStateRefresh{false};

  // Guards mCachedProjectJson and mPadPositions: the UI thread writes them, while the host may call SerializeState/UnserializeState
  // from its own thread.
  mutable std::mutex mStateMutex;

  std::vector<dgg::Track> mTracks;
  dgg::MidiExportService  mMidiExport;

  // Scheduler for host‑sync playback; thread‑safe for cross‑thread use.
  dgg::Scheduler mScheduler;

  // PreviewScheduler for internal preview playback; thread‑safe.
  dgg::PreviewScheduler mPreviewScheduler;

  // Loop range beats; -1.0 means no range.
  std::atomic<double> mLoopStartBeat{ -1.0 };
  std::atomic<double> mLoopEndBeat{ -1.0 };

#if IPLUG_DSP
  // One pool per kick instance (dsp/808InstrumentTable.h, family "kick": High Stack, Open); max polyphony 4, runtime polyphony
  // adjustable via HandleSetPolyphony. Pool index = poolSlot = paramIndex.
  std::array<dgg::VoicePool<dgg::TR808KickVoice, 4>, dgg::CountInstruments("kick")> mKickPools;
  // Calls f(instrumentRow, pool) for every kick instance, in table order.
  template <typename F> void ForEachKickPool(F&& f);
  template <typename F> void ForEachKickPool(F&& f) const;

  // One pool per snare instance (dsp/808InstrumentTable.h, family "snare"): the instances of kind SnareSd (Center, Off Center, Edge,
  // Sidestick) play the SD circuit, those of kind RmRim (Rimshot, Rim Only) the Rim Shot circuit (RM sheet, TR808RmVoice). The
  // instance's poolSlot is its index in the array of its kind.
  std::array<dgg::VoicePool<dgg::TR808SnareVoice, 8>, dgg::CountInstruments("snare", dgg::EngineKind::SnareSd)> mSnareSdPools;
  std::array<dgg::VoicePool<dgg::TR808RmVoice, 6>, dgg::CountInstruments("snare", dgg::EngineKind::RmRim)> mSnareRmPools;
  // Calls f(instrumentRow, pool) for every snare instance, in table order.
  template <typename F> void ForEachSnarePool(F&& f);
  template <typename F> void ForEachSnarePool(F&& f) const;
  void TriggerInstrument(const dgg::InstrumentDef& inst, double velocity01, double gain = 1.0);

  // One voice pool per Hi‑Hat instance (dsp/808InstrumentTable.h, family "hihat": Closed Edge, Tight Edge, Open Edge 0/4, Closed Tip,
  // Tight Tip); pool index = poolSlot = paramIndex. Choke: a hit of the Closed circuit chokes the pools of the Open circuit.
  std::array<dgg::VoicePool<dgg::HiHat808Voice, 8>, dgg::CountInstruments("hihat")> mHiHatPools;
  template <typename F> void ForEachHiHatPool(F&& f);
  template <typename F> void ForEachHiHatPool(F&& f) const;

  // Five voice pools for Tom positions (Hi, Rack2, Mid, Floor1, Low). Shared polyphony slider.
  // One pool per tom articulation, in the order of the Note Editor: Rack Tom 1 (HT, tom position) Center/Rimshot/Rim Only (notes 48/82/81),
  // Rack Tom 2 (MT) 47/80/79, Rack Tom 3 (LT) 45/78/77, Floor Tom 1 (MC, conga position) 43/75/74, Floor Tom 2 (LC) 41/73/72.
  // The HT conga is the Custom-tab conga (no pool yet).
  std::array<dgg::VoicePool<dgg::TR808TomVoice, 6>, dgg::CountInstruments("tom")> mTomPools;
  // Calls f(instrumentRow, pool) for every tom instance, in table order (pool index = poolSlot = paramIndex).
  template <typename F> void ForEachTomPool(F&& f);
  template <typename F> void ForEachTomPool(F&& f) const;

  // Custom instruments (Drum Engine > Custom tab, rows of family "custom" in dsp/808InstrumentTable.h): one pool each, played into the Custom channel.
  // The track a custom instrument is assigned to (Add Track dialog) gives its GM note and track id (setCustomInstruments).
  std::array<dgg::VoicePool<dgg::TR808TomVoice, 6>, dgg::kNumCustomInstruments> mCustomPools;      // the instruments of kind Tom
  std::array<dgg::VoicePool<dgg::TR808RmVoice, 6>, dgg::kNumCustomInstruments> mCustomRmPools;     // the instruments of kind RmClaves (RM sheet); the pool of the other kind stays idle
  std::array<dgg::VoicePool<dgg::TR808HcVoice, 6>, dgg::kNumCustomInstruments> mCustomHcPools;     // the instruments of kind HcClap / HcMaracas (HC sheet); the pools of the other kinds stay idle
  std::array<dgg::VoicePool<dgg::TR808Pc2Voice, 6>, dgg::kNumCustomInstruments> mCustomPc2Pools;   // the instrument of kind Pc2 (PC-2 sheet); the pools of the other kinds stay idle
  std::array<dgg::VoicePool<dgg::TR808CbVoice, 6>, dgg::kNumCustomInstruments> mCustomCbPools;     // the instrument of kind Cowbell (CB sheet); the pools of the other kinds stay idle
  std::atomic<int> mCustomNote[dgg::kNumCustomInstruments];   // GM note of the assigned track, -1: none
  std::atomic<double> mCustomPreviewGain[dgg::kNumCustomInstruments];   // volume knob of a card without a track: scales the preview velocity (unity = 1)
  void HandleSetCustomPreviewGain(const nlohmann::json& payload);
  std::mutex mCustomMutex;
  std::string mCustomTrackId[dgg::kNumCustomInstruments];     // its track id (guarded by mCustomMutex)
  void HandleSetCustomInstruments(const nlohmann::json& payload);
  void HandleCustomPreview(const nlohmann::json& payload);   // UI preview of a custom instrument: MIDI note-on on kCustomPreviewMidiChannel, note = registry index
  int CustomInstrumentForTrack(const std::string& trackId);   // audio thread (try-lock): index or -1
  void TriggerCustom(int k, double velocity01, double gain = 1.0);

  // One voice pool per Cymbal instance (dsp/808InstrumentTable.h, family "cymbal": Crash and Mute of each of the five cymbals, pool
  // index = poolSlot = paramIndex, even = Crash, odd = Mute); a Mute hit triggers ChokeAll on the Crash pool of its cymbal.
  // Cymbal plays through the same engine as Ride (see dsp/808RideVoice.h Articulation::Crash).
  std::array<dgg::VoicePool<dgg::TR808RideVoice, 3>, dgg::CountInstruments("cymbal")> mCymbalPools;
  template <typename F> void ForEachCymbalPool(F&& f);
  template <typename F> void ForEachCymbalPool(F&& f) const;

  // Six voice pools for Ride articulations (BowTip, BowShank, BellTip, BellShank, Edge, Mute). Mute chokes others.
  std::array<dgg::VoicePool<dgg::TR808RideVoice, 3>, dgg::CountInstruments("ride")> mRidePools;
  template <typename F> void ForEachRidePool(F&& f);
  template <typename F> void ForEachRidePool(F&& f) const;

  // Per‑note gain multipliers applied at trigger time. Separate for Internal and LoFi engines. Default 1.0.
  std::atomic<double> mArticulationGain[128];
  std::atomic<double> mArticulationGainLoFi[128];

  // Peak level accumulators per family for VU meter, read by OnIdle.
  std::atomic<double> mFocusPeak[7] = { 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 };

  // Global engine mode (Internal=0, LoFi=1).
  std::atomic<int> mEngineMode{0};

  // Active flag set by host OnActivate; if false, ProcessBlock skips processing.
  std::atomic<bool> mActive{true};

  // User‑controlled mute flag, independent of host activation.
  std::atomic<bool> mUserMuted{false};

  // Flag indicating voice parameters have been configured; prevents overwriting with defaults.
  std::atomic<bool> mVoiceParamsConfigured{false};





  // Hits that found no baked variant, written on the audio thread, serviced in OnIdle() by DrainCacheMisses():
  // Ride/HiHat/Cymbal by GM note (keys carry no velocity: the pool's current key is loaded or baked).
  std::atomic<uint64_t> mMissNoteBits[2]{{0}, {0}};
  void DrainCacheMisses();
  void TriggerGmNoteVoices(int note, double velocity01);

  // Target variant count (1-20) for the Ride/HiHat/Crash render-cache
  // pools (see 808VariantPoolCache.h) -- part of the persisted project
  // state (SerializeState()), NOT the baked audio itself (that lives in
  // %APPDATA%\DrumGroover\Cache\..., see GetCacheDir()).
  std::atomic<int> mCacheVariantCount{7};

  // Offline render (bounce/export): audio thread, called at the start of
  // every block. On the first offline block it loads every pool's set for
  // its current knob values (queueing whatever is missing), and on every
  // offline block it waits until the baker is idle -- so the render never
  // plays a live-synthesised hit for a set that is merely not loaded
  // yet or still baking, and sounds like playback. Blocking is fine here,
  // an offline render is not real time.
  void WaitForCacheBeforeOfflineRender();
  bool mOfflineCachePrepared = false;

  // Voice pools for Lo‑Fi engine, one per family.
  std::array<dgg::VoicePool<dgg::LoFiVoice, 4>, 6> mLoFiPools;

  // --- Mixer (see dsp/Mixer.h). --------------------------------------------
  // Full mixer state; UI thread writes via HandleMixerState, ProcessBlock
  // snapshots it under the lock at block start.
  mutable std::mutex mMixerMutex;
  dgg::MixerState mMixerState;

  // Mixer DSP module state — audio thread only (ProcessBlock).
  dgg::MixerProcessor mMixerProc;

  // Per-block peak accumulators for the mixer strip meters (L/R), read by
  // OnIdle() in the 'mixerLevels' bridge message. Same CAS pattern as mFocusPeak.
  std::atomic<double> mMixerPeakL[dgg::kMixerNumChannels] = {};
  std::atomic<double> mMixerPeakR[dgg::kMixerNumChannels] = {};
  std::atomic<double> mMixerMasterPeakL{0.0}, mMixerMasterPeakR{0.0};
  std::atomic<double> mMixerRevPeakL{0.0},    mMixerRevPeakR{0.0};
  std::atomic<double> mMixerDelPeakL{0.0},    mMixerDelPeakR{0.0};
  // Peak of the signal ENTERING a Lim slot (pre-limiting), per channel + master.
  // Read by OnIdle() in 'mixerLevels' so the UI can show how far above its
  // ceiling a limiter's input ran (i.e. whether it engaged). Same CAS pattern.
  std::atomic<double> mMixerLimInL[dgg::kMixerNumChannels] = {};
  std::atomic<double> mMixerLimInR[dgg::kMixerNumChannels] = {};
  std::atomic<double> mMixerMasterLimInL{0.0}, mMixerMasterLimInR{0.0};
  // Peak of the signal LEAVING a Lim slot (post-limiting), same layout.
  std::atomic<double> mMixerLimOutL[dgg::kMixerNumChannels] = {};
  std::atomic<double> mMixerLimOutR[dgg::kMixerNumChannels] = {};
  std::atomic<double> mMixerMasterLimOutL{0.0}, mMixerMasterLimOutR{0.0};
  // Deepest limiter gain reduction (1 - gain) since the last read, same layout.
  std::atomic<double> mMixerLimGrL[dgg::kMixerNumChannels] = {};
  std::atomic<double> mMixerLimGrR[dgg::kMixerNumChannels] = {};
  std::atomic<double> mMixerMasterLimGrL{0.0}, mMixerMasterLimGrR{0.0};
  // Loudest audible channel input after Kit Gain (Kit Gain strip meter). Same CAS pattern.
  std::atomic<double> mMixerKitInL{0.0}, mMixerKitInR{0.0};

  // Shared dispatch for triggering a GM note; handles internal or LoFi engine routing based on mEngineMode.
  void TriggerGmNote(int note, double velocity01);

  // Mixer state as JSON (bridge push + host project save). Locks mMixerMutex.
  nlohmann::json BuildMixerStateJson() const;

  // Helpers to build/apply voice params and articulation gains JSON for serialization.
  nlohmann::json BuildVoiceParamsJson() const;
  nlohmann::json BuildArticulationGainsJson(bool loFi) const;
  void ApplyVoiceParamsJson(const nlohmann::json& voiceParams);
  void ApplyArticulationGainsJson(const nlohmann::json& gains, bool loFi);

  // Pushes engine and project state to UI.
  void PushEngineAndProjectStateToUI();
#endif
};