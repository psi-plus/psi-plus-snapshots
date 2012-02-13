#include "opt_plugins.h"
#include "common.h"
#include "iconwidget.h"
#include "pluginmanager.h"
#include "psioptions.h"
#include "psiiconset.h"

#include <QWhatsThis>
#include <QCheckBox>
#include <QComboBox>
#include <QButtonGroup>
#include <QRadioButton>

#include "ui_opt_plugins.h"

class OptPluginsUI : public QWidget, public Ui::OptPlugins
{
public:
	OptPluginsUI() : QWidget() { setupUi(this); }
};

//----------------------------------------------------------------------------
// OptionsTabPlugins
//----------------------------------------------------------------------------

OptionsTabPlugins::OptionsTabPlugins(QObject *parent)
	: OptionsTab(parent, "plugins", "", tr("Plugins"), tr("Options for Psi plugins"), "psi/plugins")
	, w(0)
{
}

OptionsTabPlugins::~OptionsTabPlugins()
{
	if(infoDialog)
		delete(infoDialog);
}

QWidget *OptionsTabPlugins::widget()
{
	if ( w )
		return 0;

	w = new OptPluginsUI();
	OptPluginsUI *d = (OptPluginsUI *)w;

	d->pb_info->setIcon(QIcon(IconsetFactory::iconPixmap("psi/info")));

	listPlugins();

	
	/*d->ck_messageevents->setWhatsThis(
		tr("Enables the sending and requesting of message events such as "
		"'Contact is Typing', ..."));*/

	connect(d->cb_plugins,SIGNAL(currentIndexChanged(int)),SLOT(pluginSelected(int)));
	//connect(d->cb_loadPlugin,SIGNAL(stateChanged(int)),SLOT(loadToggled(int)));
	connect(d->pb_info, SIGNAL(clicked()), SLOT(showPluginInfo()));
	
	return w;
}

void OptionsTabPlugins::applyOptions()
{
	if ( !w )
		return;

	OptPluginsUI *d = (OptPluginsUI *)w;
	QString pluginName=d->cb_plugins->currentText();
	bool value=d->cb_loadPlugin->isChecked();
	if(value != state_) {
		PluginManager::instance()->loadUnloadPlugin(d->cb_plugins->currentText(), value);
		pluginSelected(0);
	}

	if(state_)
		PluginManager::instance()->applyOptions( pluginName );
}

void OptionsTabPlugins::restoreOptions()
{
	if ( !w )
		return;

	if(state_) {
		OptPluginsUI *d = (OptPluginsUI *)w;
		QString pluginName=d->cb_plugins->currentText();
		PluginManager::instance()->restoreOptions( pluginName );
	}
}

bool OptionsTabPlugins::stretchable() const
{
	return true;
}


void OptionsTabPlugins::listPlugins()
{
  	if ( !w )
		return;

	OptPluginsUI *d = (OptPluginsUI *)w;

	d->cb_plugins->clear();
	
	PluginManager *pm=PluginManager::instance();
	
	QStringList plugins = pm->availablePlugins();
	plugins.sort();
	foreach (QString plugin, plugins){
		d->cb_plugins->addItem(plugin);
	}
	pluginSelected(0);
}

/*void OptionsTabPlugins::loadToggled(int state)
{
	Q_UNUSED(state);
	if ( !w )
		return;
	
	OptPluginsUI *d = (OptPluginsUI *)w;
	
	QString option=QString("%1.%2")
		.arg(PluginManager::loadOptionPrefix)
		.arg(PluginManager::instance()->shortName(d->cb_plugins->currentText()));
	bool value=d->cb_loadPlugin->isChecked(); 
	PsiOptions::instance()->setOption(option, value);
}*/

void OptionsTabPlugins::pluginSelected(int index)
{
	Q_UNUSED(index);
  	if ( !w )
		return;
	
	OptPluginsUI *d = (OptPluginsUI *)w;
	d->le_location->setText(tr("No plugin selected."));
	d->cb_loadPlugin->setEnabled(false);
	d->pb_info->setEnabled(false);
	if(infoDialog)
		delete(infoDialog);

	if ( d->cb_plugins->count() > 0 ) {
		QString pluginName = d->cb_plugins->currentText();
		d->le_location->setText(PluginManager::instance()->pathToPlugin( pluginName ));
		d->cb_loadPlugin->setEnabled(true);
		QWidget* pluginOptions = PluginManager::instance()->optionsWidget( pluginName );
		d->cb_plugins->setEnabled(true);
		d->version->setText(tr("Version: ")+PluginManager::instance()->version( pluginName ));
		QString option=QString("%1.%2")
			.arg(PluginManager::loadOptionPrefix)
			.arg(PluginManager::instance()->shortName(pluginName));
		d->cb_loadPlugin->setChecked(PsiOptions::instance()->getOption(option, false).toBool());
		state_ = d->cb_loadPlugin->isChecked();
		pluginOptions->setParent(d);
		qWarning("Showing Plugin options");
		d->vboxLayout1->addWidget(pluginOptions);
		emit connectDataChanged(w);
		//d->pluginOptions->show();
		//d->updateGeometry();
		d->pb_info->setEnabled(PluginManager::instance()->hasInfoProvider(d->cb_plugins->currentText()));
	}
}

void OptionsTabPlugins::showPluginInfo()
{
	if(infoDialog)
		infoDialog->raise();
	else {
		OptPluginsUI *d = (OptPluginsUI *)w;
		infoDialog = new QDialog();
		infoDialog->setWindowIcon(QIcon(IconsetFactory::iconPixmap("psi/logo_128")));
		ui_.setupUi(infoDialog);
		ui_.te_info->setText(PluginManager::instance()->pluginInfo(d->cb_plugins->currentText()));
		infoDialog->setAttribute(Qt::WA_DeleteOnClose);
		infoDialog->show();
	}
}
