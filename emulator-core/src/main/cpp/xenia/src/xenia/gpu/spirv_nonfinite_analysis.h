/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 *                                                                            *
 * Adapted from XDroid360 (TheCafeGuy/XDroid360, commit f5c2ef55f,            *
 * BSD-3-Clause): translation-time static fixpoint +/-Inf/NaN taint analysis  *
 * for Shader Model 3 multiplication ("0 * anything = 0").                    *
 ******************************************************************************
 */

#ifndef XENIA_GPU_SPIRV_NONFINITE_ANALYSIS_H_
#define XENIA_GPU_SPIRV_NONFINITE_ANALYSIS_H_

#include <cstdint>
#include <vector>

namespace xe {
namespace gpu {

class Shader;

// Analyzes which multiplication operands in |shader| may hold +-Inf or NaN.
// Populates |out| indexed by guest ALU instruction address (ucode_dwords / 3):
//   bit 0: vector product operand 0 may be non-finite
//   bit 1: vector product operand 1 may be non-finite
//   bit 2: scalar product operand 0 may be non-finite
//   bit 3: scalar product operand 1 may be non-finite
// Where both operands of a product are proven finite (bits == 0), native IEEE
// OpFMul can be emitted without the Shader Model 3 zero-select; where only one
// operand may be non-finite, only the finite operand needs to be tested for 0.
void AnalyzeShaderNonFiniteOperands(const Shader& shader,
                                    std::vector<uint8_t>& out);

}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_SPIRV_NONFINITE_ANALYSIS_H_
