// In-headset controller offset tuning, with the controllers themselves: hold both thumbsticks
// clicked for a second to enter (or leave) tuning mode; then stick flicks nudge the offset, one step
// per flick, and the game sees no buttons until you leave. Every direction is as seen from the right
// hand (the left one mirrors it, see controller_offset()):
//
//   right stick up/down       pitch: aim up / down          1 deg
//   right stick left/right    yaw: aim left / right         1 deg
//   + right grip held:  up/down = raise / lower 5 mm, left/right = roll (twist) 1 deg
//   left stick up/down        forward / back                5 mm
//   left stick left/right     left / right                  5 mm
//   A                         back to the defaults
//
// Pure logic, no OpenVR: DeviceProvider feeds it the synced hands and applies what it returns.
#pragma once

#include "xr/xr_input.h"

#include <cmath>

namespace ml {

//! Offset values, as in DeviceProvider::offset_settings_.
enum OffsetIndex
{
	k_offset_x,
	k_offset_y,
	k_offset_z,
	k_offset_pitch,
	k_offset_yaw,
	k_offset_roll,
};

struct TunerResult
{
	bool suppress = false; //!< hide this tick's buttons and sticks from the game
	bool changed = false;  //!< values were nudged
	bool reset = false;    //!< A: restore the defaults
	bool entered = false, left = false;
	int step_hand = -1; //!< the hand whose flick made a step (for a haptic tick)
};

class OffsetTuner
{
public:
	static constexpr double k_hold_s = 1.0;     //!< chord duration to toggle
	static constexpr float k_flick_on = 0.7f;   //!< stick deflection that counts as a flick
	static constexpr float k_flick_off = 0.3f;  //!< back near the centre re-arms
	static constexpr float k_grip_modifier = 0.5f;
	static constexpr float k_step_m = 0.005f, k_step_deg = 1.f;

	bool
	active() const
	{
		return active_;
	}

	//! One tracking tick: `now_s` any monotonic clock in seconds; `values` (x, y, z, pitch, yaw, roll)
	//! is updated in place when the result says changed.
	TunerResult
	update(const HandState hands[2], double now_s, float values[6])
	{
		TunerResult r;
		const bool chord = hands[k_left].active && hands[k_right].active && hands[k_left].stick_click &&
		                   hands[k_right].stick_click;
		if (!chord) {
			chord_since_ = -1.0;
			chord_released_ = true;
		} else if (chord_released_) {
			if (chord_since_ < 0.0) {
				chord_since_ = now_s;
			} else if (now_s - chord_since_ >= k_hold_s) {
				active_ = !active_;
				(active_ ? r.entered : r.left) = true;
				chord_released_ = false; // one toggle per hold
				for (auto &hand : armed_) {
					hand[0] = hand[1] = false; // sticks must come back to the centre first
				}
				reset_armed_ = false;
			}
		}
		// Also hide the chord's release after leaving, so the game doesn't get stray stick clicks.
		r.suppress = active_ || !chord_released_;
		if (!active_) {
			return r;
		}

		const HandState &right = hands[k_right], &left = hands[k_left];
		const bool modifier = right.squeeze > k_grip_modifier;
		int dir;
		if ((dir = flick(k_right, 1, right)) != 0) {
			modifier ? nudge(values, k_offset_y, dir * k_step_m) : nudge(values, k_offset_pitch, dir * k_step_deg);
			r.step_hand = k_right;
		}
		if ((dir = flick(k_right, 0, right)) != 0) {
			// +yaw/+roll turn the right hand to its right (mirrored from the left hand's convention)
			modifier ? nudge(values, k_offset_roll, dir * k_step_deg) : nudge(values, k_offset_yaw, dir * k_step_deg);
			r.step_hand = k_right;
		}
		if ((dir = flick(k_left, 1, left)) != 0) {
			nudge(values, k_offset_z, -dir * k_step_m); // up = forward = -Z
			r.step_hand = k_left;
		}
		if ((dir = flick(k_left, 0, left)) != 0) {
			nudge(values, k_offset_x, -dir * k_step_m); // the right hand's +x is mirrored: to its left
			r.step_hand = k_left;
		}
		r.changed = r.step_hand >= 0;

		if (!right.lower_click) {
			reset_armed_ = true;
		} else if (reset_armed_) {
			reset_armed_ = false;
			r.reset = true;
		}
		return r;
	}

private:
	//! +1 / -1 on a flick of `hand`'s stick along `axis` (0 = x, 1 = y), once per flick.
	int
	flick(int hand, int axis, const HandState &s)
	{
		const float v = axis == 0 ? s.stick_x : s.stick_y;
		bool &armed = armed_[hand][axis];
		if (std::fabs(v) < k_flick_off) {
			armed = true;
			return 0;
		}
		if (!armed || std::fabs(v) < k_flick_on || s.stick_click) {
			return 0;
		}
		armed = false;
		return v > 0.f ? 1 : -1;
	}

	static void
	nudge(float values[6], int index, float delta)
	{
		// Rounded so many steps don't drift (0.1 mm / 0.01 deg).
		values[index] = std::round((values[index] + delta) * 10000.f) / 10000.f;
	}

	bool active_ = false;
	double chord_since_ = -1.0;
	bool chord_released_ = true;
	bool armed_[2][2] = {};
	bool reset_armed_ = false;
};

} // namespace ml
