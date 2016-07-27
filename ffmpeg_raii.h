#ifndef _FFMPEG_RAII_H
#define _FFMPEG_RAII_H 1

// Some helpers to make RAII versions of FFmpeg objects.
// The cleanup functions don't interact all that well with unique_ptr,
// so things get a bit messy and verbose, but overall it's worth it to ensure
// we never leak things by accident in error paths.
//
// This does not cover any of the types that can actually be declared as
// a unique_ptr with no helper functions for deleter.

#include <memory>

struct AVCodec;
struct AVCodecContext;
struct AVCodecParameters;
struct AVDictionary;
struct AVFormatContext;
struct AVFrame;
struct AVInputFormat;


// AVFormatContext
void avformat_close_input_unique(AVFormatContext *format_ctx);

typedef std::unique_ptr<AVFormatContext, decltype(avformat_close_input_unique)*>
	AVFormatContextWithCloser;

AVFormatContextWithCloser avformat_open_input_unique(
	const char *pathname, AVInputFormat *fmt, AVDictionary **options);


// AVCodecContext
void avcodec_free_context_unique(AVCodecContext *codec_ctx);

typedef std::unique_ptr<AVCodecContext, decltype(avcodec_free_context_unique)*>
	AVCodecContextWithDeleter;

AVCodecContextWithDeleter avcodec_alloc_context3_unique(const AVCodec *codec);


// AVCodecParameters
void avcodec_parameters_free_unique(AVCodecParameters *codec_par);

typedef std::unique_ptr<AVCodecParameters, decltype(avcodec_parameters_free_unique)*>
	AVCodecParametersWithDeleter;


// AVFrame
void av_frame_free_unique(AVFrame *frame);

typedef std::unique_ptr<AVFrame, decltype(av_frame_free_unique)*>
	AVFrameWithDeleter;

AVFrameWithDeleter av_frame_alloc_unique();

#endif  // !defined(_FFMPEG_RAII_H)
