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
};

} // namespace ml
