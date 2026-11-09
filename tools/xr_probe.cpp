// SPDX-License-Identifier: MIT
// Runs XrClient (the driver's OpenXR code) against the active OpenXR runtime without SteamVR:
// prints what the HMD would report and head poses. Usage: monadolink_xr_probe [seconds] [runtime.json]
#include "xr/xr_client.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int
main(int argc, char **argv)
{
	const int seconds = argc > 1 ? atoi(argv[1]) : 10;
	ml::XrClient xr;
	if (!xr.start(argc > 2 ? argv[2] : "")) {
		fprintf(stderr, "could not connect to the OpenXR runtime\n");
		return 1;
	}
	ml::DisplayConfig c{};
	xr.apply_display_info(&c);
	if (c.has_eye_to_head) {
		for (int eye = 0; eye < 2; eye++) {
			printf("eye %d to head: translation %.4f %.4f %.4f\n", eye, c.eye_to_head[eye].m[0][3],
			       c.eye_to_head[eye].m[1][3], c.eye_to_head[eye].m[2][3]);
		}
	}

	const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
	int n = 0;
	while (std::chrono::steady_clock::now() < end && !xr.lost()) {
		xr.poll_events();
		vr::DriverPose_t p;
		if (xr.locate_head(&p) && n++ % 50 == 0) { // every ~100 ms
			printf("head valid %d result %d pos %+.3f %+.3f %+.3f (world y %+.2f) quat w%+.3f x%+.3f y%+.3f "
			       "z%+.3f vel %+.2f %+.2f %+.2f\n",
			       p.poseIsValid, (int)p.result, p.vecPosition[0], p.vecPosition[1], p.vecPosition[2],
			       p.vecWorldFromDriverTranslation[1], p.qRotation.w, p.qRotation.x, p.qRotation.y, p.qRotation.z,
			       p.vecVelocity[0], p.vecVelocity[1], p.vecVelocity[2]);
			fflush(stdout);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	printf("%s\n", xr.lost() ? "runtime lost" : "done");
	return 0;
}
