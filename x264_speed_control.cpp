#include "x264_speed_control.h"

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <x264.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ratio>
#include <type_traits>

#include "flags.h"
#include "metrics.h"

using namespace std;
using namespace std::chrono;

X264SpeedControl::X264SpeedControl(x264_t *x264, float f_speed, int i_buffer_size, float f_buffer_init)
	: dyn(load_x264_for_bit_depth(global_flags.x264_bit_depth)),
	  x264(x264), f_speed(f_speed)
{
	x264_param_t param;
	dyn.x264_encoder_parameters(x264, &param);

	float fps = (float)param.i_fps_num / param.i_fps_den;
	uspf = 1e6 / fps;
	set_buffer_size(i_buffer_size);
	buffer_fill = buffer_size * f_buffer_init;
	buffer_fill = max<int64_t>(buffer_fill, uspf);
	buffer_fill = min(buffer_fill, buffer_size);
	timestamp = steady_clock::now();
	preset = -1;
	cplx_num = 3e3; //FIXME estimate initial complexity
	cplx_den = .1;
	stat.min_buffer = buffer_size;
	stat.max_buffer = 0;
	stat.avg_preset = 0.0;
	stat.den = 0;

	metric_x264_speedcontrol_buffer_available_seconds = buffer_fill * 1e-6;
	metric_x264_speedcontrol_buffer_size_seconds = buffer_size * 1e-6;
	global_metrics.add_histogram("x264_speedcontrol_preset_used_frames", {}, metric_x264_speedcontrol_preset_used_frames, &metric_x264_speedcontrol_preset_used_frames_sum, SC_PRESETS);
	global_metrics.add("x264_speedcontrol_buffer_available_seconds", &metric_x264_speedcontrol_buffer_available_seconds, Metrics::TYPE_GAUGE);
	global_metrics.add("x264_speedcontrol_buffer_size_seconds", &metric_x264_speedcontrol_buffer_size_seconds, Metrics::TYPE_GAUGE);
	global_metrics.add("x264_speedcontrol_idle_frames", &metric_x264_speedcontrol_idle_frames);
	global_metrics.add("x264_speedcontrol_late_frames", &metric_x264_speedcontrol_late_frames);
}

X264SpeedControl::~X264SpeedControl()
{
	fprintf(stderr, "speedcontrol: avg preset=%.3f  buffer min=%.3f max=%.3f\n",
		stat.avg_preset / stat.den,
		(float)stat.min_buffer / buffer_size,
		(float)stat.max_buffer / buffer_size );
	//  x264_log( x264, X264_LOG_INFO, "speedcontrol: avg cplx=%.5f\n", cplx_num / cplx_den );
	if (dyn.handle) {
		dlclose(dyn.handle);
	}
}

typedef struct
{
	float time; // relative encoding time, compared to the other presets
	int subme;
	int me;
	int refs;
	int mix;
	int trellis;
	int partitions;
	int badapt;
	int bframes;
	int direct;
	int merange;
} sc_preset_t;

// The actual presets, including the equivalent commandline options. Note that
// all presets are benchmarked with --weightp 1 --mbtree --rc-lookahead 20
// on top of the given settings (equivalent settings to the "faster" preset).
// Timings and SSIM measurements were done on a quadcore Haswell i5 3.2 GHz
// on the first 1000 frames of "Tears of Steel" in 1080p.
//
// Note that the two first and the two last are also used for extrapolation
// should the desired time be outside the range. Thus, it is disadvantageous if
// they are chosen so that the timings are too close to each other.
static const sc_preset_t presets[SC_PRESETS] = {
#define I4 X264_ANALYSE_I4x4
#define I8 X264_ANALYSE_I8x8
#define P4 X264_ANALYSE_PSUB8x8
#define P8 X264_ANALYSE_PSUB16x16
#define B8 X264_ANALYSE_BSUB16x16
	// Preset 0: 14.179db, --preset superfast --b-adapt 0 --bframes 0
	{ .time= 1.000, .subme=1, .me=X264_ME_DIA, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4, .badapt=0, .bframes=0, .direct=0, .merange=16 },

	// Preset 1: 14.459db, --preset superfast
	{ .time= 1.283, .subme=1, .me=X264_ME_DIA, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 2: 14.761db, --preset superfast --subme 2
	{ .time= 1.603, .subme=2, .me=X264_ME_DIA, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 3: 15.543db, --preset veryfast
	{ .time= 1.843, .subme=2, .me=X264_ME_HEX, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 4: 15.716db, --preset veryfast --subme 3
	{ .time= 2.452, .subme=3, .me=X264_ME_HEX, .refs=1, .mix=0, .trellis=0, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 5: 15.786db, --preset veryfast --subme 3 --ref 2
	{ .time= 2.733, .subme=3, .me=X264_ME_HEX, .refs=2, .mix=0, .trellis=0, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 6: 15.813db, --preset veryfast --subme 4 --ref 2
	{ .time= 3.085, .subme=4, .me=X264_ME_HEX, .refs=2, .mix=0, .trellis=0, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 7: 15.849db, --preset faster
	{ .time= 3.101, .subme=4, .me=X264_ME_HEX, .refs=2, .mix=0, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 8: 15.857db, --preset faster --mixed-refs
	{ .time= 3.284, .subme=4, .me=X264_ME_HEX, .refs=2, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 9: 15.869db, --preset faster --mixed-refs --subme 5
	{ .time= 3.587, .subme=5, .me=X264_ME_HEX, .refs=2, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 10: 16.051db, --preset fast
	{ .time= 3.947, .subme=6, .me=X264_ME_HEX, .refs=2, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 11: 16.356db, --preset fast --subme 7
	{ .time= 4.041, .subme=7, .me=X264_ME_HEX, .refs=2, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 12: 16.418db, --preset fast --subme 7 --ref 3
	{ .time= 4.406, .subme=7, .me=X264_ME_HEX, .refs=3, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 13: 16.460db, --preset medium
	{ .time= 4.707, .subme=7, .me=X264_ME_HEX, .refs=3, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 14: 16.517db, --preset medium --subme 8
	{ .time= 5.133, .subme=8, .me=X264_ME_HEX, .refs=3, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 15: 16.523db, --preset medium --subme 8 --me umh
	{ .time= 6.050, .subme=8, .me=X264_ME_UMH, .refs=3, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=1, .bframes=3, .direct=1, .merange=16 },

	// Preset 16: 16.543db, --preset medium --subme 8 --me umh --direct auto --b-adapt 2
	{ .time= 6.849, .subme=8, .me=X264_ME_UMH, .refs=3, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 17: 16.613db, --preset slow
	{ .time= 8.042, .subme=8, .me=X264_ME_UMH, .refs=5, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 18: 16.641db, --preset slow --subme 9
	{ .time= 8.972, .subme=9, .me=X264_ME_UMH, .refs=5, .mix=1, .trellis=1, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 19: 16.895db, --preset slow --subme 9 --trellis 2
	{ .time=10.073, .subme=9, .me=X264_ME_UMH, .refs=5, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 20: 16.918db, --preset slow --subme 9 --trellis 2 --ref 6
	{ .time=11.147, .subme=9, .me=X264_ME_UMH, .refs=6, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 21: 16.934db, --preset slow --subme 9 --trellis 2 --ref 7
	{ .time=12.267, .subme=9, .me=X264_ME_UMH, .refs=7, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 22: 16.948db, --preset slower
	{ .time=13.829, .subme=9, .me=X264_ME_UMH, .refs=8, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8|P4, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 23: 17.058db, --preset slower --subme 10
	{ .time=14.831, .subme=10, .me=X264_ME_UMH, .refs=8, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8|P4, .badapt=2, .bframes=3, .direct=3, .merange=16 },

	// Preset 24: 17.268db, --preset slower --subme 10 --bframes 8
	{ .time=18.705, .subme=10, .me=X264_ME_UMH, .refs=8, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8|P4, .badapt=2, .bframes=8, .direct=3, .merange=16 },

	// Preset 25: 17.297db, --preset veryslow
	{ .time=31.419, .subme=10, .me=X264_ME_UMH, .refs=16, .mix=1, .trellis=2, .partitions=I8|I4|P8|B8|P4, .badapt=2, .bframes=8, .direct=3, .merange=24 },
#undef I4
#undef I8
#undef P4
#undef P8
#undef B8
};

void X264SpeedControl::before_frame(float new_buffer_fill, int new_buffer_size, float new_uspf)
{
	if (new_uspf > 0.0) {
		uspf = new_uspf;
	}
	if (new_buffer_size) {
		set_buffer_size(new_buffer_size);
	}
	buffer_fill = buffer_size * new_buffer_fill;
	metric_x264_speedcontrol_buffer_available_seconds = buffer_fill * 1e-6;

	steady_clock::time_point t;

	// update buffer state after encoding and outputting the previous frame(s)
	if (first) {
		t = timestamp = steady_clock::now();
		first = false;
	} else {
		t = steady_clock::now();
	}

	auto delta_t = t - timestamp;
	timestamp = t;

	// update the time predictor
	if (preset >= 0) {
		int cpu_time = duration_cast<microseconds>(cpu_time_last_frame).count();
		cplx_num *= cplx_decay;
		cplx_den *= cplx_decay;
		cplx_num += cpu_time / presets[preset].time;
		++cplx_den;

		stat.avg_preset += preset;
		++stat.den;
	}

	stat.min_buffer = min(buffer_fill, stat.min_buffer);
	stat.max_buffer = max(buffer_fill, stat.max_buffer);

	if (buffer_fill >= buffer_size) { // oops, cpu was idle
		// not really an error, but we'll warn for debugging purposes
		static int64_t idle_t = 0;
		static steady_clock::time_point print_interval;
		static bool first = false;
		idle_t += buffer_fill - buffer_size;
		if (first || duration<double>(t - print_interval).count() > 0.1) {
			//fprintf(stderr, "speedcontrol idle (%.6f sec)\n", idle_t/1e6);
			print_interval = t;
			idle_t = 0;
			first = false;
		}
		buffer_fill = buffer_size;
		metric_x264_speedcontrol_buffer_available_seconds = buffer_fill * 1e-6;
		++metric_x264_speedcontrol_idle_frames;
	} else if (buffer_fill <= 0) {  // oops, we're late
		// fprintf(stderr, "speedcontrol underflow (%.6f sec)\n", buffer_fill/1e6);
		++metric_x264_speedcontrol_late_frames;
	}

	{
		// Pick the preset that should return the buffer to 3/4-full within a time
		// specified by compensation_period.
		//
		// NOTE: This doesn't actually do that, at least assuming the same target is
		// chosen for every frame; exactly what it does is unclear to me. It seems
		// to consistently undershoot a bit, so it needs to be saved by the second
		// predictor below. However, fixing the formula seems to yield somewhat less
		// stable results in practice; in particular, once the buffer is half-full
		// or so, it would give us a negative target. Perhaps increasing
		// compensation_period would be a good idea, but initial (very brief) tests
		// did not yield good results.
		float target = uspf / f_speed
			* (buffer_fill + compensation_period)
			/ (buffer_size*3/4 + compensation_period);
		float cplx = cplx_num / cplx_den;
		float set, t0, t1;
		float filled = (float) buffer_fill / buffer_size;
		int i;
		t0 = presets[0].time * cplx;
		for (i = 1; ; i++) {
			t1 = presets[i].time * cplx;
			if (t1 >= target || i == SC_PRESETS - 1)
				break;
			t0 = t1;
		}
		// exponential interpolation between states
		set = i-1 + (log(target) - log(t0)) / (log(t1) - log(t0));
		set = max<float>(set, -5);
		set = min<float>(set, (SC_PRESETS-1) + 5);
		// Even if our time estimations in the SC_PRESETS array are off
		// this will push us towards our target fullness
		float s1 = set;
		set += (40 * (filled-0.75));
		float s2 = (40 * (filled-0.75));
		set = min<float>(max<float>(set, 0), SC_PRESETS - 1);
		apply_preset(dither_preset(set));

		if (global_flags.x264_speedcontrol_verbose) {
			static float cpu, wall, tgt, den;
			const float decay = 1-1/100.;
			cpu = cpu*decay + duration_cast<microseconds>(cpu_time_last_frame).count();
			wall = wall*decay + duration_cast<microseconds>(delta_t).count();
			tgt = tgt*decay + target;
			den = den*decay + 1;
			fprintf(stderr, "speed: %.2f+%.2f %d[%.5f] (t/c/w: %6.0f/%6.0f/%6.0f = %.4f) fps=%.2f\r",
					s1, s2, preset, (float)buffer_fill / buffer_size,
					tgt/den, cpu/den, wall/den, cpu/wall, 1e6*den/wall );
		}
	}

}

void X264SpeedControl::after_frame()
{
	cpu_time_last_frame = steady_clock::now() - timestamp;
}

void X264SpeedControl::set_buffer_size(int new_buffer_size)
{
	new_buffer_size = max(3, new_buffer_size);
	buffer_size = new_buffer_size * uspf;
	cplx_decay = 1 - 1./new_buffer_size;
	compensation_period = buffer_size/4;
}

int X264SpeedControl::dither_preset(float f)
{
	int i = f;
	if (f < 0) {
		i--;
	}
	dither += f - i;
	if (dither >= 1.0) {
		dither--;
		i++;
	}
	return i;
}

void X264SpeedControl::apply_preset(int new_preset)
{
	new_preset = max(new_preset, 0);
	new_preset = min(new_preset, SC_PRESETS - 1);

	const sc_preset_t *s = &presets[new_preset];
	x264_param_t p;
	dyn.x264_encoder_parameters(x264, &p);

	p.i_frame_reference = s->refs;
	p.i_bframe_adaptive = s->badapt;
	p.i_bframe = s->bframes;
	p.analyse.inter = s->partitions;
	p.analyse.i_subpel_refine = s->subme;
	p.analyse.i_me_method = s->me;
	p.analyse.i_trellis = s->trellis;
	p.analyse.b_mixed_references = s->mix;
	p.analyse.i_direct_mv_pred = s->direct;
	p.analyse.i_me_range = s->merange;
	if (override_func) {
		override_func(&p);
	}
	dyn.x264_encoder_reconfig(x264, &p);
	preset = new_preset;

	++metric_x264_speedcontrol_preset_used_frames[new_preset];
	// Non-atomic add, but that's fine, since there are no concurrent writers.
	metric_x264_speedcontrol_preset_used_frames_sum = metric_x264_speedcontrol_preset_used_frames_sum + new_preset;
}
