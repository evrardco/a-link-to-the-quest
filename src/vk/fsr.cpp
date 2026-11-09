// SPDX-License-Identifier: MIT
#include "vk/fsr.h"

// AMD's headers, CPU half: the same code the shaders run sets up their constants. Its own translation
// unit: ffx_a.h defines many short macros. It expects the C math functions to be declared already.
#include <math.h>
#include <stdlib.h>
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#define A_CPU 1
#include "ffx_a.h"
#include "ffx_fsr1.h"
#pragma GCC diagnostic pop

namespace ml {

void
fsr_easu_constants(uint32_t out[16], float in_w, float in_h, float image_w, float image_h, float out_w, float out_h)
{
	FsrEasuCon(out, out + 4, out + 8, out + 12, in_w, in_h, image_w, image_h, out_w, out_h);
}

void
fsr_rcas_constants(uint32_t out[4], float sharpness)
{
	FsrRcasCon(out, sharpness);
}

} // namespace ml
