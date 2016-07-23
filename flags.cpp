#include "flags.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <utility>

using namespace std;

Flags global_flags;

// Long options that have no corresponding short option.
enum LongOption {
	OPTION_VA_DISPLAY = 1000,
	OPTION_HTTP_UNCOMPRESSED_VIDEO,
	OPTION_HTTP_X264_VIDEO,
	OPTION_X264_PRESET,
	OPTION_X264_TUNE,
	OPTION_X264_SPEEDCONTROL,
	OPTION_X264_SPEEDCONTROL_VERBOSE,
	OPTION_X264_BITRATE,
	OPTION_X264_VBV_BUFSIZE,
	OPTION_X264_VBV_MAX_BITRATE,
	OPTION_X264_PARAM,
	OPTION_HTTP_MUX,
	OPTION_HTTP_COARSE_TIMEBASE,
	OPTION_HTTP_AUDIO_CODEC,
	OPTION_HTTP_AUDIO_BITRATE,
	OPTION_FLAT_AUDIO,
	OPTION_GAIN_STAGING,
	OPTION_DISABLE_LOCUT,
	OPTION_ENABLE_LOCUT,
	OPTION_DISABLE_GAIN_STAGING_AUTO,
	OPTION_ENABLE_GAIN_STAGING_AUTO,
	OPTION_DISABLE_COMPRESSOR,
	OPTION_ENABLE_COMPRESSOR,
	OPTION_DISABLE_LIMITER,
	OPTION_ENABLE_LIMITER,
	OPTION_DISABLE_MAKEUP_GAIN_AUTO,
	OPTION_ENABLE_MAKEUP_GAIN_AUTO,
	OPTION_DISABLE_ALSA_OUTPUT,
	OPTION_NO_FLUSH_PBOS
};

void usage()
{
	fprintf(stderr, "Usage: nageru [OPTION]...\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "  -h, --help                      print usage information\n");
	fprintf(stderr, "  -c, --num-cards                 set number of input cards, including fake cards (default 2)\n");
	fprintf(stderr, "  -C, --num-fake-cards            set number of fake cards (default 0)\n");
	fprintf(stderr, "  -t, --theme=FILE                choose theme (default theme.lua)\n");
	fprintf(stderr, "  -v, --va-display=SPEC           VA-API device for H.264 encoding\n");
	fprintf(stderr, "                                    ($DISPLAY spec or /dev/dri/render* path)\n");
	fprintf(stderr, "  -m, --map-signal=SIGNAL,CARD    set a default card mapping (can be given multiple times)\n");
	fprintf(stderr, "      --http-uncompressed-video   send uncompressed NV12 video to HTTP clients\n");
	fprintf(stderr, "      --http-x264-video           send x264-compressed video to HTTP clients\n");
	fprintf(stderr, "      --x264-preset               x264 quality preset (default " X264_DEFAULT_PRESET ")\n");
	fprintf(stderr, "      --x264-tune                 x264 tuning (default " X264_DEFAULT_TUNE ", can be blank)\n");
	fprintf(stderr, "      --x264-speedcontrol         try to match x264 preset to available CPU speed\n");
	fprintf(stderr, "      --x264-speedcontrol-verbose  output speedcontrol debugging statistics\n");
	fprintf(stderr, "      --x264-bitrate              x264 bitrate (in kilobit/sec, default %d)\n",
		DEFAULT_X264_OUTPUT_BIT_RATE);
	fprintf(stderr, "      --x264-vbv-bufsize          x264 VBV size (in kilobits, 0 = one-frame VBV,\n");
	fprintf(stderr, "                                  default: same as --x264-bitrate, that is, one-second VBV)\n");
	fprintf(stderr, "      --x264-vbv-max-bitrate      x264 local max bitrate (in kilobit/sec per --vbv-bufsize,\n");
	fprintf(stderr, "                                  0 = no limit, default: same as --x264-bitrate, i.e., CBR)\n");
	fprintf(stderr, "      --x264-param=NAME[,VALUE]   set any x264 parameter, for fine tuning\n");
	fprintf(stderr, "      --http-mux=NAME             mux to use for HTTP streams (default " DEFAULT_STREAM_MUX_NAME ")\n");
	fprintf(stderr, "      --http-audio-codec=NAME     audio codec to use for HTTP streams\n");
	fprintf(stderr, "                                  (default is to use the same as for the recording)\n");
	fprintf(stderr, "      --http-audio-bitrate=KBITS  audio codec bit rate to use for HTTP streams\n");
	fprintf(stderr, "                                  (default is %d, ignored unless --http-audio-codec is set)\n",
		DEFAULT_AUDIO_OUTPUT_BIT_RATE / 1000);
	fprintf(stderr, "      --http-coarse-timebase      use less timebase for HTTP (recommended for muxers\n");
	fprintf(stderr, "                                  that handle large pts poorly, like e.g. MP4)\n");
	fprintf(stderr, "      --flat-audio                start with most audio processing turned off\n");
	fprintf(stderr, "                                    (can be overridden by e.g. --enable-limiter)\n");
	fprintf(stderr, "      --gain-staging=DB           set initial gain staging to the given value\n");
	fprintf(stderr, "                                    (--disable-gain-staging-auto)\n");
	fprintf(stderr, "      --disable-locut             turn off locut filter (also --enable)\n");
	fprintf(stderr, "      --disable-gain-staging-auto  turn off automatic gain staging (also --enable)\n");
	fprintf(stderr, "      --disable-compressor        turn off regular compressor (also --enable)\n");
	fprintf(stderr, "      --disable-limiter           turn off limiter (also --enable)\n");
	fprintf(stderr, "      --disable-makeup-gain-auto  turn off auto-adjustment of final makeup gain (also --enable)\n");
	fprintf(stderr, "      --disable-alsa-output       disable audio monitoring via ALSA\n");
	fprintf(stderr, "      --no-flush-pbos             do not explicitly signal texture data uploads\n");
	fprintf(stderr, "                                    (will give display corruption, but makes it\n");
	fprintf(stderr, "                                    possible to run with apitrace in real time)\n");
}

void parse_flags(int argc, char * const argv[])
{
	static const option long_options[] = {
		{ "help", no_argument, 0, 'h' },
		{ "num-cards", required_argument, 0, 'c' },
		{ "num-fake-cards", required_argument, 0, 'C' },
		{ "theme", required_argument, 0, 't' },
		{ "map-signal", required_argument, 0, 'm' },
		{ "va-display", required_argument, 0, OPTION_VA_DISPLAY },
		{ "http-uncompressed-video", no_argument, 0, OPTION_HTTP_UNCOMPRESSED_VIDEO },
		{ "http-x264-video", no_argument, 0, OPTION_HTTP_X264_VIDEO },
		{ "x264-preset", required_argument, 0, OPTION_X264_PRESET },
		{ "x264-tune", required_argument, 0, OPTION_X264_TUNE },
		{ "x264-speedcontrol", no_argument, 0, OPTION_X264_SPEEDCONTROL },
		{ "x264-speedcontrol-verbose", no_argument, 0, OPTION_X264_SPEEDCONTROL_VERBOSE },
		{ "x264-bitrate", required_argument, 0, OPTION_X264_BITRATE },
		{ "x264-vbv-bufsize", required_argument, 0, OPTION_X264_VBV_BUFSIZE },
		{ "x264-vbv-max-bitrate", required_argument, 0, OPTION_X264_VBV_MAX_BITRATE },
		{ "x264-param", required_argument, 0, OPTION_X264_PARAM },
		{ "http-mux", required_argument, 0, OPTION_HTTP_MUX },
		{ "http-coarse-timebase", no_argument, 0, OPTION_HTTP_COARSE_TIMEBASE },
		{ "http-audio-codec", required_argument, 0, OPTION_HTTP_AUDIO_CODEC },
		{ "http-audio-bitrate", required_argument, 0, OPTION_HTTP_AUDIO_BITRATE },
		{ "flat-audio", no_argument, 0, OPTION_FLAT_AUDIO },
		{ "gain-staging", required_argument, 0, OPTION_GAIN_STAGING },
		{ "disable-locut", no_argument, 0, OPTION_DISABLE_LOCUT },
		{ "enable-locut", no_argument, 0, OPTION_ENABLE_LOCUT },
		{ "disable-gain-staging-auto", no_argument, 0, OPTION_DISABLE_GAIN_STAGING_AUTO },
		{ "enable-gain-staging-auto", no_argument, 0, OPTION_ENABLE_GAIN_STAGING_AUTO },
		{ "disable-compressor", no_argument, 0, OPTION_DISABLE_COMPRESSOR },
		{ "enable-compressor", no_argument, 0, OPTION_ENABLE_COMPRESSOR },
		{ "disable-limiter", no_argument, 0, OPTION_DISABLE_LIMITER },
		{ "enable-limiter", no_argument, 0, OPTION_ENABLE_LIMITER },
		{ "disable-makeup-gain-auto", no_argument, 0, OPTION_DISABLE_MAKEUP_GAIN_AUTO },
		{ "enable-makeup-gain-auto", no_argument, 0, OPTION_ENABLE_MAKEUP_GAIN_AUTO },
		{ "disable-alsa-output", no_argument, 0, OPTION_DISABLE_ALSA_OUTPUT },
		{ "no-flush-pbos", no_argument, 0, OPTION_NO_FLUSH_PBOS },
		{ 0, 0, 0, 0 }
	};
	for ( ;; ) {
		int option_index = 0;
		int c = getopt_long(argc, argv, "c:C:t:v:m:", long_options, &option_index);

		if (c == -1) {
			break;
		}
		switch (c) {
		case 'c':
			global_flags.num_cards = atoi(optarg);
			break;
		case 'C':
			global_flags.num_fake_cards = atoi(optarg);
			break;
		case 't':
			global_flags.theme_filename = optarg;
			break;
		case 'm': {
			char *ptr = strchr(optarg, ',');
			if (ptr == nullptr) {
				fprintf(stderr, "ERROR: Invalid argument '%s' to --map-signal (needs a signal and a card number, separated by comma)\n", optarg);
				exit(1);
			}
			*ptr = '\0';
			const int signal_num = atoi(optarg);
			const int card_num = atoi(ptr + 1);
			if (global_flags.default_stream_mapping.count(signal_num)) {
				fprintf(stderr, "ERROR: Signal %d already mapped to card %d\n",
					signal_num, global_flags.default_stream_mapping[signal_num]);
				exit(1);
			}
			global_flags.default_stream_mapping[signal_num] = card_num;
			break;
		}
		case OPTION_VA_DISPLAY:
			global_flags.va_display = optarg;
			break;
		case OPTION_HTTP_UNCOMPRESSED_VIDEO:
			global_flags.uncompressed_video_to_http = true;
			break;
		case OPTION_HTTP_MUX:
			global_flags.stream_mux_name = optarg;
			break;
		case OPTION_HTTP_COARSE_TIMEBASE:
			global_flags.stream_coarse_timebase = true;
			break;
		case OPTION_HTTP_AUDIO_CODEC:
			global_flags.stream_audio_codec_name = optarg;
			break;
		case OPTION_HTTP_AUDIO_BITRATE:
			global_flags.stream_audio_codec_bitrate = atoi(optarg) * 1000;
			break;
		case OPTION_HTTP_X264_VIDEO:
			global_flags.x264_video_to_http = true;
			break;
		case OPTION_X264_PRESET:
			global_flags.x264_preset = optarg;
			break;
		case OPTION_X264_TUNE:
			global_flags.x264_tune = optarg;
			break;
		case OPTION_X264_SPEEDCONTROL:
			global_flags.x264_speedcontrol = true;
			break;
		case OPTION_X264_SPEEDCONTROL_VERBOSE:
			global_flags.x264_speedcontrol_verbose = true;
			break;
		case OPTION_X264_BITRATE:
			global_flags.x264_bitrate = atoi(optarg);
			break;
		case OPTION_X264_VBV_BUFSIZE:
			global_flags.x264_vbv_buffer_size = atoi(optarg);
			break;
		case OPTION_X264_VBV_MAX_BITRATE:
			global_flags.x264_vbv_max_bitrate = atoi(optarg);
			break;
		case OPTION_X264_PARAM:
			global_flags.x264_extra_param.push_back(optarg);
			break;
		case OPTION_FLAT_AUDIO:
			// If --flat-audio is given, turn off everything that messes with the sound,
			// except the final makeup gain.
			global_flags.locut_enabled = false;
			global_flags.gain_staging_auto = false;
			global_flags.compressor_enabled = false;
			global_flags.limiter_enabled = false;
			break;
		case OPTION_GAIN_STAGING:
			global_flags.initial_gain_staging_db = atof(optarg);
			global_flags.gain_staging_auto = false;
			break;
		case OPTION_DISABLE_LOCUT:
			global_flags.locut_enabled = false;
			break;
		case OPTION_ENABLE_LOCUT:
			global_flags.locut_enabled = true;
			break;
		case OPTION_DISABLE_GAIN_STAGING_AUTO:
			global_flags.gain_staging_auto = false;
			break;
		case OPTION_ENABLE_GAIN_STAGING_AUTO:
			global_flags.gain_staging_auto = true;
			break;
		case OPTION_DISABLE_COMPRESSOR:
			global_flags.compressor_enabled = false;
			break;
		case OPTION_ENABLE_COMPRESSOR:
			global_flags.compressor_enabled = true;
			break;
		case OPTION_DISABLE_LIMITER:
			global_flags.limiter_enabled = false;
			break;
		case OPTION_ENABLE_LIMITER:
			global_flags.limiter_enabled = true;
			break;
		case OPTION_DISABLE_MAKEUP_GAIN_AUTO:
			global_flags.final_makeup_gain_auto = false;
			break;
		case OPTION_ENABLE_MAKEUP_GAIN_AUTO:
			global_flags.final_makeup_gain_auto = true;
			break;
		case OPTION_DISABLE_ALSA_OUTPUT:
			global_flags.enable_alsa_output = false;
			break;
		case OPTION_NO_FLUSH_PBOS:
			global_flags.flush_pbos = false;
			break;
		case 'h':
			usage();
			exit(0);
		default:
			fprintf(stderr, "Unknown option '%s'\n", argv[option_index]);
			fprintf(stderr, "\n");
			usage();
			exit(1);
		}
	}

	if (global_flags.uncompressed_video_to_http &&
	    global_flags.x264_video_to_http) {
		fprintf(stderr, "ERROR: --http-uncompressed-video and --http-x264-video are mutually incompatible\n");
		exit(1);
	}
	if (global_flags.num_fake_cards > global_flags.num_cards) {
		fprintf(stderr, "ERROR: More fake cards then total cards makes no sense\n");
		exit(1);
	}
	if (global_flags.num_cards <= 0) {
		fprintf(stderr, "ERROR: --num-cards must be at least 1\n");
		exit(1);
	}
	if (global_flags.num_fake_cards < 0) {
		fprintf(stderr, "ERROR: --num-fake-cards cannot be negative\n");
		exit(1);
	}
	if (global_flags.x264_speedcontrol) {
		if (!global_flags.x264_preset.empty() && global_flags.x264_preset != "faster") {
			fprintf(stderr, "WARNING: --x264-preset is overridden by --x264-speedcontrol (implicitly uses \"faster\" as base preset)\n");
		}
		global_flags.x264_preset = "faster";
	} else if (global_flags.x264_preset.empty()) {
		global_flags.x264_preset = X264_DEFAULT_PRESET;
	}

	for (pair<int, int> mapping : global_flags.default_stream_mapping) {
		if (mapping.second >= global_flags.num_cards) {
			fprintf(stderr, "ERROR: Signal %d mapped to card %d, which doesn't exist (try adjusting --num-cards)\n",
				mapping.first, mapping.second);
			exit(1);
		}
	}
}
