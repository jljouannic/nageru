#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <chrono>
#include <string>
#include <vector>
#include <sys/time.h>

#include "mixer.h"

class GLWidget;
class QResizeEvent;

namespace Ui {
class AudioMiniView;
class Display;
class MainWindow;
}  // namespace Ui

class QLabel;
class QPushButton;

class MainWindow : public QMainWindow
{
	Q_OBJECT

public:
	MainWindow();
	void resizeEvent(QResizeEvent *event) override;
	void mixer_created(Mixer *mixer);

	// Used to release FBOs on the global ResourcePool. Call after the
	// mixer has been shut down but not destroyed yet.
	void mixer_shutting_down();

public slots:
	void cut_triggered();
	void x264_bitrate_triggered();
	void exit_triggered();
	void about_triggered();
	void transition_clicked(int transition_number);
	void channel_clicked(int channel_number);
	void wb_button_clicked(int channel_number);
	void set_transition_names(std::vector<std::string> transition_names);
	void update_channel_name(Mixer::Output output, const std::string &name);
	void update_channel_color(Mixer::Output output, const std::string &color);
	void gain_staging_knob_changed(int value);
	void final_makeup_gain_knob_changed(int value);
	void cutoff_knob_changed(int value);
	void limiter_threshold_knob_changed(int value);
	void compressor_threshold_knob_changed(int value);
	void mini_fader_changed(Ui::AudioMiniView *ui, int channel, int value);
	void reset_meters_button_clicked();
	void relayout();

private:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void set_white_balance(int channel_number, int x, int y);

	// Called from DiskSpaceEstimator.
	void report_disk_space(off_t free_bytes, double estimated_seconds_left);

	// Called from the mixer.
	void audio_level_callback(float level_lufs, float peak_db, float global_level_lufs, float range_low_lufs, float range_high_lufs, float gain_staging_db, float final_makeup_gain_db, float correlation);
	std::chrono::steady_clock::time_point last_audio_level_callback;

	Ui::MainWindow *ui;
	QLabel *disk_free_label;
	QPushButton *transition_btn1, *transition_btn2, *transition_btn3;
	std::vector<Ui::Display *> previews;
	int current_wb_pick_display = -1;
};

extern MainWindow *global_mainwindow;

#endif
