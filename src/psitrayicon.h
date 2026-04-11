#ifndef PSITRAYICON_H
#define PSITRAYICON_H

#include <QObject>

// On Linux with DBus support use KStatusNotifierItem instead of QSystemTrayIcon
#if defined(Q_OS_LINUX) && defined(QT_DBUS_LIB)
#define PSI_USE_KSNI 1
#endif

#ifndef PSI_USE_KSNI
#include <QSystemTrayIcon>
#endif

class PsiIcon;
class QMenu;
class QPixmap;
class QPoint;

#ifdef PSI_USE_KSNI
class KStatusNotifierItemPrivate;
#endif

class PsiTrayIcon : public QObject {
    Q_OBJECT
public:
    PsiTrayIcon(const QString &tip, QMenu *popup, QObject *parent = nullptr);
    ~PsiTrayIcon();

    void setContextMenu(QMenu *);
    void setToolTip(const QString &);
    void setIcon(const PsiIcon *, bool alert = false);
    void setAlert(const PsiIcon *);

signals:
    void clicked(const QPoint &, int);
    void doubleClicked(const QPoint &);
    void closed();
    void doToolTip(QObject *, QPoint);

public slots:
    void show();
    void hide();

private slots:
    void animate();
#ifndef PSI_USE_KSNI
    void trayicon_activated(QSystemTrayIcon::ActivationReason reason);
#endif

protected:
    QPixmap makeIcon();
    bool    eventFilter(QObject *, QEvent *);

private:
    PsiIcon  *icon_;
    quintptr  realIcon_;
    bool      isAlert_;

#ifdef PSI_USE_KSNI
    KStatusNotifierItemPrivate *ksni_;
#else
    QSystemTrayIcon *trayicon_;
#endif
};

#endif // PSITRAYICON_H
