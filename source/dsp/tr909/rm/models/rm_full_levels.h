// Level set of the full RM model (gen_full_set.py): one model per grid level of the output potentiometer (R 17-18 = level x 100 kOhm). Generated, do not edit.
#pragma once
#include "../RmLevelHost.h"
#include "rm_full_lv0.h"
#include "rm_full_lv1.h"
#include "rm_full_lv2.h"
#include "rm_full_lv3.h"
#include "rm_full_lv4.h"
#include "rm_full_lv5.h"
#include "rm_full_lv6.h"
#include "rm_full_lv7.h"
#include "rm_full_lv8.h"
#include "rm_full_lv9.h"
#include "rm_full_lv10.h"
#include "rm_full_lv11.h"
#include "rm_full_lv12.h"
#include "rm_full_lv13.h"
#include "rm_full_lv14.h"
#include "rm_full_lv15.h"
#include "rm_full_lv16.h"
#include "rm_full_lv17.h"
#include "rm_full_lv18.h"
#include "rm_full_lv19.h"
#include "rm_full_lv20.h"
#include "rm_full_lv21.h"
#include "rm_level_states.h"
struct RmFullLevels {
  static constexpr int kN = 22;
  static constexpr int kR = RmFull_0::kR, kNP = RmFull_0::kNP, kNF = RmFull_0::kNF;   // identical for all levels (state transfer between level models)
  static constexpr int kLevN = kLevStates;   // output coupling capacitors whose charge a change of the level does not scale (level_states.py)
  static const double* LevW() { return kLevW; }
  static const double* LevD() { return kLevD; }
  static const double* LevA() { return kLevA; }
  static constexpr double kLv[kN] = {0.01, 0.025, 0.05, 0.1, 0.15, 0.2, 0.25, 0.3, 0.35, 0.4, 0.45, 0.5, 0.55, 0.6, 0.65, 0.7, 0.75, 0.8, 0.85, 0.9, 0.95, 1.0};
  static dgg::rmfast::FullModelBase* Make(int k)
  {
    switch (k)
    {
      case 0: return new dgg::rmfast::FullModelImpl<RmFull_0>;
      case 1: return new dgg::rmfast::FullModelImpl<RmFull_1>;
      case 2: return new dgg::rmfast::FullModelImpl<RmFull_2>;
      case 3: return new dgg::rmfast::FullModelImpl<RmFull_3>;
      case 4: return new dgg::rmfast::FullModelImpl<RmFull_4>;
      case 5: return new dgg::rmfast::FullModelImpl<RmFull_5>;
      case 6: return new dgg::rmfast::FullModelImpl<RmFull_6>;
      case 7: return new dgg::rmfast::FullModelImpl<RmFull_7>;
      case 8: return new dgg::rmfast::FullModelImpl<RmFull_8>;
      case 9: return new dgg::rmfast::FullModelImpl<RmFull_9>;
      case 10: return new dgg::rmfast::FullModelImpl<RmFull_10>;
      case 11: return new dgg::rmfast::FullModelImpl<RmFull_11>;
      case 12: return new dgg::rmfast::FullModelImpl<RmFull_12>;
      case 13: return new dgg::rmfast::FullModelImpl<RmFull_13>;
      case 14: return new dgg::rmfast::FullModelImpl<RmFull_14>;
      case 15: return new dgg::rmfast::FullModelImpl<RmFull_15>;
      case 16: return new dgg::rmfast::FullModelImpl<RmFull_16>;
      case 17: return new dgg::rmfast::FullModelImpl<RmFull_17>;
      case 18: return new dgg::rmfast::FullModelImpl<RmFull_18>;
      case 19: return new dgg::rmfast::FullModelImpl<RmFull_19>;
      case 20: return new dgg::rmfast::FullModelImpl<RmFull_20>;
      case 21: return new dgg::rmfast::FullModelImpl<RmFull_21>;
      default: return nullptr;
    }
  }
};
