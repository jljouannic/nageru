#ifndef _ANALYZER_H
#define _ANALYZER_H 1

#include <QImage>
#include <QMainWindow>
#include <QString>

#include <string>

#include <epoxy/gl.h>

#include "mixer.h"

class QObject;
class QOpenGLContext;
class QSurface;

namespace Ui {
class Analyzer;
}  // namespace Ui

namespace movit {
class ResourcePool;
}  // namespace movit

class Analyzer : public QMainWindow
{
	Q_OBJECT

public:
	Analyzer();
	~Analyzer();
	void update_channel_name(Mixer::Output output, const std::string &name);
	void mixer_shutting_down();

public slots:
	void relayout();

private:
	void grab_clicked();
	void signal_changed();
	bool eventFilter(QObject *watched, QEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;

	Ui::Analyzer *ui;
	QSurface *surface;
	QOpenGLContext *context;
	GLuint pbo;
	movit::ResourcePool *resource_pool = nullptr;
	QImage grabbed_image;
};

#endif  // !defined(_ANALYZER_H)
