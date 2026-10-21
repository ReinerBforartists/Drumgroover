// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#define PLUG_NAME "DrumGroover"
#define PLUG_MFR "Reiner Prokein"
#define PLUG_VERSION_HEX 0x00000801
#define PLUG_VERSION_STR "0.8.1"
#define PLUG_UNIQUE_ID 'Dgg1'
#define PLUG_MFR_ID 'Rprk'
#define PLUG_URL_STR "https://reinerprokein.com"
#define PLUG_EMAIL_STR ""
#define PLUG_COPYRIGHT_STR "Copyright 2026 Reiner Prokein"
#define PLUG_CLASS_NAME DrumGroover

#define BUNDLE_NAME "DrumGroover"
#define BUNDLE_MFR "ReinerProkein"
#define BUNDLE_DOMAIN "com"

#define PLUG_CHANNEL_IO "0-2"
#define SHARED_RESOURCES_SUBPATH "DrumGroover"

#define PLUG_LATENCY 0
#define PLUG_TYPE 1
#define PLUG_DOES_MIDI_IN 1
#define PLUG_DOES_MIDI_OUT 0
#define PLUG_DOES_MPE 0
// Required: without state chunks the host never calls SerializeState/UnserializeState and only saves the 3 EParams,
// so a project would lose its blocks, song and all Drum Engine / mixer settings.
#define PLUG_DOES_STATE_CHUNKS 1
#define PLUG_HAS_UI 1
#define PLUG_WIDTH 1440
#define PLUG_HEIGHT 810
// UI Scale (Options menus, 75%-200%) needs a resize range wider than the default size. Without these limits iPlug2
// sets min == max == PLUG_WIDTH/HEIGHT and the VST3 host silently clamps every EditorResizeFromUI() request back to
// that size. The maximum fits the largest UI Scale option (200% = 2880x1620).
#define PLUG_MIN_WIDTH 720
#define PLUG_MAX_WIDTH 2880
#define PLUG_MIN_HEIGHT 405
#define PLUG_MAX_HEIGHT 1620
#define PLUG_FPS 60
#define PLUG_SHARED_RESOURCES 0
#define PLUG_HOST_RESIZE 1

#define VST3_SUBCATEGORY "Instrument|Drum"
