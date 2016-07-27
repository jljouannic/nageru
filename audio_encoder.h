// A class to encode audio (using ffmpeg) and send it to a Mux.

#ifndef _AUDIO_ENCODER_H
#define _AUDIO_ENCODER_H 1

#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavresample/avresample.h>
#include <libavutil/frame.h>
}

#include "mux.h"

static inline void avcodec_parameters_free_unique(AVCodecParameters *codec_par)
{
	avcodec_parameters_free(&codec_par);
}

typedef std::unique_ptr<AVCodecParameters, decltype(avcodec_parameters_free_unique)*>
AVCodecParametersWithDeleter;

class AudioEncoder {
public:
	AudioEncoder(const std::string &codec_name, int bit_rate, const AVOutputFormat *oformat);
	~AudioEncoder();

	void add_mux(Mux *mux) {  // Does not take ownership.
		muxes.push_back(mux);
	}
	void encode_audio(const std::vector<float> &audio, int64_t audio_pts);
	void encode_last_audio();

	AVCodecParametersWithDeleter get_codec_parameters();

private:
	void encode_audio_one_frame(const float *audio, size_t num_samples, int64_t audio_pts);

	std::vector<float> audio_queue;
	int64_t last_pts = 0;  // The first pts after all audio we've encoded.

	AVCodecContext *ctx;
	AVAudioResampleContext *resampler;
	AVFrame *audio_frame = nullptr;
	std::vector<Mux *> muxes;
};

#endif  // !defined(_AUDIO_ENCODER_H)
