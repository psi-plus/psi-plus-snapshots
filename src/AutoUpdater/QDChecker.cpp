/*
 * Quick-and-dirty checker for updates.
 * Written by Aleksey Palazhchenko.
 * No rights reserved for this ugly code. This file is in public domain.
 */

#include "QDChecker.h"
#include "ui_QDChangeLog.h"

#include <QUrl>
#include <QDesktopServices>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QNetworkProxy>

#include "psioptions.h"
#include "proxy.h"

const QUrl QDChecker::updateCheckUrl_ = QUrl("https://raw.githubusercontent.com/psi-plus/main/master/version.txt");
const QUrl QDChecker::changelogUrl_ = QUrl("https://raw.githubusercontent.com/psi-plus/main/master/changelog.txt");
const QUrl QDChecker::downloadPageUrl_ = QUrl("http://sourceforge.net/projects/psiplus/files/MS-Windows/Installers/0.16/");

const QString QDChecker::settingsKey_ = QLatin1String("options.auto-update.last-check-value");

QDChecker::QDChecker()
    : manager_(new QNetworkAccessManager(this)), logForm_(0)
{
    ProxyItem it = ProxyManager::instance()->getItemForObject("Auto Updater");
    ProxySettings ps = it.settings;
    if(!ps.host.isEmpty()) {
        QNetworkProxy prx(QNetworkProxy::HttpCachingProxy, ps.host, ps.port, ps.user, ps.pass);
        if(it.type == "socks")
            prx.setType(QNetworkProxy::Socks5Proxy);
        manager_->setProxy(prx);
    }
}

QDChecker::~QDChecker()
{
    delete logForm_;
}

void QDChecker::checkForUpdates()
{
    QNetworkRequest request(updateCheckUrl_);
    QNetworkReply* reply = manager_->get(request);
    connect(reply, SIGNAL(finished()), this, SLOT(onCheckFinished()));
}

void QDChecker::onCheckFinished()
{
    qDebug("onCheckFinished");
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if(reply && reply->error() == QNetworkReply::NoError) {
        const QByteArray data = reply->readAll();
        const quint16 newValue = qChecksum(data.constData(), data.size());
        const quint16 oldValue = PsiOptions::instance()->getOption(settingsKey_).toUInt();

        if(newValue != oldValue) {
            QNetworkRequest request(changelogUrl_);
            QNetworkReply* reply = manager_->get(request);
            connect(reply, SIGNAL(finished()), this, SLOT(onDownloadLogFinished()));
            PsiOptions::instance()->setOption(settingsKey_, newValue);
        }

        reply->deleteLater();
    } else {
        qCritical("WTF?! at %s %d", __FILE__, __LINE__);
    }
}

void QDChecker::onDownloadLogFinished()
{
    qDebug("onDownloadLogFinished");
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if(reply && reply->error() == QNetworkReply::NoError) {
        if(logForm_) {
            logForm_->deleteLater();
        }
        Ui::ShowChangeLogForm ui;
        logForm_ = new QWidget;
        ui.setupUi(logForm_);

        ui.logText->setText(QString::fromUtf8(reply->readAll()));

        connect(ui.buttonBox, SIGNAL(accepted()), this, SLOT(onOpenDownloadPage()));
        connect(ui.buttonBox, SIGNAL(clicked(QAbstractButton*)), logForm_, SLOT(hide()));
        logForm_->show();

        reply->deleteLater();
    } else {
        qCritical("WTF?! at %s %d", __FILE__, __LINE__);
    }
}

void QDChecker::onOpenDownloadPage()
{
    QDesktopServices::openUrl(downloadPageUrl_);
}
