// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Reiner Prokein
#pragma once
// Custom instruments (Drum Engine > Custom tab): extra percussion that a track can be assigned to (Add Track dialog). They are available
// in every engine mode and the engine mode never redirects them. A custom instrument is one definition (CustomInstrumentDef) that names
// its own implementation (kind) and carries its own knobs and defaults; the set it belongs to ("TR-808", "TR-909") is a label, not a class.
// The definitions live with their implementations (808CustomInstruments.h, 909CustomInstruments.h); CustomInstrumentRegistry.h lists them.
#include "InstrumentSchema.h"
#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#else
#include "json.hpp"   // the plugin build has iPlug2's Extras/nlohmann on its include path
#endif

namespace dgg {

// The implementations a custom instrument can run on; the host keeps the voice pool of each.
enum class CustomKind { Conga, Claves, HandClap, Maracas, Cowbell, Pc2, Tr909 };

struct CustomInstrumentDef
{
  const char* id;          // registry id (track assignment)
  const char* name;        // Drum Engine card label
  const char* origin;      // the instrument set it belongs to: label only
  CustomKind kind;
  int arg;                 // engine argument of the kind (Conga: tom sheet; Tr909: the Tr909Voice)
  MixerChannel channel;
  const ControlDef* controls; int numControls;
  const DefaultValue* defaults; int numDefaults;   // includes "volume": the default of the card's Volume knob (linear gain after the voice, 1.0 = unity)
  nlohmann::json (*engineJson)();                  // the "engine" object of the UI row
};

inline double DefaultOf(const CustomInstrumentDef& d, const char* key, double fallback = 0.0) { return DefaultOfValues(d.defaults, d.numDefaults, key, fallback); }

// The row of a custom instrument in the shape the UI consumes (the fields of the kit rows of the instrument table); slot = its index in the registry.
inline nlohmann::json CustomInstrumentToJson(const CustomInstrumentDef& d, int slot)
{
  nlohmann::json j;
  j["id"] = d.id;
  j["name"] = d.name;
  j["origin"] = d.origin;
  j["trackName"] = nullptr;
  j["family"] = "custom";
  j["paramIndex"] = slot;
  j["uiFamily"] = "custom";
  j["gm"] = nullptr;
  j["mixerChannel"] = MixerChannelName(d.channel);
  j["poolSlot"] = slot;
  j["engine"] = d.engineJson();
  nlohmann::json defaults = nlohmann::json::object();
  for (int i = 0; i < d.numDefaults; ++i) defaults[d.defaults[i].key] = d.defaults[i].value;
  j["defaults"] = defaults;
  nlohmann::json controls = nlohmann::json::array();
  for (int i = 0; i < d.numControls; ++i)
  {
    const ControlDef& c = d.controls[i];
    controls.push_back({ {"group", c.group}, {"key", c.key}, {"label", c.label}, {"tooltip", c.tooltip},
                         {"min", c.min}, {"max", c.max}, {"step", c.step},
                         {"default", defaults.contains(c.key) ? defaults[c.key] : nlohmann::json(nullptr)} });
  }
  j["controls"] = controls;
  return j;
}

}  // namespace dgg
