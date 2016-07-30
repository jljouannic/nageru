#include "input_mapping_dialog.h"

#include "ui_input_mapping.h"

#include <QComboBox>

using namespace std;

InputMappingDialog::InputMappingDialog()
	: ui(new Ui::InputMappingDialog)
{
	ui->setupUi(this);

	//connect(ui->button_box, &QDialogButtonBox::accepted, [this]{ this->close(); });
	vector<string> card_names = global_mixer->get_audio_mixer()->get_names();
	fill_ui_from_mapping(global_mixer->get_audio_mixer()->get_input_mapping(), card_names);
}

void InputMappingDialog::fill_ui_from_mapping(const InputMapping &mapping, const vector<string> &card_names)
{
	ui->table->verticalHeader()->hide();
	ui->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	ui->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	ui->table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	ui->table->horizontalHeader()->setSectionsClickable(false);

	ui->table->setRowCount(mapping.inputs.size());
	for (unsigned row = 0; row < mapping.inputs.size(); ++row) {
		// TODO: Mark as some sort of header (by means of background color, probably).
		QString name(QString::fromStdString(mapping.inputs[row].name));
		ui->table->setItem(row, 0, new QTableWidgetItem(name));

		// Card choices.
		QComboBox *card_combo = new QComboBox;
		card_combo->addItem(QString("(none)   "));
		for (const string &name : card_names) {
			card_combo->addItem(QString::fromStdString(name + "   "));
		}
		switch (mapping.inputs[row].input_source_type) {
		case InputSourceType::SILENCE:
			card_combo->setCurrentIndex(0);
			break;
		case InputSourceType::CAPTURE_CARD:
			card_combo->setCurrentIndex(mapping.inputs[row].input_source_index + 1);
			break;
		default:
			assert(false);
		}
		ui->table->setCellWidget(row, 1, card_combo);

		// Left and right channel.
		fill_channel_ui_from_mapping(row, mapping.inputs[row]);
	}
}

void InputMappingDialog::fill_channel_ui_from_mapping(unsigned row, const InputMapping::Input &input)
{
	for (unsigned channel = 0; channel < 2; ++channel) {
		QComboBox *channel_combo = new QComboBox;
		channel_combo->addItem(QString("(none)"));
		if (input.input_source_type == InputSourceType::CAPTURE_CARD) {
			for (unsigned source = 0; source < 8; ++source) {  // TODO: Ask the card about number of channels, and names.
				char buf[256];
				snprintf(buf, sizeof(buf), "Channel %u   ", source + 1);
				channel_combo->addItem(QString(buf));
			}
		}
		channel_combo->setCurrentIndex(input.source_channel[channel] + 1);
		ui->table->setCellWidget(row, 2 + channel, channel_combo);
	}
}
