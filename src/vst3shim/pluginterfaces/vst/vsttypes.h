#pragma once

// Stand-in for the VST3 SDK's pluginterfaces/vst/vsttypes.h, providing just
// the type aliases the pd submodule's DSP core (pd/eg/voice/const) actually
// uses. This lets those sources compile completely unmodified without the
// VST3 SDK: rtsynth puts src/vst3shim/ on the include path, so the pd
// files' `#include "pluginterfaces/vst/vsttypes.h"` resolves here instead.
// When the same sources are built inside the real plugin, the real SDK
// header is found and this file is never seen.
//
// Keep this header minimal on purpose — if a pd update starts using more of
// the SDK than plain typedefs, that usage belongs behind its own interface,
// not in an ever-growing shim.

#include <cstdint>

namespace Steinberg {

using int8 = int8_t;
using uint8 = uint8_t;
using int16 = int16_t;
using uint16 = uint16_t;
using int32 = int32_t;
using uint32 = uint32_t;
using int64 = int64_t;
using uint64 = uint64_t;
using tresult = int32_t;

namespace Vst {

using ParamValue = double;
using ParamID = uint32_t;
using Sample32 = float;
using Sample64 = double;
using SampleRate = double;

}  // namespace Vst
}  // namespace Steinberg
