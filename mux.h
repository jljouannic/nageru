#ifndef _MUX_H
#define _MUX_H 1

// Wrapper around an AVFormat mux.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
}

#include <mutex>
#include <queue>
#include <vector>

class Mux {
public:
	enum Codec {
		CODEC_H264,
		CODEC_NV12,  // Uncompressed 4:2:0.
	};

	// Takes ownership of avctx. <keyframe_signal_receiver> can be nullptr.
	Mux(AVFormatContext *avctx, int width, int height, Codec video_codec, const std::string &video_extradata, const AVCodecContext *audio_ctx, int time_base);
	~Mux();
	void add_packet(const AVPacket &pkt, int64_t pts, int64_t dts);

	// As long as the mux is plugged, it will not actually write anything to disk,
	// just queue the packets. Once it is unplugged, the packets are reordered by pts
	// and written. This is primarily useful if you might have two different encoders
	// writing to the mux at the same time (because one is shutting down), so that
	// pts might otherwise come out-of-order.
	//
	// You can plug and unplug multiple times; only when the plug count reaches zero,
	// something will actually happen.
	void plug();
	void unplug();

private:
	void write_packet_or_die(const AVPacket &pkt);  // Must be called with <mu> held.

	std::mutex mu;
	AVFormatContext *avctx;  // Protected by <mu>.
	int plug_count = 0;  // Protected by <mu>.
	std::vector<AVPacket *> plugged_packets;  // Protected by <mu>.

	AVStream *avstream_video, *avstream_audio;
};

#endif  // !defined(_MUX_H)
