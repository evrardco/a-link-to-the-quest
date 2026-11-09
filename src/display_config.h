// SPDX-License-Identifier: MIT
// The HMD's display as SteamVR sees it: defaults from the settings, overridden by what Monado reports.
#pragma once

#include <openvr_driver.h>

#include <cstdint>

namespace ml {

struct DisplayConfig
{
	uint32_t render_width;  //!< per eye
	uint32_t render_height; //!< per eye
	float refresh_rate;
	float seconds_from_vsync_to_photons;
	//! Per-eye projection as tangents in OpenVR's raw convention: left = tan(left edge) < 0, right > 0,
	//! and vertically *top = tan(down edge) < 0, bottom = tan(up edge) > 0* (see projection_from_xr_fov).
	float tan_left[2], tan_right[2], tan_top[2], tan_bottom[2];
	float ipd_meters;
	//! Eye poses relative to the head; only passed to SteamVR when known (from Monado).
	bool has_eye_to_head;
	vr::HmdMatrix34_t eye_to_head[2];
	//! FSR (setting fsr_render_scale): render_width/height were scaled by this from Monado's size and
	//! monadolink upscales back. 1 = rendered at Monado's size.
	float fsr_scale = 1.f;
	float fsr_sharpness = 0.2f; //!< RCAS, in stops (setting fsr_sharpness)
};

//! Setting fsr_render_scale: shrink the render size SteamVR is told (and games render at) by
//! `scale` (clamped to 0.5..1; 0 or >= 0.99 means off), keeping it even. Returns the scale applied.
inline float
apply_render_scale(DisplayConfig *c, float scale)
{
	if (!(scale > 0.f) || scale >= 0.99f) {
		c->fsr_scale = 1.f;
		return 1.f;
	}
	scale = scale < 0.5f ? 0.5f : scale;
	auto shrink = [&](uint32_t v) {
		uint32_t s = (uint32_t)((float)v * scale + 0.5f);
		return s & ~1u;
	};
	c->render_width = shrink(c->render_width);
	c->render_height = shrink(c->render_height);
	c->fsr_scale = scale;
	return scale;
}

} // namespace ml
