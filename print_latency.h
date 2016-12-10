#ifndef _PRINT_LATENCY_H
#define _PRINT_LATENCY_H 1

// A small utility function to print the latency between two end points
// (typically when the frame was received from the video card, and some
// point when the frame is ready to be output in some form).

#include <chrono>
#include <string>

// Since every output frame is based on multiple input frames, we need
// more than one start timestamp. For now, we keep just the smallest
// and largest timestamp, so that we can print out a range.
// For both of these, steady_clock::time_point::min() is used for “not set”.
struct ReceivedTimestamps {
	std::chrono::steady_clock::time_point min_ts, max_ts;
};

void print_latency(const std::string &header, const ReceivedTimestamps &received_ts, bool is_b_frame, int *frameno);

#endif  // !defined(_PRINT_LATENCY_H)
