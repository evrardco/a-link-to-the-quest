// OpenXR -> OpenVR conversions (poses, FOV, eye transforms).
#include "xr/xr_math.h"

#include <doctest/doctest.h>

#include <cmath>

static const XrSpaceLocationFlags k_all_valid =
    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
    XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;

TEST_CASE("a fully tracked location maps 1:1 with velocities")
{
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	loc.locationFlags = k_all_valid;
	const float s = std::sqrt(0.5f);
	loc.pose = {{0.f, s, 0.f, s}, {0.1f, 1.7f, -0.2f}}; // 90 degrees yaw
	XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
	vel.velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
	vel.linearVelocity = {1.f, 2.f, 3.f};
	vel.angularVelocity = {0.f, 0.5f, 0.f};

	vr::DriverPose_t p = ml::driver_pose_from_xr(loc, vel, 0.0);
	CHECK(p.poseIsValid);
	CHECK(p.deviceIsConnected);
	CHECK(p.result == vr::TrackingResult_Running_OK);
	CHECK_FALSE(p.shouldApplyHeadModel);
	CHECK(p.qRotation.w == doctest::Approx(s));
	CHECK(p.qRotation.x == doctest::Approx(0.0));
	CHECK(p.qRotation.y == doctest::Approx(s));
	CHECK(p.qRotation.z == doctest::Approx(0.0));
	CHECK(p.vecPosition[0] == doctest::Approx(0.1));
	CHECK(p.vecPosition[1] == doctest::Approx(1.7));
	CHECK(p.vecPosition[2] == doctest::Approx(-0.2));
	CHECK(p.vecVelocity[2] == doctest::Approx(3.0));
	CHECK(p.vecAngularVelocity[1] == doctest::Approx(0.5));
	CHECK(p.qWorldFromDriverRotation.w == 1.0);
	CHECK(p.qDriverFromHeadRotation.w == 1.0);
	CHECK(p.vecWorldFromDriverTranslation[1] == 0.0);
}

TEST_CASE("the world offset lifts a LOCAL base space to standing height")
{
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	loc.locationFlags = k_all_valid;
	loc.pose.orientation.w = 1.f;
	XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
	CHECK(ml::driver_pose_from_xr(loc, vel, 1.6).vecWorldFromDriverTranslation[1] == doctest::Approx(1.6));
}

TEST_CASE("rotation-only tracking is a valid pose that falls back to the head model")
{
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	loc.locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	loc.pose = {{0.f, 0.f, 0.f, 1.f}, {9.f, 9.f, 9.f}}; // position is garbage when not valid
	XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
	vel.linearVelocity = {5.f, 5.f, 5.f}; // flags not set: ignored

	vr::DriverPose_t p = ml::driver_pose_from_xr(loc, vel, 0.0);
	CHECK(p.poseIsValid);
	CHECK(p.result == vr::TrackingResult_Fallback_RotationOnly);
	CHECK(p.shouldApplyHeadModel);
	CHECK(p.vecPosition[0] == 0.0);
	CHECK(p.vecVelocity[0] == 0.0);
}

TEST_CASE("no valid orientation is an invalid pose with an identity rotation")
{
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	loc.locationFlags = 0;
	loc.pose = {{0.f, 0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}}; // a zero quaternion must not reach SteamVR
	XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};

	vr::DriverPose_t p = ml::driver_pose_from_xr(loc, vel, 0.0);
	CHECK_FALSE(p.poseIsValid);
	CHECK(p.result == vr::TrackingResult_Running_OutOfRange);
	CHECK(p.qRotation.w == 1.0);
}

TEST_CASE("FOV angles become OpenVR raw tangents: top = tan(down edge), bottom = tan(up edge)")
{
	XrFovf fov{-0.9f, 0.7f, 0.8f, -0.85f}; // left, right, up, down
	float l, r, t, b;
	ml::projection_from_xr_fov(fov, &l, &r, &t, &b);
	CHECK(l == doctest::Approx(std::tan(-0.9f)));
	CHECK(r == doctest::Approx(std::tan(0.7f)));
	CHECK(t == doctest::Approx(std::tan(-0.85f)));
	CHECK(b == doctest::Approx(std::tan(0.8f)));
}

TEST_CASE("SteamVR's projection from our raw values equals OpenXR's for the same FOV")
{
	// An asymmetric Quest 2-like eye: 52° out, 45° in, 48° up, 50° down.
	const float d = (float)M_PI / 180.f;
	XrFovf fov{-52.f * d, 45.f * d, 48.f * d, -50.f * d};
	float l, r, t, b;
	ml::projection_from_xr_fov(fov, &l, &r, &t, &b);

	// OpenVR's ComposeProjection (openvr wiki, IVRSystem::GetProjectionRaw): what SteamVR renders with.
	const float vr_m00 = 2.f / (r - l), vr_m02 = (r + l) / (r - l);
	const float vr_m11 = 2.f / (b - t), vr_m12 = (b + t) / (b - t);

	// OpenXR's XrMatrix4x4f_CreateProjectionFov (GL-style, +Y up): what the layer FOV means to Monado.
	const float tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight);
	const float tu = std::tan(fov.angleUp), td = std::tan(fov.angleDown);
	const float xr_m00 = 2.f / (tr - tl), xr_m02 = (tr + tl) / (tr - tl);
	const float xr_m11 = 2.f / (tu - td), xr_m12 = (tu + td) / (tu - td);

	CHECK(vr_m00 == doctest::Approx(xr_m00));
	CHECK(vr_m02 == doctest::Approx(xr_m02));
	CHECK(vr_m11 == doctest::Approx(xr_m11));
	CHECK(vr_m12 == doctest::Approx(xr_m12)); // the term the first version got backwards
	CHECK(xr_m12 < 0.f);                      // more down than up: the centre sits above the middle
}

TEST_CASE("pose to 3x4 matrix: identity, translation and a 90 degree yaw")
{
	XrPosef identity{{0.f, 0.f, 0.f, 1.f}, {-0.032f, 0.f, 0.01f}};
	vr::HmdMatrix34_t m = ml::matrix_from_xr_pose(identity);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			CHECK(m.m[i][j] == doctest::Approx(i == j ? 1.f : 0.f));
		}
	}
	CHECK(m.m[0][3] == doctest::Approx(-0.032f));
	CHECK(m.m[2][3] == doctest::Approx(0.01f));

	// Yaw +90 degrees about +Y turns -Z (forward) into -X (left).
	const float s = std::sqrt(0.5f);
	XrPosef yaw{{0.f, s, 0.f, s}, {0.f, 0.f, 0.f}};
	m = ml::matrix_from_xr_pose(yaw);
	const float fwd[3] = {0.f, 0.f, -1.f};
	float out[3];
	for (int i = 0; i < 3; i++) {
		out[i] = m.m[i][0] * fwd[0] + m.m[i][1] * fwd[1] + m.m[i][2] * fwd[2];
	}
	CHECK(out[0] == doctest::Approx(-1.f));
	CHECK(out[1] == doctest::Approx(0.f).epsilon(1e-6));
	CHECK(out[2] == doctest::Approx(0.f).epsilon(1e-6));
}

// --- Layer poses and bounds (M2) -------------------------------------------------------------------

TEST_CASE("pose_from_matrix inverts matrix_from_xr_pose, including a 180 degree turn")
{
	const float s = std::sqrt(0.5f);
	for (XrQuaternionf q : {XrQuaternionf{0.f, 0.f, 0.f, 1.f}, XrQuaternionf{0.f, s, 0.f, s},
	                        XrQuaternionf{0.f, 1.f, 0.f, 0.f}, XrQuaternionf{1.f, 0.f, 0.f, 0.f},
	                        XrQuaternionf{0.f, 0.f, 1.f, 0.f}, XrQuaternionf{0.1f, 0.7f, -0.1f, 0.7f}}) {
		const float n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		q = {q.x / n, q.y / n, q.z / n, q.w / n};
		XrPosef in{q, {0.3f, 1.6f, -0.4f}};
		XrPosef out = ml::pose_from_matrix(ml::matrix_from_xr_pose(in));
		// q and -q are the same rotation
		const float sign = (out.orientation.w * q.w + out.orientation.x * q.x + out.orientation.y * q.y +
		                    out.orientation.z * q.z) < 0.f ? -1.f : 1.f;
		CHECK(sign * out.orientation.x == doctest::Approx(q.x).epsilon(1e-4));
		CHECK(sign * out.orientation.y == doctest::Approx(q.y).epsilon(1e-4));
		CHECK(sign * out.orientation.z == doctest::Approx(q.z).epsilon(1e-4));
		CHECK(sign * out.orientation.w == doctest::Approx(q.w).epsilon(1e-4));
		CHECK(out.position.y == doctest::Approx(1.6f));
	}
}

TEST_CASE("eye poses: the eye offset turns with the head and the world offset is removed")
{
	// Head at standing height in SteamVR's universe, turned 90 degrees left (yaw +90 about +Y).
	const float s = std::sqrt(0.5f);
	XrPosef head{{0.f, s, 0.f, s}, {1.f, 1.7f, 2.f}};
	vr::HmdMatrix34_t head_world = ml::matrix_from_xr_pose(head);
	const XrPosef eyes[2] = {{{0, 0, 0, 1}, {-0.032f, 0, 0}}, {{0, 0, 0, 1}, {0.032f, 0, 0}}};

	XrPosef out[2];
	ml::layer_eye_poses(head_world, 0.0, eyes, out);
	// Yaw +90 maps the head's +X (right) to world -Z, so the left eye (-X) ends up at +Z.
	CHECK(out[0].position.x == doctest::Approx(1.f));
	CHECK(out[0].position.z == doctest::Approx(2.f + 0.032f));
	CHECK(out[1].position.z == doctest::Approx(2.f - 0.032f));
	CHECK(out[0].position.y == doctest::Approx(1.7f));
	CHECK(out[0].orientation.y == doctest::Approx(s));

	ml::layer_eye_poses(head_world, 1.6, eyes, out); // LOCAL base space: SteamVR's world is 1.6 m higher
	CHECK(out[0].position.y == doctest::Approx(0.1f));
}

TEST_CASE("texture bounds become texel rectangles, flips included")
{
	int32_t r[4];
	ml::rect_from_bounds({0.f, 0.f, 1.f, 1.f}, 2260, 2364, r);
	CHECK(r[0] == 0);
	CHECK(r[1] == 0);
	CHECK(r[2] == 2260);
	CHECK(r[3] == 2364);
	ml::rect_from_bounds({0.5f, 0.f, 1.f, 1.f}, 4520, 2364, r); // right half of a side-by-side texture
	CHECK(r[0] == 2260);
	CHECK(r[2] == 4520);
	ml::rect_from_bounds({0.f, 1.f, 1.f, 0.f}, 100, 200, r); // vertically flipped
	CHECK(r[1] == 200);
	CHECK(r[3] == 0);
}

TEST_CASE("rotation_angle_deg: identical, q vs -q, and a 90 degree yaw")
{
	const float s = std::sqrt(0.5f);
	XrQuaternionf id{0.f, 0.f, 0.f, 1.f}, yaw90{0.f, s, 0.f, s}, neg{0.f, 0.f, 0.f, -1.f};
	CHECK(ml::rotation_angle_deg(id, id) == doctest::Approx(0.f).epsilon(1e-3));
	CHECK(ml::rotation_angle_deg(id, neg) == doctest::Approx(0.f).epsilon(1e-3));
	CHECK(ml::rotation_angle_deg(id, yaw90) == doctest::Approx(90.f).epsilon(1e-3));
}

TEST_CASE("a valid position without the TRACKED bit is still Running_OK (Monado's Quest poses)")
{
	XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
	loc.locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
	loc.pose = {{0.f, 0.f, 0.f, 1.f}, {0.f, 1.6f, 0.f}};
	XrSpaceVelocity vel{XR_TYPE_SPACE_VELOCITY};
	vr::DriverPose_t p = ml::driver_pose_from_xr(loc, vel, 0.0);
	CHECK(p.result == vr::TrackingResult_Running_OK);
	CHECK((int)p.result == 200);
}
