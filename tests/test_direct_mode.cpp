// DirectModeComponent swap texture bookkeeping against a fake IVRIPCResourceManagerClient.
#include "direct/direct_mode.h"
#include "fake_openvr.h"

#include <doctest/doctest.h>

#include <chrono>

using Desc = vr::IVRDriverDirectModeComponent::SwapTextureSetDesc_t;
using Set = vr::IVRDriverDirectModeComponent::SwapTextureSet_t;

static constexpr uint32_t k_vk_format_r8g8b8a8_srgb = 43;

TEST_CASE("CreateSwapTextureSet allocates three renderable shared images")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 0);

	Desc desc{1832, 1920, k_vk_format_r8g8b8a8_srgb, 1};
	Set set{};
	dm.CreateSwapTextureSet(42, &desc, &set);

	REQUIRE(ctx.ipc.images.size() == 3);
	for (auto handle : set.rSharedTextureHandles) {
		REQUIRE(ctx.ipc.images.count(handle) == 1);
		const auto &img = ctx.ipc.images[handle];
		CHECK(img.format == k_vk_format_r8g8b8a8_srgb);
		CHECK(img.width == 1832);
		CHECK(img.height == 1920);
		CHECK(img.renderable);
	}
	CHECK(set.rSharedTextureHandles[0] != set.rSharedTextureHandles[1]);
	CHECK(set.rSharedTextureHandles[1] != set.rSharedTextureHandles[2]);
}

TEST_CASE("a failed allocation releases what was already allocated")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.ipc.fail_after = 2; // third image fails
	ml::DirectModeComponent dm(120.f, 0);

	Desc desc{100, 100, k_vk_format_r8g8b8a8_srgb, 1};
	Set set{};
	dm.CreateSwapTextureSet(1, &desc, &set);

	CHECK(ctx.ipc.images.empty());
	CHECK(ctx.ipc.unrefs.size() == 2);
	CHECK(set.rSharedTextureHandles[0] == 0);
}

TEST_CASE("GetNextSwapTextureSetIndex cycles through the three images per set")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 0);

	Desc desc{64, 64, k_vk_format_r8g8b8a8_srgb, 1};
	Set left{}, right{};
	dm.CreateSwapTextureSet(7, &desc, &left);
	dm.CreateSwapTextureSet(7, &desc, &right);

	vr::SharedTextureHandle_t handles[2] = {left.rSharedTextureHandles[0], right.rSharedTextureHandles[0]};
	uint32_t indices[2] = {99, 99};
	const uint32_t expected[] = {1, 2, 0, 1};
	for (uint32_t e : expected) {
		dm.GetNextSwapTextureSetIndex(handles, &indices);
		CHECK(indices[0] == e);
		CHECK(indices[1] == e);
	}
}

TEST_CASE("one set shared by both eyes only advances once per call")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 0);

	Desc desc{64, 64, k_vk_format_r8g8b8a8_srgb, 1};
	Set set{};
	dm.CreateSwapTextureSet(7, &desc, &set);

	vr::SharedTextureHandle_t handles[2] = {set.rSharedTextureHandles[0], set.rSharedTextureHandles[0]};
	uint32_t indices[2] = {};
	dm.GetNextSwapTextureSetIndex(handles, &indices);
	CHECK(indices[0] == 1);
	CHECK(indices[1] == 1);
	dm.GetNextSwapTextureSetIndex(handles, &indices);
	CHECK(indices[0] == 2);
}

TEST_CASE("unknown handles get index 0")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 0);

	vr::SharedTextureHandle_t handles[2] = {12345, 54321};
	uint32_t indices[2] = {7, 7};
	dm.GetNextSwapTextureSetIndex(handles, &indices);
	CHECK(indices[0] == 0);
	CHECK(indices[1] == 0);
}

TEST_CASE("DestroySwapTextureSet frees the whole set from any of its handles")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 0);

	Desc desc{64, 64, k_vk_format_r8g8b8a8_srgb, 1};
	Set a{}, b{};
	dm.CreateSwapTextureSet(1, &desc, &a);
	dm.CreateSwapTextureSet(1, &desc, &b);
	REQUIRE(ctx.ipc.images.size() == 6);

	dm.DestroySwapTextureSet(a.rSharedTextureHandles[2]);
	CHECK(ctx.ipc.images.size() == 3);
	for (auto h : b.rSharedTextureHandles) {
		CHECK(ctx.ipc.images.count(h) == 1); // the other set is untouched
	}

	dm.DestroySwapTextureSet(999999); // unknown: no-op
	CHECK(ctx.ipc.images.size() == 3);
}

TEST_CASE("DestroyAllSwapTextureSets only frees the given process's sets")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 0);

	Desc desc{64, 64, k_vk_format_r8g8b8a8_srgb, 1};
	Set p1a{}, p1b{}, p2{};
	dm.CreateSwapTextureSet(1, &desc, &p1a);
	dm.CreateSwapTextureSet(2, &desc, &p2);
	dm.CreateSwapTextureSet(1, &desc, &p1b);

	dm.DestroyAllSwapTextureSets(1);
	CHECK(ctx.ipc.images.size() == 3);
	for (auto h : p2.rSharedTextureHandles) {
		CHECK(ctx.ipc.images.count(h) == 1);
	}
}

TEST_CASE("Submit/Present bookkeeping logs the first frames only")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 2); // log 2 frames

	Desc desc{64, 64, k_vk_format_r8g8b8a8_srgb, 1};
	Set set{};
	dm.CreateSwapTextureSet(1, &desc, &set);

	vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};
	eyes[0].hTexture = set.rSharedTextureHandles[0];
	eyes[1].hTexture = set.rSharedTextureHandles[0];
	eyes[0].bounds = eyes[1].bounds = vr::VRTextureBounds_t{0.f, 0.f, 1.f, 1.f};

	const size_t before = ctx.log.copy().size();
	for (int frame = 0; frame < 4; frame++) {
		dm.SubmitLayer(eyes);
		dm.Present(0);
	}
	// 2 logged frames × (2 eye lines + 1 present line)
	CHECK(ctx.log.copy().size() - before == 6);
}

TEST_CASE("more layers than shown in a frame: logged once, the rest still works")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 0);

	Desc desc{64, 64, k_vk_format_r8g8b8a8_srgb, 1};
	Set set{};
	dm.CreateSwapTextureSet(1, &desc, &set);
	vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};
	eyes[0].hTexture = eyes[1].hTexture = set.rSharedTextureHandles[0];
	eyes[0].bounds = eyes[1].bounds = vr::VRTextureBounds_t{0.f, 0.f, 1.f, 1.f};

	auto overflow_lines = [&] {
		int n = 0;
		for (const auto &l : ctx.log.copy()) {
			n += l.find("more than 4 layers") != std::string::npos;
		}
		return n;
	};
	for (int frame = 0; frame < 3; frame++) {
		for (int layer = 0; layer < ml::DirectModeComponent::k_max_layers; layer++) {
			dm.SubmitLayer(eyes);
		}
		dm.Present(0);
	}
	CHECK(overflow_lines() == 0); // up to k_max_layers is fine
	for (int frame = 0; frame < 3; frame++) {
		for (int layer = 0; layer < ml::DirectModeComponent::k_max_layers + 2; layer++) {
			dm.SubmitLayer(eyes);
		}
		dm.Present(0);
		dm.PostPresent(nullptr);
	}
	CHECK(overflow_lines() == 1);
}

TEST_CASE("GetFrameTiming clears the reprojection flags")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 0);

	vr::DriverDirectMode_FrameTiming timing{};
	timing.m_nSize = sizeof(timing);
	timing.m_nReprojectionFlags = 0xffffffff;
	dm.GetFrameTiming(&timing);
	CHECK(timing.m_nReprojectionFlags == 0);
}

TEST_CASE("PostPresent paces frames to the refresh rate")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(100.f, 0); // 10 ms per frame

	vr::IVRDriverDirectModeComponent::Throttling_t throttling{};
	dm.PostPresent(&throttling); // first call syncs the pacing clock
	auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < 3; i++) {
		dm.PostPresent(&throttling);
	}
	auto elapsed = std::chrono::steady_clock::now() - start;
	CHECK(elapsed >= std::chrono::milliseconds(25)); // ~30 ms, with slack for scheduling
	CHECK(elapsed < std::chrono::milliseconds(200));
}

// --- Texture fds for the Vulkan import (M2) --------------------------------------------------------

#include "vk/vk_copy.h"

#include <dirent.h>

static int
open_fd_count()
{
	int n = 0;
	if (DIR *d = opendir("/proc/self/fd")) {
		while (readdir(d) != nullptr) {
			n++;
		}
		closedir(d);
	}
	return n;
}

TEST_CASE("swap images are created importable: the flags and usage the copier imports with")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(120.f, 0);

	Desc desc{64, 64, k_vk_format_r8g8b8a8_srgb, 1};
	Set set{};
	dm.CreateSwapTextureSet(1, &desc, &set);
	for (auto h : set.rSharedTextureHandles) {
		CHECK(ctx.ipc.images[h].create_flags == ml::k_shared_image_flags);
		CHECK(ctx.ipc.images[h].usage_flags == ml::k_shared_image_usage);
	}
}

TEST_CASE("each texture's fd is received, and the extra reference and the fd are released with the set")
{
	fake::DriverContext ctx;
	ctx.install();
	ctx.ipc.share_fds = true;
	const int fds_before = open_fd_count();
	{
		ml::DirectModeComponent dm(120.f, 0);
		Desc desc{64, 64, k_vk_format_r8g8b8a8_srgb, 1};
		Set set{};
		dm.CreateSwapTextureSet(1, &desc, &set);
		CHECK(ctx.ipc.fds_given == 3);
		CHECK(open_fd_count() == fds_before + 3);
		for (auto h : set.rSharedTextureHandles) {
			CHECK(ctx.ipc.images[h].refs == 2); // SteamVR's + ours (RefResource)
		}

		dm.DestroySwapTextureSet(set.rSharedTextureHandles[0]);
		CHECK(ctx.ipc.images.empty()); // both references dropped
		CHECK(open_fd_count() == fds_before);

		Set again{};
		dm.CreateSwapTextureSet(1, &desc, &again); // left alive: the destructor closes its fds
	}
	CHECK(open_fd_count() == fds_before);
}

TEST_CASE("without an OpenXR session Present doesn't pace through OpenXR: PostPresent still sleeps")
{
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(100.f, 0);
	Desc desc{64, 64, k_vk_format_r8g8b8a8_srgb, 1};
	Set set{};
	dm.CreateSwapTextureSet(1, &desc, &set);
	vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t eyes[2]{};
	eyes[0].hTexture = eyes[1].hTexture = set.rSharedTextureHandles[0];

	vr::IVRDriverDirectModeComponent::Throttling_t throttling{};
	dm.PostPresent(&throttling);
	auto start = std::chrono::steady_clock::now();
	for (int i = 0; i < 3; i++) {
		dm.SubmitLayer(eyes);
		dm.Present(0);
		dm.PostPresent(&throttling);
	}
	CHECK(std::chrono::steady_clock::now() - start >= std::chrono::milliseconds(25));
}

TEST_CASE("frame_logged: the first log_frames frames, then every 1200 unless quiet")
{
	CHECK(ml::frame_logged(0, 5, false));
	CHECK(ml::frame_logged(4, 5, false));
	CHECK_FALSE(ml::frame_logged(5, 5, false));
	CHECK(ml::frame_logged(2400, 5, false));
	CHECK(ml::frame_logged(4, 5, true));
	CHECK_FALSE(ml::frame_logged(2400, 5, true));
	CHECK_FALSE(ml::frame_logged(0, 0, true));
}
