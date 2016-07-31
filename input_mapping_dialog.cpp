#include "input_mapping_dialog.h"

#include "ui_input_mapping.h"

#include <QComboBox>

using namespace std;
using namespace std::placeholders;

InputMappingDialog::InputMappingDialog()
	: ui(new Ui::InputMappingDialog),
	  mapping(global_mixer->get_audio_mixer()->get_input_mapping()),
	  old_mapping(mapping),
	  card_names(global_mixer->get_audio_mixer()->get_names())
{
	ui->setupUi(this);

	fill_ui_from_mapping(mapping);
	connect(ui->table, &QTableWidget::cellChanged, this, &InputMappingDialog::cell_changed);
	connect(ui->ok_cancel_buttons, &QDialogButtonBox::accepted, this, &InputMappingDialog::ok_clicked);
	connect(ui->ok_cancel_buttons, &QDialogButtonBox::rejected, this, &InputMappingDialog::cancel_clicked);
	connect(ui->add_button, &QPushButton::clicked, this, &InputMappingDialog::add_clicked);
	//connect(ui->add_button, &QPushButton::clicked, this, &InputMappingDialog::add_clicked);
}

void InputMappingDialog::fill_ui_from_mapping(const InputMapping &mapping)
{
	ui->table->verticalHeader()->hide();
	ui->table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
	ui->table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
	ui->table->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
	ui->table->horizontalHeader()->setSectionsClickable(false);

	ui->table->setRowCount(mapping.inputs.size());
	for (unsigned row = 0; row < mapping.inputs.size(); ++row) {
		fill_row_from_input(row, mapping.inputs[row]);
	}
}

void InputMappingDialog::fill_row_from_input(unsigned row, const InputMapping::Input &input)
{
	QString name(QString::fromStdString(input.name));
	ui->table->setItem(row, 0, new QTableWidgetItem(name));

	// Card choices.
	QComboBox *card_combo = new QComboBox;
	card_combo->addItem(QString("(none)   "));
	for (const string &name : card_names) {
		card_combo->addItem(QString::fromStdString(name + "   "));
	}
	switch (input.input_source_type) {
	case InputSourceType::SILENCE:
		card_combo->setCurrentIndex(0);
		break;
	case InputSourceType::CAPTURE_CARD:
		card_combo->setCurrentIndex(mapping.inputs[row].input_source_index + 1);
		break;
	default:
		assert(false);
	}
	connect(card_combo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		bind(&InputMappingDialog::card_selected, this, row, _1));
	ui->table->setCellWidget(row, 1, card_combo);

	setup_channel_choices_from_input(row, input);
}

void InputMappingDialog::setup_channel_choices_from_input(unsigned row, const InputMapping::Input &input)
{
	// Left and right channel.
	for (unsigned channel = 0; channel < 2; ++channel) {
		QComboBox *channel_combo = new QComboBox;
		channel_combo->addItem(QString("(none)"));
		if (input.input_source_type == InputSourceType::CAPTURE_CARD) {
			for (unsigned source = 0; source < 8; ++source) {  // TODO: Ask the card about number of channels, and names.
				char buf[256];
				snprintf(buf, sizeof(buf), "Channel %u   ", source + 1);
				channel_combo->addItem(QString(buf));
			}
			channel_combo->setCurrentIndex(input.source_channel[channel] + 1);
		} else {
			channel_combo->setCurrentIndex(0);
		}
		connect(channel_combo, static_cast<void(QComboBox::*)(int)>(&QComboBox::currentIndexChanged),
		        bind(&InputMappingDialog::channel_selected, this, row, channel, _1));
		ui->table->setCellWidget(row, 2 + channel, channel_combo);
	}
}

void InputMappingDialog::ok_clicked()
{
	global_mixer->get_audio_mixer()->set_input_mapping(mapping);
	accept();
}

void InputMappingDialog::cancel_clicked()
{
	global_mixer->get_audio_mixer()->set_input_mapping(old_mapping);
	reject();
}

void InputMappingDialog::cell_changed(int row, int column)
{
	if (column != 0) {
		// Spurious; only really the name column should fire these.
		return;
	}
	mapping.inputs[row].name = ui->table->item(row, column)->text().toStdString();
}

void InputMappingDialog::card_selected(unsigned row, int index)
{
	if (index == 0) {
		mapping.inputs[row].input_source_type = InputSourceType::SILENCE;
	} else {
		mapping.inputs[row].input_source_type = InputSourceType::CAPTURE_CARD;
		mapping.inputs[row].input_source_index = index - 1;
	}
	setup_channel_choices_from_input(row, mapping.inputs[row]);
}

void InputMappingDialog::channel_selected(unsigned row, unsigned channel, int index)
{
	mapping.inputs[row].source_channel[channel] = index - 1;
}

void InputMappingDialog::add_clicked()
{
	InputMapping::Input new_input;
	new_input.name = "New input";
	new_input.input_source_type = InputSourceType::SILENCE;
	mapping.inputs.push_back(new_input);
	ui->table->setRowCount(mapping.inputs.size());

	unsigned row = mapping.inputs.size() - 1;
	fill_row_from_input(row, new_input);
	ui->table->editItem(ui->table->item(row, 0));  // Start editing the name.
}
