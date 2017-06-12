#ifndef _MUX_H
#define _MUX_H 1

// Wrapper around an AVFormat mux.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <sys/types.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

struct MuxMetrics {
	// “written” will usually be equal video + audio + mux overhead,
	// except that there could be buffered packets that count in audio or video
	// but not yet in written.
	std::atomic<int64_t> metric_video_bytes{0}, metric_audio_bytes{0}, metric_written_bytes{0};

	// Registers in global_metrics.
	void init(const std::vector<std::pair<std::string, std::string>> &labels);

	void reset()
	{
		metric_video_bytes = 0;
		metric_audio_bytes = 0;
		metric_written_bytes = 0;
	}
};

class Mux {
public:
	enum Codec {
		CODEC_H264,
		CODEC_NV12,  // Uncompressed 4:2:0.
	};

	// Takes ownership of avctx. <write_callback> will be called every time
	// a write has been made to the video stream (id 0), with the pts of
	// the just-written frame. (write_callback can be nullptr.)
	// Does not take ownership of <metrics>; elements in there, if any,
	// will be added to.
	Mux(AVFormatContext *avctx, int width, int height, Codec video_codec, const std::string &video_extradata, const AVCodecParameters *audio_codecpar, int time_base, std::function<void(int64_t)> write_callback, const std::vector<MuxMetrics *> &metrics);
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

	std::function<void(int64_t)> write_callback;
	std::vector<MuxMetrics *> metrics;
};

#endif  // !defined(_MUX_H)
