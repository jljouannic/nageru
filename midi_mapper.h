#ifndef _MIDI_MAPPER_H
#define _MIDI_MAPPER_H 1

// MIDIMapper is a class that listens for incoming MIDI messages from
// mixer controllers (ie., it is not meant to be used with regular
// instruments), interprets them according to a device-specific, user-defined
// mapping, and calls back into a receiver (typically the MainWindow).
// This way, it is possible to control audio functionality using physical
// pots and faders instead of the mouse.

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>

class MIDIMappingProto;

// Interface for receiving interpreted controller messages.
class ControllerReceiver {
public:
	// All values are [0.0, 1.0].
	virtual void set_locut(float value) = 0;
	virtual void set_limiter_threshold(float value) = 0;
	virtual void set_makeup_gain(float value) = 0;

	virtual void set_treble(unsigned bus_idx, float value) = 0;
	virtual void set_mid(unsigned bus_idx, float value) = 0;
	virtual void set_bass(unsigned bus_idx, float value) = 0;
	virtual void set_gain(unsigned bus_idx, float value) = 0;
	virtual void set_compressor_threshold(unsigned bus_idx, float value) = 0;
	virtual void set_fader(unsigned bus_idx, float value) = 0;

	virtual void toggle_locut(unsigned bus_idx) = 0;
	virtual void toggle_auto_gain_staging(unsigned bus_idx) = 0;
	virtual void toggle_compressor(unsigned bus_idx) = 0;
	virtual void clear_peak(unsigned bus_idx) = 0;
};

class MIDIMapper {
public:
	MIDIMapper(ControllerReceiver *receiver);
	virtual ~MIDIMapper();
	void set_midi_mapping(const MIDIMappingProto &new_mapping);
	void start_thread();
	const MIDIMappingProto &get_current_mapping() const { return *mapping_proto; }

private:
	void thread_func();
	void match_controller(int controller, int field_number, int bank_field_number, float value, std::function<void(unsigned, float)> func);
	void match_button(int note, int field_number, int bank_field_number, std::function<void(unsigned)> func);
	bool bank_mismatch(int bank_field_number);

	ControllerReceiver *receiver;
	std::atomic<bool> should_quit{false};
	int should_quit_fd;

	std::unique_ptr<MIDIMappingProto> mapping_proto;
	int num_controller_banks;
	int current_controller_bank = 0;

	std::thread midi_thread;
};

bool load_midi_mapping_from_file(const std::string &filename, MIDIMappingProto *new_mapping);

#endif  // !defined(_MIDI_MAPPER_H)
