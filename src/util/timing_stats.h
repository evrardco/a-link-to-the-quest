// SPDX-License-Identifier: MIT
// Frame timing statistics, logged as one line per interval to find where late frames come from.
//
// The interval is a compile-time setting: ML_TIMING_STATS_INTERVAL_S seconds (CMake cache variable
// MONADOLINK_TIMING_STATS_S, default 1); 0 compiles the statistics out.
#pragma once

#include <cstdint>
#include <cstdio>

#ifndef ML_TIMING_STATS_INTERVAL_S
#define ML_TIMING_STATS_INTERVAL_S 1
#endif

namespace ml {

class TimingStats
{
public:
	enum Metric
	{
		Gap,     //!< between two SteamVR Present calls (SteamVR's frame cadence)
		Wait,    //!< xrWaitFrame + xrBeginFrame (Monado pacing us)
		Acquire, //!< acquiring + waiting for both swapchain images
		Copy,    //!< our GPU copy, submit to fence
		End,     //!< xrEndFrame
		Total,   //!< the whole OpenXR frame inside Present
		Ahead,   //!< predicted display time minus now, after xrWaitFrame
		Predict, //!< how far ahead (from SubmitLayer) SteamVR predicted the pose it rendered with
		Margin,  //!< Monado's display time minus the end of our xrEndFrame (slack before Monado needs it)
		Mismatch, //!< SteamVR's predicted display time minus Monado's (> 0: SteamVR predicts further)
		k_metric_count
	};

	//! `late_after_ns`: a Present gap longer than this counts as late (1.5 frame intervals).
	explicit TimingStats(int64_t late_after_ns) : late_after_ns_(late_after_ns) {}

	void
	add(Metric m, int64_t ns)
	{
		Acc &a = acc_[m];
		if (a.count == 0 || ns < a.min) {
			a.min = ns;
		}
		if (a.count == 0 || ns > a.max) {
			a.max = ns;
		}
		a.sum += ns;
		a.count++;
		if (m == Gap && ns > late_after_ns_) {
			late_++;
		}
	}
	void
	count_present()
	{
		presents_++;
	}
	void
	count_shown()
	{
		shown_++;
	}
	void
	count_dropped()
	{
		dropped_++;
	}

	//! True once `interval_ns` has passed since the period started (the first call starts it).
	bool
	due(int64_t now_ns, int64_t interval_ns)
	{
		if (period_start_ns_ == 0) {
			period_start_ns_ = now_ns;
			return false;
		}
		return now_ns - period_start_ns_ >= interval_ns;
	}

	//! One line: counts, then avg/max (ms) of each metric that has samples. Starts a new period.
	void
	report(char *buf, size_t len, int64_t now_ns)
	{
		const double secs = (double)(now_ns - period_start_ns_) / 1e9;
		int n = snprintf(buf, len, "timing %.1f s: %llu presents (%llu late), %llu shown, %llu dropped", secs,
		                 (unsigned long long)presents_, (unsigned long long)late_, (unsigned long long)shown_,
		                 (unsigned long long)dropped_);
		static const char *const names[k_metric_count] = {"gap",   "wait",    "acquire", "copy",    "end",
		                                                          "frame", "ahead",   "predict", "margin", "mismatch"};
		for (int m = 0; m < k_metric_count && n > 0 && (size_t)n < len; m++) {
			const Acc &a = acc_[m];
			if (a.count == 0) {
				continue;
			}
			n += snprintf(buf + n, len - n, " | %s avg %.2f max %.2f", names[m], (double)a.sum / a.count / 1e6,
			              (double)a.max / 1e6);
		}
		reset(now_ns);
	}

	// For tests.
	uint64_t
	late() const
	{
		return late_;
	}

private:
	struct Acc
	{
		int64_t min = 0, max = 0, sum = 0;
		uint64_t count = 0;
	};

	void
	reset(int64_t now_ns)
	{
		for (auto &a : acc_) {
			a = Acc{};
		}
		presents_ = shown_ = dropped_ = late_ = 0;
		period_start_ns_ = now_ns;
	}

	int64_t late_after_ns_;
	Acc acc_[k_metric_count];
	uint64_t presents_ = 0, shown_ = 0, dropped_ = 0, late_ = 0;
	int64_t period_start_ns_ = 0;
};

} // namespace ml
