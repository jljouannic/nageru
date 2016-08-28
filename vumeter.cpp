#include <QPainter>

#include "vumeter.h"
#include "vu_common.h"

using namespace std;

VUMeter::VUMeter(QWidget *parent)
	: QWidget(parent)
{
}

void VUMeter::resizeEvent(QResizeEvent *event)
{
	recalculate_pixmaps();
}

void VUMeter::paintEvent(QPaintEvent *event)
{
	QPainter painter(this);

	float level_lufs[2];
	{
		unique_lock<mutex> lock(level_mutex);
		level_lufs[0] = this->level_lufs[0];
		level_lufs[1] = this->level_lufs[1];
	}

	int mid = width() / 2;

	for (unsigned channel = 0; channel < 2; ++channel) {
		int left = (channel == 0) ? 0 : mid;
		int right = (channel == 0) ? mid : width();
		float level_lu = level_lufs[channel] - ref_level_lufs;
		int on_pos = lrint(lufs_to_pos(level_lu, height()));

		if (flip) {
			QRect on_rect(left, 0, right, height() - on_pos);
			QRect off_rect(left, height() - on_pos, right, height());

			painter.drawPixmap(on_rect, on_pixmap, on_rect);
			painter.drawPixmap(off_rect, off_pixmap, off_rect);
		} else {
			QRect off_rect(left, 0, right, on_pos);
			QRect on_rect(left, on_pos, right, height() - on_pos);

			painter.drawPixmap(off_rect, off_pixmap, off_rect);
			painter.drawPixmap(on_rect, on_pixmap, on_rect);
		}
	}
}

void VUMeter::recalculate_pixmaps()
{
	on_pixmap = QPixmap(width(), height());
	QPainter on_painter(&on_pixmap);
	draw_vu_meter(on_painter, width(), height(), 0, true, min_level, max_level, flip);

	off_pixmap = QPixmap(width(), height());
	QPainter off_painter(&off_pixmap);
	draw_vu_meter(off_painter, width(), height(), 0, false, min_level, max_level, flip);
}
