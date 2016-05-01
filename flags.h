#ifndef _FLAGS_H
#define _FLAGS_H

#include <map>
#include <string>

#include "defs.h"

struct Flags {
	int num_cards = 2;
	std::string va_display;
	bool uncompressed_video_to_http = false;
	bool x264_video_to_http = false;
	std::string theme_filename = "theme.lua";
	bool flat_audio = false;
	bool flush_pbos = true;
	std::string stream_mux_name = DEFAULT_STREAM_MUX_NAME;
	bool stream_coarse_timebase = false;
	std::string stream_audio_codec_name;  // Blank = use the same as for the recording.
	int stream_audio_codec_bitrate = DEFAULT_AUDIO_OUTPUT_BIT_RATE;  // Ignored if stream_audio_codec_name is blank.
	std::string x264_preset = X264_DEFAULT_PRESET;
	std::string x264_tune = X264_DEFAULT_TUNE;
	int x264_bitrate = DEFAULT_X264_OUTPUT_BIT_RATE;  // In kilobit/sec.
	int x264_vbv_max_bitrate = -1;  // In kilobits. 0 = no limit, -1 = same as <x264_bitrate> (CBR).
	int x264_vbv_buffer_size = -1;  // In kilobits. 0 = one-frame VBV, -1 = same as <x264_bitrate> (one-second VBV).
	bool enable_alsa_output = true;
	std::map<int, int> default_stream_mapping;
};
extern Flags global_flags;

void parse_flags(int argc, char * const argv[]);

#endif  // !defined(_FLAGS_H)
