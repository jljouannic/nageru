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
	void fill_ui_from_mapping(const InputMapping &mapping, const std::vector<std::string> &card_names);
	void fill_channel_ui_from_mapping(unsigned row, const InputMapping::Input &input);

	Ui::InputMappingDialog *ui;
};

#endif  // !defined(_INPUT_MAPPING_DIALOG_H)
