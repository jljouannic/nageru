#include "print_latency.h"

#include "flags.h"
#include "metrics.h"

#include <stdio.h>
#include <chrono>
#include <string>

using namespace std;
using namespace std::chrono;

ReceivedTimestamps find_received_timestamp(const vector<RefCountedFrame> &input_frames)
{
	// Find min and max timestamp of all input frames that have a timestamp.
	steady_clock::time_point min_ts = steady_clock::time_point::max(), max_ts = steady_clock::time_point::min();
	for (const RefCountedFrame &input_frame : input_frames) {
		if (input_frame && input_frame->received_timestamp > steady_clock::time_point::min()) {
			min_ts = min(min_ts, input_frame->received_timestamp);
			max_ts = max(max_ts, input_frame->received_timestamp);
		}
	}
	return { min_ts, max_ts };
}

void LatencyHistogram::init(const string &measuring_point)
{
	histogram_lowest_latency_input.init_geometric(0.001, 10.0, 30);
	histogram_highest_latency_input.init_geometric(0.001, 10.0, 30);
	histogram_lowest_latency_input_bframe.init_geometric(0.001, 10.0, 30);
	histogram_highest_latency_input_bframe.init_geometric(0.001, 10.0, 30);

	global_metrics.add("latency_seconds",
		{{ "measuring_point", measuring_point }, { "input", "lowest_latency" }, { "frame_type", "i/p" }},
		&histogram_lowest_latency_input);
	global_metrics.add("latency_seconds",
		{{ "measuring_point", measuring_point }, { "input", "highest_latency" }, { "frame_type", "i/p" }},
		&histogram_highest_latency_input);
	global_metrics.add("latency_seconds",
		{{ "measuring_point", measuring_point }, { "input", "lowest_latency" }, { "frame_type", "b" }},
		&histogram_lowest_latency_input_bframe);
	global_metrics.add("latency_seconds",
		{{ "measuring_point", measuring_point }, { "input", "highest_latency" }, { "frame_type", "b" }},
		&histogram_highest_latency_input_bframe);
}

void print_latency(const string &header, const ReceivedTimestamps &received_ts, bool is_b_frame, int *frameno, LatencyHistogram *histogram)
{
	if (received_ts.max_ts == steady_clock::time_point::min())
		return;

	const steady_clock::time_point now = steady_clock::now();
	duration<double> lowest_latency = now - received_ts.max_ts;
	duration<double> highest_latency = now - received_ts.min_ts;

	if (is_b_frame) {
		histogram->histogram_lowest_latency_input_bframe.count_event(lowest_latency.count());
		histogram->histogram_highest_latency_input_bframe.count_event(highest_latency.count());
	} else {
		histogram->histogram_lowest_latency_input.count_event(lowest_latency.count());
		histogram->histogram_highest_latency_input.count_event(highest_latency.count());
	}

	// 101 is chosen so that it's prime, which is unlikely to get the same frame type every time.
	if (global_flags.print_video_latency && (++*frameno % 101) == 0) {
		printf("%-60s %4.0f ms (lowest-latency input), %4.0f ms (highest-latency input)",
			header.c_str(), 1e3 * lowest_latency.count(), 1e3 * highest_latency.count());

		if (is_b_frame) {
			printf("  [on B-frame; potential extra latency]\n");
		} else {
			printf("\n");
		}
	}
}
