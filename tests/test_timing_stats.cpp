// TimingStats: the per-interval frame timing line in vrserver.txt.
#include "direct/direct_mode.h"
#include "fake_openvr.h"
#include "util/timing_stats.h"

#include <doctest/doctest.h>

#include <string>
#include <thread>

static const int64_t ms = 1'000'000;

TEST_CASE("the report has counts and avg/max per metric, in ms")
{
	ml::TimingStats st(12 * ms);
	CHECK_FALSE(st.due(1'000 * ms, 1'000 * ms)); // first call starts the period
	for (int i = 0; i < 3; i++) {
		st.count_present();
	}
	st.add(ml::TimingStats::Gap, 8 * ms);
	st.add(ml::TimingStats::Gap, 9 * ms);
	st.add(ml::TimingStats::Gap, 20 * ms); // late (> 12 ms)
	st.add(ml::TimingStats::Copy, 1 * ms);
	st.add(ml::TimingStats::Copy, 3 * ms);
	st.count_shown();
	st.count_shown();
	st.count_dropped();
	CHECK(st.late() == 1);

	CHECK_FALSE(st.due(1'500 * ms, 1'000 * ms));
	REQUIRE(st.due(2'000 * ms, 1'000 * ms));
	char line[512];
	st.report(line, sizeof(line), 2'000 * ms);
	const std::string s = line;
	CHECK(s.find("timing 1.0 s: 3 presents (1 late), 2 shown, 1 dropped") == 0);
	CHECK(s.find("gap avg 12.33 max 20.00") != std::string::npos);
	CHECK(s.find("copy avg 2.00 max 3.00") != std::string::npos);
	CHECK(s.find("wait") == std::string::npos); // metrics without samples are left out
}

TEST_CASE("the latency metrics (predict, margin, mismatch) can be negative and all fit in one line")
{
	ml::TimingStats st(12 * ms);
	st.due(1, 1'000 * ms);
	for (int m = 0; m < ml::TimingStats::k_metric_count; m++) {
		st.add((ml::TimingStats::Metric)m, 99 * ms); // widest realistic values
	}
	st.add(ml::TimingStats::Mismatch, -12 * ms);
	st.add(ml::TimingStats::Mismatch, -4 * ms);
	char line[512];
	st.report(line, sizeof(line), 1'000 * ms);
	const std::string s = line;
	CHECK(s.find("predict avg 99.00") != std::string::npos);
	CHECK(s.find("margin avg 99.00") != std::string::npos);
	CHECK(s.find("mismatch avg 27.67 max 99.00") != std::string::npos);
	CHECK(s.size() < sizeof(line) - 1); // not truncated

	st.add(ml::TimingStats::Mismatch, -12 * ms);
	st.add(ml::TimingStats::Mismatch, -4 * ms);
	st.report(line, sizeof(line), 2'000 * ms);
	CHECK(std::string(line).find("mismatch avg -8.00 max -4.00") != std::string::npos);
}

TEST_CASE("a report starts a new period")
{
	ml::TimingStats st(12 * ms);
	st.due(0 + 1, 1'000 * ms);
	st.count_present();
	st.add(ml::TimingStats::Gap, 30 * ms);
	char line[512];
	st.report(line, sizeof(line), 1'000 * ms);
	CHECK(st.late() == 0);
	CHECK_FALSE(st.due(1'500 * ms, 1'000 * ms));
	st.report(line, sizeof(line), 2'000 * ms);
	CHECK(std::string(line).find("0 presents (0 late), 0 shown, 0 dropped") != std::string::npos);
}

TEST_CASE("Present logs a timing line every ML_TIMING_STATS_INTERVAL_S seconds")
{
	if (ML_TIMING_STATS_INTERVAL_S <= 0 || ML_TIMING_STATS_INTERVAL_S > 2) {
		return; // compiled out, or too slow to test
	}
	fake::DriverContext ctx;
	ctx.install();
	ml::DirectModeComponent dm(200.f, 0); // 5 ms frames; no OpenXR: PostPresent paces
	vr::IVRDriverDirectModeComponent::Throttling_t throttling{};
	const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ML_TIMING_STATS_INTERVAL_S * 1000 + 300);
	while (std::chrono::steady_clock::now() < end) {
		dm.Present(0);
		dm.PostPresent(&throttling);
	}
	int timing_lines = 0;
	for (const auto &l : ctx.log.copy()) {
		if (l.find("[monadolink] timing ") == 0) {
			timing_lines++;
			CHECK(l.find("presents") != std::string::npos);
			CHECK(l.find("gap avg") != std::string::npos);
		}
	}
	CHECK(timing_lines == 1);
}
