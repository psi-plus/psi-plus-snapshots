#ifndef OPT_AVCALL_H
#define OPT_AVCALL_H

#include "optionaccessinghost.h"
#include "psimediahost.h"

#include <QIcon>

namespace PsiMedia {
class Provider;
class FeaturesContext;
}
class OptionAccessingHost;

class OptionsTabAvCall : public OAH_PluginOptionsTab {
public:
    OptionsTabAvCall(PsiMedia::Provider *provider, OptionAccessingHost *optHost, PsiMediaHost *mediaHost, QIcon icon);
    ~OptionsTabAvCall();

    QWidget *widget() override;

    void applyOptions() override;
    void restoreOptions() override;

    QByteArray id() const override;       // Unique identifier, i.e. "plugins_misha's_cool-plugin"
    QByteArray nextToId() const override; // the page will be added after this page
    QByteArray parentId() const override; // Identifier of parent tab, i.e. "general"

    QString title() const override; // "General"
    QIcon   icon() const override;
    QString desc() const override; // "You can configure your roster here"

    void setCallbacks(std::function<void()> dataChanged, std::function<void(bool)> noDirty,
                      std::function<void(QWidget *)> connectDataChanged) override;

private:
    QPointer<QWidget>          w;
    QIcon                      _icon;
    PsiMedia::Provider *       provider;
    PsiMedia::FeaturesContext *features  = nullptr;
    OptionAccessingHost *      optHost   = nullptr;
    PsiMediaHost *             mediaHost = nullptr;

    std::function<void()>          dataChanged;
    std::function<void(bool)>      noDirty;
    std::function<void(QWidget *)> connectDataChanged;
};

#endif // OPT_AVCALL_H
