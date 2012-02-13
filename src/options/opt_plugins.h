#ifndef OPT_PLUGINS_H
#define OPT_PLUGINS_H

#include "optionstab.h"
#include "ui_plugininfodialog.h"
#include <QPointer>

class QWidget;
class Options;

class OptionsTabPlugins : public OptionsTab
{
	Q_OBJECT
public:
	OptionsTabPlugins(QObject *parent);
	~OptionsTabPlugins();

	QWidget *widget();
	void applyOptions();
	void restoreOptions();
	bool stretchable() const;

private:
	QWidget *w;
	QWidget *pluginWidget;
	bool state_;
	QPointer<QDialog> infoDialog;
	Ui::PluginInfoDialog ui_;

private slots:
	void listPlugins();
	void pluginSelected(int index);
	void showPluginInfo();
	//void loadToggled(int state);
};

#endif
