#ifndef _FLAGS_H
#define _FLAGS_H

#include <map>
#include <string>
#include <vector>

#include "defs.h"

struct Flags {
	int width = 1280, height = 720;
	int num_cards = 2;
	std::string va_display;
	bool fake_cards_audio = false;
	bool uncompressed_video_to_http = false;
	bool x264_video_to_http = false;
	std::vector<std::string> theme_dirs { ".", "/usr/local/share/nageru" };
	std::string theme_filename = "theme.lua";
	bool locut_enabled = true;
	bool gain_staging_auto = true;
	float initial_gain_staging_db = 0.0f;
	bool compressor_enabled = true;
	bool limiter_enabled = true;
	bool final_makeup_gain_auto = true;
	bool flush_pbos = true;
	std::string stream_mux_name = DEFAULT_STREAM_MUX_NAME;
	bool stream_coarse_timebase = false;
	std::string stream_audio_codec_name;  // Blank = use the same as for the recording.
	int stream_audio_codec_bitrate = DEFAULT_AUDIO_OUTPUT_BIT_RATE;  // Ignored if stream_audio_codec_name is blank.
	std::string x264_preset;  // Empty will be overridden by X264_DEFAULT_PRESET, unless speedcontrol is set.
	std::string x264_tune = X264_DEFAULT_TUNE;
	bool x264_speedcontrol = false;
	bool x264_speedcontrol_verbose = false;
	int x264_bitrate = DEFAULT_X264_OUTPUT_BIT_RATE;  // In kilobit/sec.
	int x264_vbv_max_bitrate = -1;  // In kilobits. 0 = no limit, -1 = same as <x264_bitrate> (CBR).
	int x264_vbv_buffer_size = -1;  // In kilobits. 0 = one-frame VBV, -1 = same as <x264_bitrate> (one-second VBV).
	std::vector<std::string> x264_extra_param;  // In “key[,value]” format.
	bool enable_alsa_output = true;
	std::map<int, int> default_stream_mapping;
	bool multichannel_mapping_mode = false;  // Implicitly true if input_mapping_filename is nonempty.
	std::string input_mapping_filename;  // Empty for none.
	std::string midi_mapping_filename;  // Empty for none.
	bool print_video_latency = false;
	double audio_queue_length_ms = 100.0;
	bool ycbcr_rec709_coefficients = false;
	int output_card = -1;
	double output_buffer_frames = 6.0;
	double output_slop_frames = 0.5;
};
extern Flags global_flags;

void parse_flags(int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
