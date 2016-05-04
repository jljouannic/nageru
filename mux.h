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

class KeyFrameSignalReceiver {
public:
	// Needs to automatically turn the flag off again after actually receiving data.
	virtual void signal_keyframe() = 0;
};

class Mux {
public:
	enum Codec {
		CODEC_H264,
		CODEC_NV12,  // Uncompressed 4:2:0.
	};

	// Takes ownership of avctx. <keyframe_signal_receiver> can be nullptr.
	Mux(AVFormatContext *avctx, int width, int height, Codec video_codec, const std::string &video_extradata, const AVCodecContext *audio_ctx, int time_base, KeyFrameSignalReceiver *keyframe_signal_receiver);
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
	void add_interleaved_packet(const AVPacket &pkt);  // Must be called with <mu> held.
	void write_packet_with_signal(const AVPacket &pkt);  // Must be called with <mu> held.

	std::mutex mu;
	AVFormatContext *avctx;  // Protected by <mu>.
	int plug_count = 0;  // Protected by <mu>.
	std::vector<AVPacket *> plugged_packets;  // Protected by <mu>.

	// We need to do our own interleaving since we do explicit flushes
	// before each keyframe. This queue contains every packet that we
	// couldn't send yet, in add order. Essentially, we can't send a packet
	// before we know we cannot receive an earlier (dts-wise) packet
	// from another stream. This means that this queue will either contain
	// video packets only or audio packets only, and as soon as a packet
	// of the other type comes in, we can empty the flush the queue up
	// to that point.
	// Protected by <mu>.
	std::queue<AVPacket *> waiting_packets;

	AVStream *avstream_video, *avstream_audio;
	KeyFrameSignalReceiver *keyframe_signal_receiver;
};

#endif  // !defined(_MUX_H)
