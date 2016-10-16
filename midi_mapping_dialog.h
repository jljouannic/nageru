#ifndef _MIDI_MAPPING_DIALOG_H
#define _MIDI_MAPPING_DIALOG_H

#include <QDialog>
#include <string>
#include <vector>
#include <sys/time.h>

#include "audio_mixer.h"
#include "mixer.h"

namespace Ui {
class MIDIMappingDialog;
}  // namespace Ui

class MIDIMapper;
class MIDIMappingProto;
class QComboBox;
class QSpinBox;
class QTreeWidgetItem;

class MIDIMappingDialog : public QDialog
{
	Q_OBJECT

public:
	MIDIMappingDialog(MIDIMapper *mapper);
	~MIDIMappingDialog();

	// For use in midi_mapping_dialog.cpp only.
	struct Control {
		std::string label;
		int field_number;  // In MIDIMappingBusProto.
		int bank_field_number;  // In MIDIMappingProto.
	};

public slots:
	void ok_clicked();
	void cancel_clicked();
	void save_clicked();
	void load_clicked();

private:
	static constexpr unsigned num_buses = 8;

	void add_bank_selector(QTreeWidgetItem *item, const MIDIMappingProto &mapping_proto, int bank_field_number);
	
	enum class ControlType { CONTROLLER, BUTTON };
	void add_controls(const std::string &heading, ControlType control_type,
	                  const MIDIMappingProto &mapping_proto, const std::vector<Control> &controls);
	void fill_controls_from_mapping(const MIDIMappingProto &mapping_proto);

	std::unique_ptr<MIDIMappingProto> construct_mapping_proto_from_ui();


	Ui::MIDIMappingDialog *ui;
	MIDIMapper *mapper;

	// All controllers actually laid out on the grid (we need to store them
	// so that we can move values back and forth between the controls and
	// the protobuf on save/load).
	struct InstantiatedSpinner {
		QSpinBox *spinner;
		unsigned bus_idx;
		int field_number;  // In MIDIMappingBusProto.
	};
	struct InstantiatedComboBox {
		QComboBox *combo_box;
		int field_number;  // In MIDIMappingProto.
	};
	std::vector<InstantiatedSpinner> controller_spinners;
	std::vector<InstantiatedSpinner> button_spinners;
	std::vector<InstantiatedComboBox> bank_combo_boxes;
};

#endif  // !defined(_MIDI_MAPPING_DIALOG_H)
