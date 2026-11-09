// SPDX-License-Identifier: MIT
// Logging into SteamVR's vrserver.txt (Settings → Developer → Web console, or the log file).
#pragma once

#include <openvr_driver.h>

#include <cstdarg>
#include <cstdio>
#include <sys/syscall.h>
#include <unistd.h>

namespace ml {

__attribute__((format(printf, 1, 2))) inline void
log(const char *fmt, ...)
{
	char buf[1024];
	int n = snprintf(buf, sizeof(buf), "[monadolink] ");
	va_list args;
	va_start(args, fmt);
	vsnprintf(buf + n, sizeof(buf) - n, fmt, args);
	va_end(args);
	// Outside vrserver (tools, before Init) there is no driver context, and VRDriverLog() would
	// dereference it: check the context first.
	if (vr::VRDriverContext() != nullptr && vr::VRDriverLog() != nullptr) {
		vr::VRDriverLog()->Log(buf);
	} else {
		fprintf(stderr, "%s\n", buf);
	}
}

//! "pid 1234 (vrserver) tid 1240": used to find out which process/thread calls what.
inline const char *
process_tag()
{
	static thread_local char tag[96];
	char comm[32] = "?";
	if (FILE *f = fopen("/proc/self/comm", "r")) {
		if (fgets(comm, sizeof(comm), f) != nullptr) {
			for (char *c = comm; *c; c++) {
				if (*c == '\n') {
					*c = '\0';
				}
			}
		}
		fclose(f);
	}
	snprintf(tag, sizeof(tag), "pid %d (%s) tid %ld", (int)getpid(), comm, (long)syscall(SYS_gettid));
	return tag;
}

} // namespace ml
