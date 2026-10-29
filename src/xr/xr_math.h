// Conversions between OpenXR and OpenVR types. Both APIs use the same conventions (right-handed,
// +Y up, -Z forward, metres, quaternions), so poses map one to one.
#pragma once

#include <openvr_driver.h>
#include <openxr/openxr.h>

#include <cmath>

namespace ml {

//! A DriverPose_t from an OpenXR space location + velocity in our base space. `world_offset_y` lifts
//! the base space into SteamVR's standing universe (0 when the base space is STAGE, i.e. the floor).
inline vr::DriverPose_t
driver_pose_from_xr(const XrSpaceLocation &loc, const XrSpaceVelocity &vel, double world_offset_y)
{
	vr::DriverPose_t p{};
	p.qWorldFromDriverRotation.w = 1.0;
	p.vecWorldFromDriverTranslation[1] = world_offset_y;
	p.qDriverFromHeadRotation.w = 1.0;
	p.qRotation.w = 1.0;
	p.deviceIsConnected = true;

	const bool orientation_valid = (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
	const bool position_valid = (loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;

	if (orientation_valid) {
		p.qRotation.w = loc.pose.orientation.w;
		p.qRotation.x = loc.pose.orientation.x;
		p.qRotation.y = loc.pose.orientation.y;
		p.qRotation.z = loc.pose.orientation.z;
	}
	if (position_valid) {
		p.vecPosition[0] = loc.pose.position.x;
		p.vecPosition[1] = loc.pose.position.y;
		p.vecPosition[2] = loc.pose.position.z;
	}
	// Both APIs express velocities in the base space (OpenVR: axis-angle rad/s for angular).
	if ((vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0) {
		p.vecVelocity[0] = vel.linearVelocity.x;
		p.vecVelocity[1] = vel.linearVelocity.y;
		p.vecVelocity[2] = vel.linearVelocity.z;
	}
	if ((vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0) {
		p.vecAngularVelocity[0] = vel.angularVelocity.x;
		p.vecAngularVelocity[1] = vel.angularVelocity.y;
		p.vecAngularVelocity[2] = vel.angularVelocity.z;
	}

	p.poseIsValid = orientation_valid;
	if (!orientation_valid) {
		p.result = vr::TrackingResult_Running_OutOfRange;
	} else if (position_valid) {
		// Not POSITION_TRACKED: Monado doesn't set the TRACKED bits for the Quest even while it tracks
		// fine, and reporting Fallback_RotationOnly (300) for every pose was wrong.
		p.result = vr::TrackingResult_Running_OK;
	} else {
		p.result = vr::TrackingResult_Fallback_RotationOnly;
	}
	p.shouldApplyHeadModel = !position_valid;
	return p;
}

//! OpenVR raw projection tangents from an OpenXR FOV (angles in radians, up/right positive, so left
//! and down are negative).
//!
//! OpenVR's vertical naming is the confusing part: SteamVR builds the projection with
//! m[1][2] = (bottom + top) / (bottom - top), which matches a frustum reaching up by tan(up) and down
//! by tan(down) only if bottom = tan(angleUp) and top = tan(angleDown) (negative). ALVR does the same
//! (fov_to_tangents). Swapping them (as the first version did) flips the vertical asymmetry: SteamVR
//! then renders 50° up / 48° down for a 48°/50° Quest 2 eye, and the picture looks shifted/stretched.
inline void
projection_from_xr_fov(const XrFovf &fov, float *left, float *right, float *top, float *bottom)
{
	*left = std::tan(fov.angleLeft);
	*right = std::tan(fov.angleRight);
	*top = std::tan(fov.angleDown);
	*bottom = std::tan(fov.angleUp);
}

//! A 3x4 rigid transform (rotation | translation) from an OpenXR pose.
inline vr::HmdMatrix34_t
matrix_from_xr_pose(const XrPosef &pose)
{
	const float w = pose.orientation.w, x = pose.orientation.x, y = pose.orientation.y, z = pose.orientation.z;
	vr::HmdMatrix34_t m{};
	m.m[0][0] = 1.f - 2.f * (y * y + z * z);
	m.m[0][1] = 2.f * (x * y - w * z);
	m.m[0][2] = 2.f * (x * z + w * y);
	m.m[1][0] = 2.f * (x * y + w * z);
	m.m[1][1] = 1.f - 2.f * (x * x + z * z);
	m.m[1][2] = 2.f * (y * z - w * x);
	m.m[2][0] = 2.f * (x * z - w * y);
	m.m[2][1] = 2.f * (y * z + w * x);
	m.m[2][2] = 1.f - 2.f * (x * x + y * y);
	m.m[0][3] = pose.position.x;
	m.m[1][3] = pose.position.y;
	m.m[2][3] = pose.position.z;
	return m;
}

//! Hamilton product a*b (rotate by b, then by a).
inline XrQuaternionf
quat_mul(const XrQuaternionf &a, const XrQuaternionf &b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline XrVector3f
quat_rotate(const XrQuaternionf &q, const XrVector3f &v)
{
	// v' = v + 2w(u x v) + 2u x (u x v), u = (x, y, z)
	const XrVector3f u{q.x, q.y, q.z};
	const XrVector3f t{2.f * (u.y * v.z - u.z * v.y), 2.f * (u.z * v.x - u.x * v.z), 2.f * (u.x * v.y - u.y * v.x)};
	return {v.x + q.w * t.x + (u.y * t.z - u.z * t.y), v.y + q.w * t.y + (u.z * t.x - u.x * t.z),
	        v.z + q.w * t.z + (u.x * t.y - u.y * t.x)};
}

//! a ∘ b: the pose b (expressed in a's frame) in a's parent frame.
inline XrPosef
pose_compose(const XrPosef &a, const XrPosef &b)
{
	XrPosef r;
	r.orientation = quat_mul(a.orientation, b.orientation);
	const XrVector3f p = quat_rotate(a.orientation, b.position);
	r.position = {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z};
	return r;
}

//! The pose of a 3x4 rigid transform (its rotation must be orthonormal).
inline XrPosef
pose_from_matrix(const vr::HmdMatrix34_t &m)
{
	XrPosef p;
	const float trace = m.m[0][0] + m.m[1][1] + m.m[2][2];
	XrQuaternionf &q = p.orientation;
	if (trace > 0.f) {
		const float s = std::sqrt(trace + 1.f) * 2.f; // 4w
		q.w = 0.25f * s;
		q.x = (m.m[2][1] - m.m[1][2]) / s;
		q.y = (m.m[0][2] - m.m[2][0]) / s;
		q.z = (m.m[1][0] - m.m[0][1]) / s;
	} else if (m.m[0][0] > m.m[1][1] && m.m[0][0] > m.m[2][2]) {
		const float s = std::sqrt(1.f + m.m[0][0] - m.m[1][1] - m.m[2][2]) * 2.f; // 4x
		q.w = (m.m[2][1] - m.m[1][2]) / s;
		q.x = 0.25f * s;
		q.y = (m.m[0][1] + m.m[1][0]) / s;
		q.z = (m.m[0][2] + m.m[2][0]) / s;
	} else if (m.m[1][1] > m.m[2][2]) {
		const float s = std::sqrt(1.f + m.m[1][1] - m.m[0][0] - m.m[2][2]) * 2.f; // 4y
		q.w = (m.m[0][2] - m.m[2][0]) / s;
		q.x = (m.m[0][1] + m.m[1][0]) / s;
		q.y = 0.25f * s;
		q.z = (m.m[1][2] + m.m[2][1]) / s;
	} else {
		const float s = std::sqrt(1.f + m.m[2][2] - m.m[0][0] - m.m[1][1]) * 2.f; // 4z
		q.w = (m.m[1][0] - m.m[0][1]) / s;
		q.x = (m.m[0][2] + m.m[2][0]) / s;
		q.y = (m.m[1][2] + m.m[2][1]) / s;
		q.z = 0.25f * s;
	}
	const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
	q = {q.x / n, q.y / n, q.z / n, q.w / n};
	p.position = {m.m[0][3], m.m[1][3], m.m[2][3]};
	return p;
}

//! The eye poses, in our base space, of a layer SteamVR rendered with head pose `head_in_world`
//! (SteamVR's universe = base space lifted by `world_offset_y`). Giving Monado the pose the frame was
//! rendered with lets its compositor reproject to the latest pose.
inline void
layer_eye_poses(const vr::HmdMatrix34_t &head_in_world, double world_offset_y, const XrPosef eye_in_head[2],
                XrPosef out[2])
{
	XrPosef head = pose_from_matrix(head_in_world);
	head.position.y -= (float)world_offset_y;
	for (int eye = 0; eye < 2; eye++) {
		out[eye] = pose_compose(head, eye_in_head[eye]);
	}
}

//! The angle (degrees, 0..180) of the rotation between two orientations (q and -q are the same).
inline float
rotation_angle_deg(const XrQuaternionf &a, const XrQuaternionf &b)
{
	float d = std::fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
	d = d > 1.f ? 1.f : d;
	return 2.f * std::acos(d) * 180.f / (float)M_PI;
}

//! A controller pose offset from settings: position (m) and rotation (degrees, applied X then Y then
//! Z, intrinsic) for the left hand; the right hand gets the mirror image (x, yaw and roll negated),
//! like ALVR's controller offsets.
inline XrPosef
controller_offset(const float position[3], const float rotation_deg[3], bool right)
{
	const float d = (float)M_PI / 180.f;
	const float rx = rotation_deg[0] * d, ry = (right ? -1.f : 1.f) * rotation_deg[1] * d,
	            rz = (right ? -1.f : 1.f) * rotation_deg[2] * d;
	const XrQuaternionf qx{std::sin(rx / 2), 0.f, 0.f, std::cos(rx / 2)};
	const XrQuaternionf qy{0.f, std::sin(ry / 2), 0.f, std::cos(ry / 2)};
	const XrQuaternionf qz{0.f, 0.f, std::sin(rz / 2), std::cos(rz / 2)};
	XrPosef p;
	p.orientation = quat_mul(quat_mul(qx, qy), qz);
	p.position = {(right ? -1.f : 1.f) * position[0], position[1], position[2]};
	return p;
}

//! Distance between the two eyes, and whether it's a real IPD (50-80 mm). Monado has reported 0 when
//! the headset sent no IPD: both eyes at one point, no stereo, and hands looking huge up close.
inline bool
plausible_ipd(const XrPosef eyes[2], float *ipd_meters)
{
	const XrVector3f &l = eyes[0].position, &r = eyes[1].position;
	*ipd_meters = std::sqrt((r.x - l.x) * (r.x - l.x) + (r.y - l.y) * (r.y - l.y) + (r.z - l.z) * (r.z - l.z));
	return *ipd_meters >= 0.050f && *ipd_meters <= 0.080f;
}

//! A texel rectangle (x0, y0, x1, y1) from OpenVR texture bounds (u/v in 0..1, top-left origin;
//! min > max means flipped, which a blit with swapped offsets reproduces).
inline void
rect_from_bounds(const vr::VRTextureBounds_t &b, uint32_t width, uint32_t height, int32_t out[4])
{
	out[0] = (int32_t)std::lround(b.uMin * width);
	out[1] = (int32_t)std::lround(b.vMin * height);
	out[2] = (int32_t)std::lround(b.uMax * width);
	out[3] = (int32_t)std::lround(b.vMax * height);
}

} // namespace ml
