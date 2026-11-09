// SPDX-License-Identifier: MIT
// FSR 1 constants (AMD's FsrEasuCon / FsrRcasCon from external/FidelityFX-FSR, run on the CPU) for
// the push constants of fsr_easu.comp and fsr_rcas.comp.
#pragma once

#include <cstdint>

namespace ml {

//! EASU from the in_w x in_h region (top left) of an image of image_w x image_h to out_w x out_h:
//! con0..con3, 16 words.
void
fsr_easu_constants(uint32_t out[16], float in_w, float in_h, float image_w, float image_h, float out_w, float out_h);
//! RCAS with `sharpness` in stops: 0 = sharpest, each +1 halves it (AMD's sample uses 0.2).
void
fsr_rcas_constants(uint32_t out[4], float sharpness);

} // namespace ml
