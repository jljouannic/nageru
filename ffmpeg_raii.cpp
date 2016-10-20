#include "ffmpeg_raii.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/frame.h>
}

using namespace std;

// AVFormatContext

void avformat_close_input_unique(AVFormatContext *format_ctx)
{
	avformat_close_input(&format_ctx);
}

AVFormatContextWithCloser avformat_open_input_unique(
	const char *pathname, AVInputFormat *fmt, AVDictionary **options)
{
	AVFormatContext *format_ctx = nullptr;
	if (avformat_open_input(&format_ctx, pathname, fmt, options) != 0) {
		format_ctx = nullptr;
	}
	return AVFormatContextWithCloser(format_ctx, avformat_close_input_unique);
}


// AVCodecContext

void avcodec_free_context_unique(AVCodecContext *codec_ctx)
{
	avcodec_free_context(&codec_ctx);
}

AVCodecContextWithDeleter avcodec_alloc_context3_unique(const AVCodec *codec)
{
	return AVCodecContextWithDeleter(avcodec_alloc_context3(codec), avcodec_free_context_unique);
}


// AVCodecParameters

void avcodec_parameters_free_unique(AVCodecParameters *codec_par)
{
	avcodec_parameters_free(&codec_par);
}


// AVFrame

void av_frame_free_unique(AVFrame *frame)
{
	av_frame_free(&frame);
}

AVFrameWithDeleter av_frame_alloc_unique()
{
	return AVFrameWithDeleter(av_frame_alloc(), av_frame_free_unique);
}

