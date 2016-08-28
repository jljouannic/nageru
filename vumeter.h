#ifndef VUMETER_H
#define VUMETER_H

#include <math.h>
#include <QLabel>
#include <QPaintEvent>
#include <QWidget>
#include <mutex>

#include "vu_common.h"

class VUMeter : public QWidget
{
	Q_OBJECT

public:
	VUMeter(QWidget *parent);

	void set_level(float level_lufs) {
		set_level(level_lufs, level_lufs);
	}

	void set_level(float level_lufs_left, float level_lufs_right) {
		std::unique_lock<std::mutex> lock(level_mutex);
		this->level_lufs[0] = level_lufs_left;
		this->level_lufs[1] = level_lufs_right;
		QMetaObject::invokeMethod(this, "update", Qt::AutoConnection);
	}

	double lufs_to_pos(float level_lu, int height)
	{
		return ::lufs_to_pos(level_lu, height, min_level, max_level);
	}

	void set_min_level(float min_level)
	{
		this->min_level = min_level;
		recalculate_pixmaps();
	}

	void set_max_level(float max_level)
	{
		this->max_level = max_level;
		recalculate_pixmaps();
	}

	void set_ref_level(float ref_level_lufs)
	{
		this->ref_level_lufs = ref_level_lufs;
	}

	void set_flip(bool flip)
	{
		this->flip = flip;
		recalculate_pixmaps();
	}

private:
	void resizeEvent(QResizeEvent *event) override;
	void paintEvent(QPaintEvent *event) override;
	void recalculate_pixmaps();

	std::mutex level_mutex;
	float level_lufs[2] { -HUGE_VALF, -HUGE_VALF };
	float min_level = -18.0f, max_level = 9.0f, ref_level_lufs = -23.0f;
	bool flip = false;

	QPixmap on_pixmap, off_pixmap;
};

#endif
