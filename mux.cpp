#include <assert.h>

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "defs.h"
#include "mux.h"
#include "timebase.h"

using namespace std;

struct PacketBefore {
	PacketBefore(const AVFormatContext *ctx) : ctx(ctx) {}

	bool operator() (const AVPacket *a, const AVPacket *b) const {
		int64_t a_dts = (a->dts == AV_NOPTS_VALUE ? a->pts : a->dts);
		int64_t b_dts = (b->dts == AV_NOPTS_VALUE ? b->pts : b->dts);
		AVRational a_timebase = ctx->streams[a->stream_index]->time_base;
		AVRational b_timebase = ctx->streams[b->stream_index]->time_base;
		if (av_compare_ts(a_dts, a_timebase, b_dts, b_timebase) != 0) {
			return av_compare_ts(a_dts, a_timebase, b_dts, b_timebase) < 0;
		} else {
			return av_compare_ts(a->pts, a_timebase, b->pts, b_timebase) < 0;
		}
	}

	const AVFormatContext * const ctx;
};

Mux::Mux(AVFormatContext *avctx, int width, int height, Codec video_codec, const string &video_extradata, const AVCodecContext *audio_ctx, int time_base, KeyFrameSignalReceiver *keyframe_signal_receiver)
	: avctx(avctx), keyframe_signal_receiver(keyframe_signal_receiver)
{
	AVCodec *codec_video = avcodec_find_encoder((video_codec == CODEC_H264) ? AV_CODEC_ID_H264 : AV_CODEC_ID_RAWVIDEO);
	avstream_video = avformat_new_stream(avctx, codec_video);
	if (avstream_video == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_video->time_base = AVRational{1, time_base};
	avstream_video->codec->codec_type = AVMEDIA_TYPE_VIDEO;
	if (video_codec == CODEC_H264) {
		avstream_video->codec->codec_id = AV_CODEC_ID_H264;
	} else {
		assert(video_codec == CODEC_NV12);
		avstream_video->codec->codec_id = AV_CODEC_ID_RAWVIDEO;
		avstream_video->codec->codec_tag = avcodec_pix_fmt_to_codec_tag(AV_PIX_FMT_NV12);
	}
	avstream_video->codec->width = width;
	avstream_video->codec->height = height;
	avstream_video->codec->time_base = AVRational{1, time_base};
	avstream_video->codec->ticks_per_frame = 1;  // or 2?

	// Colorspace details. Closely correspond to settings in EffectChain_finalize,
	// as noted in each comment.
	// Note that the H.264 stream also contains this information and depending on the
	// mux, this might simply get ignored. See sps_rbsp().
	avstream_video->codec->color_primaries = AVCOL_PRI_BT709;  // RGB colorspace (inout_format.color_space).
	avstream_video->codec->color_trc = AVCOL_TRC_UNSPECIFIED;  // Gamma curve (inout_format.gamma_curve).
	avstream_video->codec->colorspace = AVCOL_SPC_SMPTE170M;  // YUV colorspace (output_ycbcr_format.luma_coefficients).
	avstream_video->codec->color_range = AVCOL_RANGE_MPEG;  // Full vs. limited range (output_ycbcr_format.full_range).
	avstream_video->codec->chroma_sample_location = AVCHROMA_LOC_LEFT;  // Chroma sample location. See chroma_offset_0[] in Mixer::subsample_chroma().
	avstream_video->codec->field_order = AV_FIELD_PROGRESSIVE;

	if (!video_extradata.empty()) {
		avstream_video->codec->extradata = (uint8_t *)av_malloc(video_extradata.size());
		avstream_video->codec->extradata_size = video_extradata.size();
		memcpy(avstream_video->codec->extradata, video_extradata.data(), video_extradata.size());
	}

	avstream_audio = avformat_new_stream(avctx, nullptr);
	if (avstream_audio == nullptr) {
		fprintf(stderr, "avformat_new_stream() failed\n");
		exit(1);
	}
	avstream_audio->time_base = AVRational{1, time_base};
	avcodec_copy_context(avstream_audio->codec, audio_ctx);

	AVDictionary *options = NULL;
	vector<pair<string, string>> opts = MUX_OPTS;
	for (pair<string, string> opt : opts) {
		av_dict_set(&options, opt.first.c_str(), opt.second.c_str(), 0);
	}
	if (avformat_write_header(avctx, &options) < 0) {
		fprintf(stderr, "avformat_write_header() failed\n");
		exit(1);
	}

	// Make sure the header is written before the constructor exits.
	avio_flush(avctx->pb);
}

Mux::~Mux()
{
	av_write_trailer(avctx);
	av_free(avctx->pb->buffer);
	av_free(avctx->pb);
	avformat_free_context(avctx);
}

void Mux::add_packet(const AVPacket &pkt, int64_t pts, int64_t dts)
{
	AVPacket pkt_copy;
	if (av_copy_packet(&pkt_copy, &pkt) < 0) {
		fprintf(stderr, "av_copy_packet() failed\n");
		exit(1);
	}
	if (pkt.stream_index == 0) {
		pkt_copy.pts = av_rescale_q(pts, AVRational{1, TIMEBASE}, avstream_video->time_base);
		pkt_copy.dts = av_rescale_q(dts, AVRational{1, TIMEBASE}, avstream_video->time_base);
		pkt_copy.duration = av_rescale_q(pkt.duration, AVRational{1, TIMEBASE}, avstream_video->time_base);
	} else if (pkt.stream_index == 1) {
		pkt_copy.pts = av_rescale_q(pts, AVRational{1, TIMEBASE}, avstream_audio->time_base);
		pkt_copy.dts = av_rescale_q(dts, AVRational{1, TIMEBASE}, avstream_audio->time_base);
		pkt_copy.duration = av_rescale_q(pkt.duration, AVRational{1, TIMEBASE}, avstream_audio->time_base);
	} else {
		assert(false);
	}

	{
		lock_guard<mutex> lock(mu);
		if (plug_count > 0) {
			plugged_packets.push_back(av_packet_clone(&pkt_copy));
		} else {
			add_interleaved_packet(pkt_copy);
		}
	}

	av_packet_unref(&pkt_copy);
}

void Mux::add_interleaved_packet(const AVPacket &pkt)
{
	if (waiting_packets.empty() || waiting_packets.front()->stream_index == pkt.stream_index) {
		// We could still get packets of the other type with earlier pts/dts,
		// so we'll have to queue and wait.
		waiting_packets.push(av_packet_clone(const_cast<AVPacket *>(&pkt)));
		return;
	}

	// Flush all the queued packets that are supposed to go before this.
	PacketBefore before(avctx);
	while (!waiting_packets.empty() && !before(&pkt, waiting_packets.front())) {
		AVPacket *queued_pkt = waiting_packets.front();
		waiting_packets.pop();
		write_packet_with_signal(*queued_pkt);
		av_packet_unref(queued_pkt);
	}

	if (waiting_packets.empty()) {
		waiting_packets.push(av_packet_clone(const_cast<AVPacket *>(&pkt)));
	} else {
		write_packet_with_signal(pkt);
	}
}

void Mux::write_packet_with_signal(const AVPacket &pkt)
{
	if (keyframe_signal_receiver) {
		if (pkt.flags & AV_PKT_FLAG_KEY) {
			av_write_frame(avctx, nullptr);
			keyframe_signal_receiver->signal_keyframe();
		}
	}
	if (av_write_frame(avctx, const_cast<AVPacket *>(&pkt)) < 0) {
		fprintf(stderr, "av_interleaved_write_frame() failed\n");
		exit(1);
	}
}

void Mux::plug()
{
	lock_guard<mutex> lock(mu);
	++plug_count;
}

void Mux::unplug()
{
	lock_guard<mutex> lock(mu);
	if (--plug_count > 0) {
		return;
	}
	assert(plug_count >= 0);

	sort(plugged_packets.begin(), plugged_packets.end(), PacketBefore(avctx));

	for (AVPacket *pkt : plugged_packets) {
		add_interleaved_packet(*pkt);
		av_packet_free(&pkt);
	}
	plugged_packets.clear();
}
