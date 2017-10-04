#include "opt_input.h"

#include <QWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QCheckBox>
#include <QLocale>
//#include <QDebug>

#include "psioptions.h"
#include "spellchecker/spellchecker.h"


#include "ui_opt_input.h"

static const QString ENABLED_OPTION("options.ui.spell-check.enabled");
static const QString DICTS_OPTION("options.ui.spell-check.langs");
static const QString AUTORESIZE_OPTION("options.ui.chat.use-expanding-line-edit");
static const uint FullName = 0;


class OptInputUI : public QWidget, public Ui::OptInput
{
public:
	OptInputUI() : QWidget() { setupUi(this); }
};

OptionsTabInput::OptionsTabInput(QObject *parent)
: OptionsTab(parent, "input", "", tr("Input"), tr("Input options"), "psi/action_templates_edit"),
  w_(0),
  psi_(0)
{
}

OptionsTabInput::~OptionsTabInput()
{}

QWidget *OptionsTabInput::widget()
{
	if (w_) {
		return 0;
	}

	w_ = new OptInputUI();
	OptInputUI *d = (OptInputUI *)w_;

	availableDicts_ = SpellChecker::instance()->getAllLanguages();
	QStringList uiLangs = QLocale::system().uiLanguages();
	if(!uiLangs.isEmpty()) {
		foreach (QString loc, uiLangs) {
			if(availableDicts_.contains(loc.replace("-", "_"), Qt::CaseInsensitive)) {
				defaultLangs_ << loc;
			}
		}
	}

	d->isSpellCheck->setWhatsThis(tr("Check this option if you want your spelling to be checked"));

	connect(d->isSpellCheck, &QCheckBox::toggled, this, &OptionsTabInput::itemToggled);

	return w_;
}

void OptionsTabInput::applyOptions()
{
	if (!w_) {
		return;
	}

	OptInputUI *d = (OptInputUI *)w_;
	PsiOptions* o = PsiOptions::instance();
	SpellChecker *s = SpellChecker::instance();

	bool isEnabled = d->isSpellCheck->isChecked();
	o->setOption(ENABLED_OPTION, isEnabled);
	o->setOption(AUTORESIZE_OPTION, d->isAutoResize->isChecked());
	if(!isEnabled) {
		loadedDicts_.clear();
		s->setActiveLanguages(loadedDicts_);
	}
	else {
		d->groupBoxDicts->setEnabled(isEnabled);
		s->setActiveLanguages(loadedDicts_);
		o->setOption(DICTS_OPTION, QVariant(loadedDicts_.join(" ")));
	}
}

void OptionsTabInput::restoreOptions()
{
	if (!w_) {
		return;
	}

	OptInputUI *d = (OptInputUI *)w_;
	PsiOptions* o = PsiOptions::instance();

	updateDictLists();

	d->isAutoResize->setChecked( o->getOption(AUTORESIZE_OPTION).toBool() );
	bool isEnabled = o->getOption(ENABLED_OPTION).toBool();
	isEnabled = (!SpellChecker::instance()->available()) ? false : isEnabled;
	d->groupBoxDicts->setEnabled(isEnabled);
	d->isSpellCheck->setChecked(isEnabled);
	if (!availableDicts_.isEmpty()) {
		d->dictsWarnLabel->setVisible(false);
		if(isEnabled && isTreeViewEmpty()) {
			fillList();
		}
		if (isEnabled) {
			setChecked();
		}
	}
	else {
		d->dictsWarnLabel->setVisible(true);
	}

}

void OptionsTabInput::setData(PsiCon *psi, QWidget *)
{
	psi_ = psi;
}

void OptionsTabInput::updateDictLists()
{
	PsiOptions* o = PsiOptions::instance();
	QStringList newLoadedList = o->getOption(DICTS_OPTION).toString().split(QRegExp("\\s+"), QString::SkipEmptyParts);
	newLoadedList = (newLoadedList.isEmpty()) ? defaultLangs_ : newLoadedList;
	if(newLoadedList != loadedDicts_ || loadedDicts_.isEmpty()) {
		loadedDicts_ = newLoadedList;
	}
}

void OptionsTabInput::fillList()
{
	if(!w_) {
		return;
	}

	OptInputUI *d = (OptInputUI *)w_;

	if(!availableDicts_.isEmpty()) {
		d->availDicts->disconnect();
		d->availDicts->clear();
		foreach (const QString &item, availableDicts_) {
			QTreeWidgetItem *dic = new QTreeWidgetItem(d->availDicts, QTreeWidgetItem::Type);
			QLocale loc(item);
			dic->setText(FullName, QString("%1 - %2").arg(loc.nativeLanguageName()).arg(loc.nativeCountryName()));
			dic->setData(FullName, Qt::UserRole, item);
			if(!loadedDicts_.contains(item)) {
				dic->setCheckState(FullName, Qt::Unchecked);
				//qDebug() << "item" << item << "unchecked";
			}
			else {
				dic->setCheckState(FullName, Qt::Checked);
				//qDebug() << "item" << item << "checked";
			}
		}
		connect(d->availDicts, &QTreeWidget::itemChanged, this, &OptionsTabInput::itemChanged);
	}
}

void OptionsTabInput::setChecked()
{
	if(!w_) {
		return;
	}

	OptInputUI *d = (OptInputUI *)w_;
	QTreeWidgetItemIterator it(d->availDicts);
	while(*it) {
		QTreeWidgetItem *item = *it;
		QString itemText = item->data(FullName, Qt::UserRole).toString();
		Qt::CheckState state = loadedDicts_.contains(itemText, Qt::CaseInsensitive) ? Qt::Checked : Qt::Unchecked;
		if(state != item->checkState(FullName)) {
			item->setCheckState(FullName, state);
		}
		++it;
	}
}

void OptionsTabInput::itemToggled(bool toggled)
{
	if(!w_) {
		return;
	}

	OptInputUI *d = (OptInputUI *)w_;

	if(toggled) {
		updateDictLists();
		fillList();
		setChecked();
	}
	d->groupBoxDicts->setEnabled(toggled);
}

void OptionsTabInput::itemChanged(QTreeWidgetItem *item, int column)
{
	if ( !w_ )
		return;

	bool enabled = bool(item->checkState(column) == Qt::Checked);
	QString itemText = item->data(column, Qt::UserRole).toString();
	if(loadedDicts_.contains(itemText, Qt::CaseInsensitive) && !enabled) {
		loadedDicts_.removeOne(itemText);
	}
	else if (enabled){
		loadedDicts_ << itemText;
	}
	dataChanged();
}

bool OptionsTabInput::isTreeViewEmpty()
{
	if ( !w_ )
		return true;
	OptInputUI *d = (OptInputUI *)w_;
	QTreeWidgetItemIterator it(d->availDicts);
	return !bool(*it);
}
