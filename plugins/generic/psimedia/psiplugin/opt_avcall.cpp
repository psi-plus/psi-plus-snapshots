#include "opt_avcall.h"

//#include "../avcall/avcall.h"
//#include "../avcall/mediadevicewatcher.h"
#include "../psimedia/psimedia.h"
//#include "common.h"
//#include "iconwidget.h"
//#include "psioptions.h"
#include "gstprovider.h"
#include "optionaccessinghost.h"
#include "ui_opt_avcall.h"

#include <QComboBox>
#include <QLineEdit>
#include <QList>

class OptAvCallUI : public QWidget, public Ui::OptAvCall {
public:
    OptAvCallUI() : QWidget() { setupUi(this); }
};

//----------------------------------------------------------------------------
// OptionsTabAvCall
//----------------------------------------------------------------------------

OptionsTabAvCall::OptionsTabAvCall(PsiMedia::Provider *provider, OptionAccessingHost *optHost, PsiMediaHost *mediaHost,
                                   QIcon icon) :
    _icon(icon),
    provider(provider), optHost(optHost), mediaHost(mediaHost)
{
    // connect(MediaDeviceWatcher::instance(), &MediaDeviceWatcher::updated, this, [this]() { restoreOptions(); });
}

OptionsTabAvCall::~OptionsTabAvCall() { delete features; }

QWidget *OptionsTabAvCall::widget()
{
    if (w)
        return nullptr;

    w = new OptAvCallUI();
    if (!features) {
        features = provider->createFeatures();
    }

    return w;
}

void OptionsTabAvCall::applyOptions()
{
    if (!w)
        return;

    OptAvCallUI *d = static_cast<OptAvCallUI *>(w.data());

    QString aout = d->cb_audioOutDevice->itemData(d->cb_audioOutDevice->currentIndex()).toString();
    QString ain  = d->cb_audioInDevice->itemData(d->cb_audioInDevice->currentIndex()).toString();
    QString vin  = d->cb_videoInDevice->itemData(d->cb_videoInDevice->currentIndex()).toString();

    optHost->setPluginOption("devices.audio-output", aout);
    optHost->setPluginOption("devices.audio-input", ain);
    optHost->setPluginOption("devices.video-input", vin);
    mediaHost->selectMediaDevices(ain, aout, vin);
}

void OptionsTabAvCall::restoreOptions()
{
    if (!w)
        return;

    OptAvCallUI *d = static_cast<OptAvCallUI *>(w.data());
    auto         devs
        = PsiMedia::FeaturesContext::AudioOut | PsiMedia::FeaturesContext::AudioIn | PsiMedia::FeaturesContext::VideoIn;

    auto handler = [this, d](const PsiMedia::PFeatures &features) {
        d->cb_audioOutDevice->clear();
        if (features.audioOutputDevices.isEmpty())
            d->cb_audioOutDevice->addItem("<None>", QString());
        for (const PsiMedia::PDevice &dev : features.audioOutputDevices)
            d->cb_audioOutDevice->addItem(dev.name, dev.id);

        d->cb_audioInDevice->clear();
        if (features.audioInputDevices.isEmpty())
            d->cb_audioInDevice->addItem("<None>", QString());
        for (const PsiMedia::PDevice &dev : features.audioInputDevices)
            d->cb_audioInDevice->addItem(dev.name, dev.id);

        d->cb_videoInDevice->clear();
        if (features.videoInputDevices.isEmpty())
            d->cb_videoInDevice->addItem("<None>", QString());
        for (const PsiMedia::PDevice &dev : features.videoInputDevices)
            d->cb_videoInDevice->addItem(dev.name, dev.id);

        auto ain  = optHost->getPluginOption("devices.audio-input", QString()).toString();
        auto aout = optHost->getPluginOption("devices.audio-output", QString()).toString();
        auto vin  = optHost->getPluginOption("devices.video-input", QString()).toString();

        if (!aout.isEmpty())
            d->cb_audioOutDevice->setCurrentIndex(d->cb_audioOutDevice->findData(aout));
        if (!ain.isEmpty())
            d->cb_audioInDevice->setCurrentIndex(d->cb_audioInDevice->findData(ain));
        if (!vin.isEmpty())
            d->cb_videoInDevice->setCurrentIndex(d->cb_videoInDevice->findData(vin));

        if (this->connectDataChanged) {
            this->connectDataChanged(w);

            // it's a hack to not play with other signals much
            this->connectDataChanged = std::function<void(QWidget *)>();
        }
    };

    features->lookup(devs, w, handler);
    // features->monitor(devs, w, handler);
}

QByteArray OptionsTabAvCall::id() const { return "avcall"; }

QByteArray OptionsTabAvCall::nextToId() const { return "sound"; }

QByteArray OptionsTabAvCall::parentId() const { return ""; }

QString OptionsTabAvCall::title() const { return QObject::tr("Multimedia"); }

QIcon OptionsTabAvCall::icon() const { return _icon; }

QString OptionsTabAvCall::desc() const { return QObject::tr("Audio and video device configuration"); }

void OptionsTabAvCall::setCallbacks(std::function<void()> dataChanged, std::function<void(bool)> noDirty,
                                    std::function<void(QWidget *)> connectDataChanged)
{
    this->dataChanged        = dataChanged;
    this->noDirty            = noDirty;
    this->connectDataChanged = connectDataChanged;
}
