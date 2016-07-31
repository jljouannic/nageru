#ifndef _INPUT_MAPPING_DIALOG_H
#define _INPUT_MAPPING_DIALOG_H

#include <QDialog>
#include <string>
#include <vector>
#include <sys/time.h>

#include "mixer.h"

namespace Ui {
class InputMappingDialog;
}  // namespace Ui

class InputMappingDialog : public QDialog
{
	Q_OBJECT

public:
	InputMappingDialog();

private:
	void fill_ui_from_mapping(const InputMapping &mapping);
	void fill_row_from_input(unsigned row, const InputMapping::Input &input);
	void setup_channel_choices_from_input(unsigned row, const InputMapping::Input &input);
	void cell_changed(int row, int column);
	void card_selected(unsigned row, int index);
	void channel_selected(unsigned row, unsigned channel, int index);
	void ok_clicked();
	void cancel_clicked();
	void add_clicked();

	Ui::InputMappingDialog *ui;
	InputMapping mapping;  // Under edit. Will be committed on OK.

	// The old mapping. Will be re-committed on cancel, so that we
	// unhold all the unused devices (otherwise they would be
	// held forever).
	InputMapping old_mapping;

	const std::vector<std::string> card_names;
};

#endif  // !defined(_INPUT_MAPPING_DIALOG_H)
