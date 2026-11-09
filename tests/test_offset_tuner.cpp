// In-headset offset tuning (offset_tuner.h): the chord, flicks, the grip modifier, reset, and that the
// game sees nothing while tuning.
#include "offset_tuner.h"

#include <doctest/doctest.h>

using ml::OffsetTuner;
using ml::TunerResult;

namespace {

struct Rig
{
	OffsetTuner tuner;
	ml::HandState hands[2] = {};
	float v[6] = {0.f, 0.f, -0.11f, 0.f, 0.f, 0.f};
	double t = 0.0;

	Rig()
	{
		hands[0].active = hands[1].active = true;
	}
	TunerResult
	tick(double dt = 0.01)
	{
		t += dt;
		return tuner.update(hands, t, v);
	}
	//! Hold both sticks clicked long enough, then release; returns whether it toggled.
	bool
	chord()
	{
		hands[0].stick_click = hands[1].stick_click = true;
		bool toggled = false;
		for (int i = 0; i < 120; i++) {
			const TunerResult r = tick();
			toggled |= r.entered || r.left;
		}
		hands[0].stick_click = hands[1].stick_click = false;
		CHECK(tick().suppress == tuner.active()); // the release itself is hidden only while/after toggling
		return toggled;
	}
	//! Flick a stick to `value` and back to the centre; returns the result of the flick tick.
	TunerResult
	flick(int hand, bool y, float value)
	{
		(y ? hands[hand].stick_y : hands[hand].stick_x) = value;
		const TunerResult r = tick();
		CHECK_FALSE(tick().changed); // holding it there doesn't repeat
		(y ? hands[hand].stick_y : hands[hand].stick_x) = 0.f;
		tick();
		return r;
	}
};

} // namespace

TEST_CASE("tuning: holding both sticks clicked for a second toggles it, once per hold")
{
	Rig rig;
	CHECK_FALSE(rig.tuner.active());
	rig.hands[0].stick_click = rig.hands[1].stick_click = true;
	CHECK_FALSE(rig.tick().entered); // the hold starts here
	CHECK_FALSE(rig.tick(0.9).entered);
	const TunerResult on = rig.tick(0.2); // 1.1 s
	CHECK(on.entered);
	CHECK(on.suppress);
	CHECK(rig.tuner.active());
	CHECK_FALSE(rig.tick(2.0).left); // keep holding: no toggle back

	rig.hands[0].stick_click = rig.hands[1].stick_click = false;
	rig.tick();
	CHECK(rig.chord());
	CHECK_FALSE(rig.tuner.active());
	CHECK_FALSE(rig.tick().suppress); // the game has its buttons back
}

TEST_CASE("tuning: one stick, or a short press, or an untracked hand doesn't toggle")
{
	Rig rig;
	rig.hands[1].stick_click = true;
	CHECK_FALSE(rig.tick(2.0).entered);
	rig.hands[0].stick_click = true;
	rig.hands[0].active = false;
	rig.tick();
	CHECK_FALSE(rig.tick(2.0).entered);
	rig.hands[0].active = true;
	rig.hands[0].stick_click = false;
	rig.tick();
	rig.hands[0].stick_click = true;
	rig.tick();
	CHECK_FALSE(rig.tick(0.5).entered);
	rig.hands[0].stick_click = false;
	rig.tick();
	rig.hands[0].stick_click = true;
	CHECK_FALSE(rig.tick(0.6).entered); // the timer restarted on release
	CHECK_FALSE(rig.tuner.active());
}

TEST_CASE("tuning: flicks nudge the offset one step each, only while active")
{
	Rig rig;
	CHECK_FALSE(rig.flick(1, true, 1.f).changed); // not tuning: the stick is the game's
	CHECK(rig.v[ml::k_offset_pitch] == 0.f);
	REQUIRE(rig.chord());

	TunerResult r = rig.flick(1, true, 1.f);
	CHECK(r.changed);
	CHECK(r.step_hand == 1);
	CHECK(rig.v[ml::k_offset_pitch] == doctest::Approx(1.f));
	rig.flick(1, true, 0.9f);
	CHECK(rig.v[ml::k_offset_pitch] == doctest::Approx(2.f));
	rig.flick(1, true, -1.f);
	CHECK(rig.v[ml::k_offset_pitch] == doctest::Approx(1.f));
	CHECK_FALSE(rig.flick(1, true, 0.5f).changed); // a nudge below the flick threshold
	rig.flick(1, false, 1.f);
	CHECK(rig.v[ml::k_offset_yaw] == doctest::Approx(1.f));

	r = rig.flick(0, true, 1.f); // left stick up = forward = -Z
	CHECK(r.step_hand == 0);
	CHECK(rig.v[ml::k_offset_z] == doctest::Approx(-0.115f));
	rig.flick(0, false, 1.f); // left stick right = the right hand moves right = -x (mirrored)
	CHECK(rig.v[ml::k_offset_x] == doctest::Approx(-0.005f));

	// Right grip held: height and roll instead of pitch and yaw.
	rig.hands[1].squeeze = 1.f;
	rig.flick(1, true, 1.f);
	rig.flick(1, false, -1.f);
	CHECK(rig.v[ml::k_offset_y] == doctest::Approx(0.005f));
	CHECK(rig.v[ml::k_offset_roll] == doctest::Approx(-1.f));
	CHECK(rig.v[ml::k_offset_pitch] == doctest::Approx(1.f));
	CHECK(rig.v[ml::k_offset_yaw] == doctest::Approx(1.f));
}

TEST_CASE("tuning: many steps don't drift, and A asks for the defaults once per press")
{
	Rig rig;
	REQUIRE(rig.chord());
	for (int i = 0; i < 37; i++) {
		rig.flick(0, true, -1.f);
	}
	CHECK(rig.v[ml::k_offset_z] == doctest::Approx(-0.11f + 37 * 0.005f).epsilon(1e-6));

	rig.hands[1].lower_click = true;
	CHECK(rig.tick().reset);
	CHECK_FALSE(rig.tick().reset);
	rig.hands[1].lower_click = false;
	rig.tick();
	rig.hands[1].lower_click = true;
	CHECK(rig.tick().reset);
}

TEST_CASE("tuning: a stick still deflected when tuning starts must come back to the centre first")
{
	Rig rig;
	rig.hands[1].stick_y = 1.f; // e.g. resting on it while clicking
	REQUIRE(rig.chord());
	CHECK_FALSE(rig.tick().changed);
	rig.hands[1].stick_y = 0.f;
	rig.tick();
	CHECK(rig.flick(1, true, 1.f).changed);
}

TEST_CASE("tuning: B asks for a stream reset once per press, only while active")
{
	Rig rig;
	rig.hands[1].upper_click = true;
	CHECK_FALSE(rig.tick().reset_stream); // not tuning: B is the game's
	rig.hands[1].upper_click = false;
	rig.tick();
	REQUIRE(rig.chord());
	rig.hands[1].upper_click = true;
	CHECK(rig.tick().reset_stream);
	CHECK_FALSE(rig.tick().reset_stream); // held
	rig.hands[1].upper_click = false;
	rig.tick();
	rig.hands[1].upper_click = true;
	CHECK(rig.tick().reset_stream);
}

TEST_CASE("tuning: X sets the floor and Y resets it, once per press, only while active")
{
	Rig rig;
	rig.hands[0].lower_click = rig.hands[0].upper_click = true;
	TunerResult r = rig.tick();
	CHECK_FALSE(r.floor_from_controller); // not tuning: X and Y are the game's
	CHECK_FALSE(r.floor_reset);
	rig.hands[0].lower_click = rig.hands[0].upper_click = false;
	rig.tick();
	REQUIRE(rig.chord());
	rig.hands[0].lower_click = true;
	r = rig.tick();
	CHECK(r.floor_from_controller);
	CHECK_FALSE(r.floor_reset);
	CHECK(r.suppress);
	CHECK_FALSE(rig.tick().floor_from_controller); // held
	rig.hands[0].lower_click = false;
	rig.hands[0].upper_click = true;
	r = rig.tick();
	CHECK(r.floor_reset);
	CHECK_FALSE(r.floor_from_controller);
	CHECK_FALSE(r.reset); // A (the offsets) is the right hand's
}

TEST_CASE("floor_adjust_from_controller: puts the controller on the floor, within 1 m")
{
	double adjust = 9.0;
	// The headset's floor is 12 cm too high: a controller on the real floor reads -0.08 m.
	REQUIRE(ml::floor_adjust_from_controller(0.0, -0.08, &adjust));
	CHECK(adjust == doctest::Approx(0.12));
	// Pressing again with it still there changes nothing.
	REQUIRE(ml::floor_adjust_from_controller(adjust, ml::k_controller_on_floor_m, &adjust));
	CHECK(adjust == doctest::Approx(0.12));
	// Floor too low: the controller reads 0.30 m.
	REQUIRE(ml::floor_adjust_from_controller(0.0, 0.30, &adjust));
	CHECK(adjust == doctest::Approx(-0.26));
	// A controller held at the waist (1 m up): refused, the old value stays.
	adjust = 0.05;
	CHECK_FALSE(ml::floor_adjust_from_controller(0.05, 1.2, &adjust));
	CHECK(adjust == 0.05);
	CHECK_FALSE(ml::floor_adjust_from_controller(0.0, std::nan(""), &adjust));
}
