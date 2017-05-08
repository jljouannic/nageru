#ifndef _ANALYZER_H
#define _ANALYZER_H 1

#include <QDialog>
#include <QString>

#include <epoxy/gl.h>

class QObject;
class QOpenGLContext;
class QSurface;

namespace Ui {
class Analyzer;
}  // namespace Ui

namespace movit {
class ResourcePool;
}  // namespace movit

class Analyzer : public QDialog
{
	Q_OBJECT

public:
	Analyzer();
	~Analyzer();

private:
	void grab_clicked();

	Ui::Analyzer *ui;
	QSurface *surface;
	QOpenGLContext *context;
	GLuint pbo;
	movit::ResourcePool *resource_pool = nullptr;
};

#endif  // !defined(_ANALYZER_H)
