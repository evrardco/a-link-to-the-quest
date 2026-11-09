// SPDX-License-Identifier: MIT
// Forward what the process writes to stderr into SteamVR's log for a while.
//
// The OpenXR loader and Monado's client library report their errors on stderr, which vrserver
// doesn't log: without this, a failed xrCreateInstance only says "XR_ERROR_RUNTIME_FAILURE".
#pragma once

#include "util/log.h"

#include <fcntl.h>
#include <string>
#include <thread>
#include <unistd.h>

namespace ml {

class StderrCapture
{
public:
	//! Starts capturing; lines are logged with `prefix`. Does nothing if the pipe can't be made.
	explicit StderrCapture(const char *prefix) : prefix_(prefix)
	{
		int fds[2];
		if (pipe2(fds, O_CLOEXEC) != 0) {
			return;
		}
		saved_ = dup(STDERR_FILENO);
		if (saved_ < 0 || dup2(fds[1], STDERR_FILENO) < 0) {
			close(fds[0]);
			close(fds[1]);
			if (saved_ >= 0) {
				close(saved_);
				saved_ = -1;
			}
			return;
		}
		close(fds[1]); // stderr now holds the write end
		read_fd_ = fds[0];
		reader_ = std::thread([this] { forward(); });
	}

	//! Restores stderr and logs whatever is still pending.
	~StderrCapture()
	{
		if (saved_ < 0) {
			return;
		}
		fflush(stderr);
		dup2(saved_, STDERR_FILENO); // closes the last write end: the reader sees EOF
		close(saved_);
		reader_.join();
		close(read_fd_);
	}

	StderrCapture(const StderrCapture &) = delete;
	StderrCapture &
	operator=(const StderrCapture &) = delete;

private:
	void
	forward()
	{
		std::string line;
		char buf[512];
		ssize_t n;
		while ((n = read(read_fd_, buf, sizeof(buf))) > 0) {
			for (ssize_t i = 0; i < n; i++) {
				if (buf[i] == '\n') {
					flush_line(line);
				} else {
					line += buf[i];
				}
			}
		}
		flush_line(line);
	}

	void
	flush_line(std::string &line)
	{
		if (!line.empty()) {
			// Our own log() would write to stderr outside vrserver: write to the saved stderr then.
			if (vr::VRDriverContext() != nullptr) {
				log("%s%s", prefix_, line.c_str());
			} else {
				dprintf(saved_, "%s%s\n", prefix_, line.c_str());
			}
		}
		line.clear();
	}

	const char *prefix_;
	int saved_ = -1;
	int read_fd_ = -1;
	std::thread reader_;
};

} // namespace ml
