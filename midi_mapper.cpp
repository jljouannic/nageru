#include "midi_mapper.h"
#include "midi_mapping.pb.h"

#include <alsa/asoundlib.h>
#include <google/protobuf/text_format.h>
#include <google/protobuf/io/zero_copy_stream.h>
#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <fcntl.h>
#include <sys/eventfd.h>

#include <functional>
#include <thread>

using namespace google::protobuf;
using namespace std;
using namespace std::placeholders;

namespace {

double map_controller_to_float(int val)
{
	// Slightly hackish mapping so that we can represent exactly 0.0, 0.5 and 1.0.
	if (val <= 0) {
		return 0.0;
	} else if (val >= 127) {
		return 1.0;
	} else {
		return (val + 0.5) / 127.0;
	}
}

}  // namespace

MIDIMapper::MIDIMapper(ControllerReceiver *receiver)
	: receiver(receiver), mapping_proto(new MIDIMappingProto)
{
	should_quit_fd = eventfd(/*initval=*/0, /*flags=*/0);
	assert(should_quit_fd != -1);
}

MIDIMapper::~MIDIMapper()
{
	should_quit = true;
	const uint64_t one = 1;
	write(should_quit_fd, &one, sizeof(one));
	midi_thread.join();
	close(should_quit_fd);
}

bool load_midi_mapping_from_file(const string &filename, MIDIMappingProto *new_mapping)
{
	// Read and parse the protobuf from disk.
	int fd = open(filename.c_str(), O_RDONLY);
	if (fd == -1) {
		perror(filename.c_str());
		return false;
	}
	io::FileInputStream input(fd);  // Takes ownership of fd.
	if (!TextFormat::Parse(&input, new_mapping)) {
		input.Close();
		return false;
	}
	input.Close();
	return true;
}

void MIDIMapper::set_midi_mapping(const MIDIMappingProto &new_mapping)
{
	if (mapping_proto) {
		mapping_proto->CopyFrom(new_mapping);
	} else {
		mapping_proto.reset(new MIDIMappingProto(new_mapping));
	}

	num_controller_banks = min(max(mapping_proto->num_controller_banks(), 1), 5);
        current_controller_bank = 0;
}

void MIDIMapper::start_thread()
{
	midi_thread = thread(&MIDIMapper::thread_func, this);
}

#define RETURN_ON_ERROR(msg, expr) do {                            \
	int err = (expr);                                          \
	if (err < 0) {                                             \
		fprintf(stderr, msg ": %s\n", snd_strerror(err));  \
		return;                                            \
	}                                                          \
} while (false)


void MIDIMapper::thread_func()
{
	snd_seq_t *seq;
	int err;

	RETURN_ON_ERROR("snd_seq_open", snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0));
	RETURN_ON_ERROR("snd_seq_nonblock", snd_seq_nonblock(seq, 1));
	RETURN_ON_ERROR("snd_seq_client_name", snd_seq_set_client_name(seq, "nageru"));
	RETURN_ON_ERROR("snd_seq_create_simple_port",
		snd_seq_create_simple_port(seq, "nageru",
			SND_SEQ_PORT_CAP_WRITE |
			SND_SEQ_PORT_CAP_SUBS_WRITE,
			SND_SEQ_PORT_TYPE_MIDI_GENERIC |
			SND_SEQ_PORT_TYPE_APPLICATION));

	// Listen to the announce port (0:1), which will tell us about new ports.
	RETURN_ON_ERROR("snd_seq_connect_from", snd_seq_connect_from(seq, 0, /*client=*/0, /*port=*/1));

	// Now go through all ports and subscribe to them.
	snd_seq_client_info_t *cinfo;
	snd_seq_client_info_alloca(&cinfo);

	snd_seq_client_info_set_client(cinfo, -1);
	while (snd_seq_query_next_client(seq, cinfo) >= 0) {
		int client = snd_seq_client_info_get_client(cinfo);

		snd_seq_port_info_t *pinfo;
		snd_seq_port_info_alloca(&pinfo);

		snd_seq_port_info_set_client(pinfo, client);
		snd_seq_port_info_set_port(pinfo, -1);
		while (snd_seq_query_next_port(seq, pinfo) >= 0) {
			constexpr int mask = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
			if ((snd_seq_port_info_get_capability(pinfo) & mask) == mask) {
				subscribe_to_port(seq, *snd_seq_port_info_get_addr(pinfo));
			}
		}
	}

	int num_alsa_fds = snd_seq_poll_descriptors_count(seq, POLLIN);
	unique_ptr<pollfd[]> fds(new pollfd[num_alsa_fds + 1]);

	while (!should_quit) {
		snd_seq_poll_descriptors(seq, fds.get(), num_alsa_fds, POLLIN);
		fds[num_alsa_fds].fd = should_quit_fd;
		fds[num_alsa_fds].events = POLLIN;
		fds[num_alsa_fds].revents = 0;

		err = poll(fds.get(), num_alsa_fds + 1, -1);
		if (err == 0 || (err == -1 && errno == EINTR)) {
			continue;
		}
		if (err == -1) {
			perror("poll");
			break;
		}
		if (fds[num_alsa_fds].revents) {
			// Activity on should_quit_fd.
			break;
		}

		// Seemingly we can get multiple events in a single poll,
		// and if we don't handle them all, poll will _not_ alert us!
		while (!should_quit) {
			snd_seq_event_t *event;
			err = snd_seq_event_input(seq, &event);
			if (err < 0) {
				if (err == -EINTR) continue;
				if (err == -EAGAIN) break;
				fprintf(stderr, "snd_seq_event_input: %s\n", snd_strerror(err));
				return;
			}
			if (event) {
				handle_event(seq, event);
			}
		}
	}
}

void MIDIMapper::handle_event(snd_seq_t *seq, snd_seq_event_t *event)
{
	switch (event->type) {
	case SND_SEQ_EVENT_CONTROLLER: {
		printf("Controller %d changed to %d\n", event->data.control.param, event->data.control.value);

		const int controller = event->data.control.param;
		const float value = map_controller_to_float(event->data.control.value);

		// Global controllers.
		match_controller(controller, MIDIMappingBusProto::kLocutFieldNumber, MIDIMappingProto::kLocutBankFieldNumber,
			value, bind(&ControllerReceiver::set_locut, receiver, _2));
		match_controller(controller, MIDIMappingBusProto::kLimiterThresholdFieldNumber, MIDIMappingProto::kLimiterThresholdBankFieldNumber,
			value, bind(&ControllerReceiver::set_limiter_threshold, receiver, _2));
		match_controller(controller, MIDIMappingBusProto::kMakeupGainFieldNumber, MIDIMappingProto::kMakeupGainBankFieldNumber,
			value, bind(&ControllerReceiver::set_makeup_gain, receiver, _2));

		// Bus controllers.
		match_controller(controller, MIDIMappingBusProto::kTrebleFieldNumber, MIDIMappingProto::kTrebleBankFieldNumber,
			value, bind(&ControllerReceiver::set_treble, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kMidFieldNumber, MIDIMappingProto::kMidBankFieldNumber,
			value, bind(&ControllerReceiver::set_mid, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kBassFieldNumber, MIDIMappingProto::kBassBankFieldNumber,
			value, bind(&ControllerReceiver::set_bass, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kGainFieldNumber, MIDIMappingProto::kGainBankFieldNumber,
			value, bind(&ControllerReceiver::set_gain, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kCompressorThresholdFieldNumber, MIDIMappingProto::kCompressorThresholdBankFieldNumber,
			value, bind(&ControllerReceiver::set_compressor_threshold, receiver, _1, _2));
		match_controller(controller, MIDIMappingBusProto::kFaderFieldNumber, MIDIMappingProto::kFaderBankFieldNumber,
			value, bind(&ControllerReceiver::set_fader, receiver, _1, _2));
		break;
	}
	case SND_SEQ_EVENT_NOTEON: {
		const int note = event->data.note.note;

		printf("Note: %d\n", note);

		// Bank change commands. TODO: Highlight the bank change in the UI.
		for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
			const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);
			if (bus_mapping.has_prev_bank() &&
			    bus_mapping.prev_bank().note_number() == note) {
				current_controller_bank = (current_controller_bank + num_controller_banks - 1) % num_controller_banks;
			}
			if (bus_mapping.has_next_bank() &&
			    bus_mapping.next_bank().note_number() == note) {
				current_controller_bank = (current_controller_bank + 1) % num_controller_banks;
			}
			if (bus_mapping.has_select_bank_1() &&
			    bus_mapping.select_bank_1().note_number() == note) {
				current_controller_bank = 0;
			}
			if (bus_mapping.has_select_bank_2() &&
			    bus_mapping.select_bank_2().note_number() == note &&
			    num_controller_banks >= 2) {
				current_controller_bank = 1;
			}
			if (bus_mapping.has_select_bank_3() &&
			    bus_mapping.select_bank_3().note_number() == note &&
			    num_controller_banks >= 3) {
				current_controller_bank = 2;
			}
			if (bus_mapping.has_select_bank_4() &&
			    bus_mapping.select_bank_4().note_number() == note &&
			    num_controller_banks >= 4) {
				current_controller_bank = 3;
			}
			if (bus_mapping.has_select_bank_5() &&
			    bus_mapping.select_bank_5().note_number() == note &&
			    num_controller_banks >= 5) {
				current_controller_bank = 4;
			}
		}

		match_button(note, MIDIMappingBusProto::kToggleLocutFieldNumber, MIDIMappingProto::kToggleLocutBankFieldNumber,
			bind(&ControllerReceiver::toggle_locut, receiver, _1));
		match_button(note, MIDIMappingBusProto::kToggleAutoGainStagingFieldNumber, MIDIMappingProto::kToggleAutoGainStagingBankFieldNumber,
			bind(&ControllerReceiver::toggle_auto_gain_staging, receiver, _1));
		match_button(note, MIDIMappingBusProto::kToggleCompressorFieldNumber, MIDIMappingProto::kToggleCompressorBankFieldNumber,
			bind(&ControllerReceiver::toggle_compressor, receiver, _1));
		match_button(note, MIDIMappingBusProto::kClearPeakFieldNumber, MIDIMappingProto::kClearPeakBankFieldNumber,
			bind(&ControllerReceiver::clear_peak, receiver, _1));
	}
	case SND_SEQ_EVENT_PORT_START:
		subscribe_to_port(seq, event->data.addr);
		break;
	case SND_SEQ_EVENT_PORT_EXIT:
		printf("MIDI port %d:%d went away.\n", event->data.addr.client, event->data.addr.port);
		break;
	case SND_SEQ_EVENT_NOTEOFF:
	case SND_SEQ_EVENT_CLIENT_START:
	case SND_SEQ_EVENT_CLIENT_EXIT:
	case SND_SEQ_EVENT_CLIENT_CHANGE:
	case SND_SEQ_EVENT_PORT_CHANGE:
	case SND_SEQ_EVENT_PORT_SUBSCRIBED:
	case SND_SEQ_EVENT_PORT_UNSUBSCRIBED:
		break;
	default:
		printf("Ignoring MIDI event of unknown type %d.\n", event->type);
	}
}

void MIDIMapper::subscribe_to_port(snd_seq_t *seq, const snd_seq_addr_t &addr)
{
	// Client 0 is basically the system; ignore it.
	if (addr.client == 0) {
		return;
	}

	int err = snd_seq_connect_from(seq, 0, addr.client, addr.port);
	if (err < 0) {
		// Just print out a warning (i.e., don't die); it could
		// very well just be e.g. another application.
		printf("Couldn't subscribe to MIDI port %d:%d (%s).\n",
			addr.client, addr.port, snd_strerror(err));
	} else {
		printf("Subscribed to MIDI port %d:%d.\n", addr.client, addr.port);
	}
}

void MIDIMapper::match_controller(int controller, int field_number, int bank_field_number, float value, function<void(unsigned, float)> func)
{
	if (bank_mismatch(bank_field_number)) {
		return;
	}

	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

		const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
		const Reflection *bus_reflection = bus_mapping.GetReflection();
		if (!bus_reflection->HasField(bus_mapping, descriptor)) {
			continue;
		}
		const MIDIControllerProto &controller_proto =
			static_cast<const MIDIControllerProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
		if (controller_proto.controller_number() == controller) {
			func(bus_idx, value);
		}
	}
}

void MIDIMapper::match_button(int note, int field_number, int bank_field_number, function<void(unsigned)> func)
{
	if (bank_mismatch(bank_field_number)) {
		return;
	}

	for (size_t bus_idx = 0; bus_idx < size_t(mapping_proto->bus_mapping_size()); ++bus_idx) {
		const MIDIMappingBusProto &bus_mapping = mapping_proto->bus_mapping(bus_idx);

		const FieldDescriptor *descriptor = bus_mapping.GetDescriptor()->FindFieldByNumber(field_number);
		const Reflection *bus_reflection = bus_mapping.GetReflection();
		if (!bus_reflection->HasField(bus_mapping, descriptor)) {
			continue;
		}
		const MIDIButtonProto &button_proto =
			static_cast<const MIDIButtonProto &>(bus_reflection->GetMessage(bus_mapping, descriptor));
		if (button_proto.note_number() == note) {
			func(bus_idx);
		}
	}
}

bool MIDIMapper::bank_mismatch(int bank_field_number)
{
	const FieldDescriptor *bank_descriptor = mapping_proto->GetDescriptor()->FindFieldByNumber(bank_field_number);
	const Reflection *reflection = mapping_proto->GetReflection();
	return (reflection->HasField(*mapping_proto, bank_descriptor) &&
 	        reflection->GetInt32(*mapping_proto, bank_descriptor) != current_controller_bank);
}
