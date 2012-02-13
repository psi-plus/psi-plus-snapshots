/*
 * Quick-and-dirty checker for updates.
 * Written by Aleksey Palazhchenko.
 * No rights reserved for this ugly code. This file is in public domain.
 */

#ifndef QDCHECKER_H
#define QDCHECKER_H

#include <QtCore/QObject>
class QWidget;
class QUrl;
class QNetworkAccessManager;

#include "AutoUpdater.h"

class QDChecker : public QObject, public AutoUpdater
{
	Q_OBJECT

public:
	QDChecker();
	virtual ~QDChecker();

	// from AutoUpdater
	virtual void checkForUpdates();

private slots:
	void onCheckFinished();
	void onDownloadLogFinished();
	void onOpenDownloadPage();

private:
	QNetworkAccessManager* manager_;
	QWidget* logForm_;

	static const QUrl updateCheckUrl_;
	static const QUrl changelogUrl_;
	static const QUrl downloadPageUrl_;

	static const QString settingsKey_;
};

#endif // QDCHECKER_H
