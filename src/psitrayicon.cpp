#include "psitrayicon.h"
#include "alerticon.h"
#include "common.h"
#include "iconset.h"

#include <QHelpEvent>
#include <QPixmap>
#include <QPixmapCache>
#include <QBuffer>
#include <QByteArray>
#include <QImage>

#if defined(Q_OS_LINUX) && defined(QT_DBUS_LIB)
#define PSI_USE_KSNI 1
#endif

#ifdef PSI_USE_KSNI
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusInterface>
#include <QDBusMetaType>
#include <QMenu>
#include <QApplication>
#include <QTimer>

// DBus type for icon pixmap: array of (int, int, bytes)
struct KSNIIconPixmap {
    int width;
    int height;
    QByteArray data; // ARGB32 big-endian
};
Q_DECLARE_METATYPE(KSNIIconPixmap)
Q_DECLARE_METATYPE(QList<KSNIIconPixmap>)

QDBusArgument &operator<<(QDBusArgument &arg, const KSNIIconPixmap &icon)
{
    arg.beginStructure();
    arg << icon.width << icon.height << icon.data;
    arg.endStructure();
    return arg;
}

const QDBusArgument &operator>>(const QDBusArgument &arg, KSNIIconPixmap &icon)
{
    arg.beginStructure();
    arg >> icon.width >> icon.height >> icon.data;
    arg.endStructure();
    return arg;
}

// Convert QPixmap to ARGB32 big-endian bytes as required by KSNi spec
static QByteArray pixmapToKSNIBytes(const QPixmap &pixmap)
{
    QImage img = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
    QByteArray bytes;
    bytes.resize(img.width() * img.height() * 4);
    // KSNi expects ARGB in network byte order (big-endian)
    for (int y = 0; y < img.height(); ++y) {
        for (int x = 0; x < img.width(); ++x) {
            QRgb pixel = img.pixel(x, y);
            int idx = (y * img.width() + x) * 4;
            bytes[idx + 0] = qAlpha(pixel);
            bytes[idx + 1] = qRed(pixel);
            bytes[idx + 2] = qGreen(pixel);
            bytes[idx + 3] = qBlue(pixel);
        }
    }
    return bytes;
}

// KStatusNotifierItem DBus adaptor
class KStatusNotifierItemPrivate : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.StatusNotifierItem")

public:
    // Properties exposed via DBus
    Q_PROPERTY(QString Category READ category)
    Q_PROPERTY(QString Id READ id)
    Q_PROPERTY(QString Title READ title)
    Q_PROPERTY(QString Status READ status NOTIFY NewStatus)
    Q_PROPERTY(QString IconName READ iconName)
    Q_PROPERTY(QList<KSNIIconPixmap> IconPixmap READ iconPixmap)
    Q_PROPERTY(QString AttentionIconName READ attentionIconName)
    Q_PROPERTY(QList<KSNIIconPixmap> AttentionIconPixmap READ attentionIconPixmap)
    Q_PROPERTY(QString OverlayIconName READ overlayIconName)
    Q_PROPERTY(QString ToolTipIconName READ toolTipIconName)
    Q_PROPERTY(QString ToolTipTitle READ toolTipTitle)
    Q_PROPERTY(QString ToolTipSubTitle READ toolTipSubTitle)
    Q_PROPERTY(QDBusObjectPath Menu READ menu)
    Q_PROPERTY(bool ItemIsMenu READ itemIsMenu)

    QString category() const { return QStringLiteral("Communications"); }
    QString id() const { return QStringLiteral("Psi+"); }
    QString title() const { return toolTip_; }
    QString status() const { return isAlert_ ? QStringLiteral("NeedsAttention") : QStringLiteral("Active"); }
    QString iconName() const { return QString(); }
    QList<KSNIIconPixmap> iconPixmap() const { return isAlert_ ? QList<KSNIIconPixmap>() : iconPixmaps_; }
    QString attentionIconName() const { return QString(); }
    QList<KSNIIconPixmap> attentionIconPixmap() const { return isAlert_ ? iconPixmaps_ : QList<KSNIIconPixmap>(); }
    QString overlayIconName() const { return QString(); }
    QString toolTipIconName() const { return QString(); }
    QString toolTipTitle() const { return toolTip_; }
    QString toolTipSubTitle() const { return QString(); }
    QDBusObjectPath menu() const { return QDBusObjectPath(menuPath_); }
    bool itemIsMenu() const { return false; }

    explicit KStatusNotifierItemPrivate(QObject *parent = nullptr)
        : QObject(parent), isAlert_(false), visible_(false), serviceRegistered_(false)
    {
        qDBusRegisterMetaType<KSNIIconPixmap>();
        qDBusRegisterMetaType<QList<KSNIIconPixmap>>();

        pid_ = QCoreApplication::applicationPid();
        serviceName_ = QString("org.kde.StatusNotifierItem-%1-1").arg(pid_);
        objectPath_ = QStringLiteral("/StatusNotifierItem");
        menuPath_ = QStringLiteral("/MenuBar");

        // Debounce timer — prevents rapid status flicker confusing the shell
        statusDebounceTimer_ = new QTimer(this);
        statusDebounceTimer_->setSingleShot(true);
        statusDebounceTimer_->setInterval(150);
        connect(statusDebounceTimer_, &QTimer::timeout,
                this, &KStatusNotifierItemPrivate::emitNewStatusNow);
    }

    ~KStatusNotifierItemPrivate()
    {
        unregister();
    }

    void registerWithWatcher()
    {
        QDBusConnection bus = QDBusConnection::sessionBus();

        if (!bus.isConnected())
            return;

        // Register our service name
        if (!bus.registerService(serviceName_)) {
            // Try with -2, -3 etc
            for (int i = 2; i < 10; ++i) {
                serviceName_ = QString("org.kde.StatusNotifierItem-%1-%2").arg(pid_).arg(i);
                if (bus.registerService(serviceName_))
                    break;
            }
        }

        // Register our object
        bus.registerObject(objectPath_, this,
                           QDBusConnection::ExportAllProperties |
                           QDBusConnection::ExportAllSignals |
                           QDBusConnection::ExportAllSlots);

        // Register with StatusNotifierWatcher
        QDBusInterface watcher(QStringLiteral("org.kde.StatusNotifierWatcher"),
                               QStringLiteral("/StatusNotifierWatcher"),
                               QStringLiteral("org.kde.StatusNotifierWatcher"),
                               bus);
        if (watcher.isValid()) {
            watcher.call(QStringLiteral("RegisterStatusNotifierItem"), serviceName_);
            serviceRegistered_ = true;
        }
    }

    void unregister()
    {
        if (!serviceRegistered_)
            return;
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.unregisterObject(objectPath_);
        bus.unregisterService(serviceName_);
        serviceRegistered_ = false;
    }

    void updateIcon(const QPixmap &pixmap, bool alert)
    {
        isAlert_ = alert;
        iconPixmaps_.clear();

        if (!pixmap.isNull()) {
            KSNIIconPixmap kp;
            kp.width  = pixmap.width();
            kp.height = pixmap.height();
            kp.data   = pixmapToKSNIBytes(pixmap);
            iconPixmaps_.append(kp);
        }

        // Notify shell that icon and status changed
        emitNewIcon();
        emitNewStatus();
    }

    void updateTooltip(const QString &tip)
    {
        toolTip_ = tip;
        emitNewToolTip();
    }

    void setVisible(bool v)
    {
        if (visible_ == v)
            return;
        visible_ = v;
        if (v && !serviceRegistered_)
            registerWithWatcher();
        else if (!v)
            unregister();
    }

signals:
    // KSNi spec signals
    Q_SCRIPTABLE void NewIcon();
    Q_SCRIPTABLE void NewAttentionIcon();
    Q_SCRIPTABLE void NewOverlayIcon();
    Q_SCRIPTABLE void NewToolTip();
    Q_SCRIPTABLE void NewStatus(const QString &status);

    // Internal signals to PsiTrayIcon
    void activated(int button);
    void scrollRequested(int delta, const QString &orientation);

public slots:
    Q_SCRIPTABLE void Activate(int x, int y)
    {
        Q_UNUSED(x) Q_UNUSED(y)
        emit activated(Qt::LeftButton);
    }

    Q_SCRIPTABLE void SecondaryActivate(int x, int y)
    {
        Q_UNUSED(x) Q_UNUSED(y)
        emit activated(Qt::MiddleButton);
    }

    Q_SCRIPTABLE void ContextMenu(int x, int y)
    {
        Q_UNUSED(x) Q_UNUSED(y)
        if (menu_) {
            menu_->popup(QPoint(x, y));
        }
    }

    Q_SCRIPTABLE void Scroll(int delta, const QString &orientation)
    {
        emit scrollRequested(delta, orientation);
    }

public:
    QMenu   *menu_ = nullptr;

private:
    void emitNewIcon()
    {
        emit NewIcon();
        emit NewAttentionIcon();
    }
    void emitNewStatus()
    {
        if (isAlert_) {
            // NeedsAttention — send immediately so animation works
            statusDebounceTimer_->stop();
            emit NewStatus(status());
        } else {
            // Active — debounce to avoid rapid flicker confusing the shell
            statusDebounceTimer_->start();
        }
    }

    Q_SLOT void emitNewStatusNow()
    {
        emit NewStatus(status());
    }
    void emitNewToolTip()
    {
        emit NewToolTip();
    }

    QString toolTip_;
    bool    isAlert_;
    bool    visible_;
    bool    serviceRegistered_;
    qint64  pid_;
    QString serviceName_;
    QString objectPath_;
    QString menuPath_;
    QList<KSNIIconPixmap> iconPixmaps_;
    QTimer *statusDebounceTimer_;
};

#include "psitrayicon.moc"

// ==================== PsiTrayIcon (KSNi implementation) ====================

PsiTrayIcon::PsiTrayIcon(const QString &tip, QMenu *popup, QObject *parent)
    : QObject(parent), icon_(nullptr), realIcon_(0), isAlert_(false),
      ksni_(new KStatusNotifierItemPrivate(this))
{
    ksni_->menu_ = popup;
    ksni_->updateTooltip(tip);

    connect(ksni_, &KStatusNotifierItemPrivate::activated, this, [this](int button) {
        emit clicked(QPoint(), button);
    });
}

PsiTrayIcon::~PsiTrayIcon()
{
    if (icon_) {
        icon_->disconnect();
        icon_->stop();
        delete icon_;
    }
    // ksni_ is a child QObject, deleted automatically
}

void PsiTrayIcon::setContextMenu(QMenu *menu)
{
    ksni_->menu_ = menu;
}

void PsiTrayIcon::setToolTip(const QString &str)
{
    ksni_->updateTooltip(str);
}

void PsiTrayIcon::setIcon(const PsiIcon *icon, bool alert)
{
    if (icon_) {
        icon_->disconnect();
        icon_->stop();
        delete icon_;
        icon_ = nullptr;
    }

    isAlert_  = alert;
    realIcon_ = quintptr(icon);

    if (icon) {
        if (!alert)
            icon_ = new PsiIcon(*icon);
        else
            icon_ = new AlertIcon(icon);
        connect(icon_, &PsiIcon::pixmapChanged, this, &PsiTrayIcon::animate);
        icon_->activated();
    } else {
        icon_ = new PsiIcon();
    }
    animate();
}

void PsiTrayIcon::setAlert(const PsiIcon *icon)
{
    setIcon(icon, true);
}

void PsiTrayIcon::show()
{
    ksni_->setVisible(true);
}

void PsiTrayIcon::hide()
{
    ksni_->setVisible(false);
}

QPixmap PsiTrayIcon::makeIcon()
{
    if (!icon_)
        return QPixmap();
    // Use a fixed size since we have no geometry from QSystemTrayIcon
    return icon_->pixmap(QSize(22, 22));
}

void PsiTrayIcon::animate()
{
    if (!icon_)
        return;

    QString cachedName = QString("PsiTray/%1/%2/%3")
                             .arg(icon_->name(),
                                  QString::number(realIcon_),
                                  QString::number(icon_->frameNumber()));
    QPixmap p;
    if (!QPixmapCache::find(cachedName, &p)) {
        p = makeIcon();
        QPixmapCache::insert(cachedName, p);
    }

    ksni_->updateIcon(p, isAlert_);
}

bool PsiTrayIcon::eventFilter(QObject *obj, QEvent *event)
{
    Q_UNUSED(obj)
    Q_UNUSED(event)
    return false;
}

#else // PSI_USE_KSNI — original QSystemTrayIcon implementation

#include <QSystemTrayIcon>

PsiTrayIcon::PsiTrayIcon(const QString &tip, QMenu *popup, QObject *parent) :
    QObject(parent), icon_(nullptr), realIcon_(0), isAlert_(false),
    trayicon_(new QSystemTrayIcon())
{
    trayicon_->setContextMenu(popup);
    setToolTip(tip);
    connect(trayicon_, &QSystemTrayIcon::activated, this, &PsiTrayIcon::trayicon_activated);
    trayicon_->installEventFilter(this);
}

PsiTrayIcon::~PsiTrayIcon()
{
    delete trayicon_;
    if (icon_) {
        icon_->disconnect();
        icon_->stop();
        delete icon_;
    }
}

void PsiTrayIcon::setContextMenu(QMenu *menu) { trayicon_->setContextMenu(menu); }
void PsiTrayIcon::setToolTip(const QString &str) { trayicon_->setToolTip(str); }

void PsiTrayIcon::setIcon(const PsiIcon *icon, bool alert)
{
    if (icon_) {
        icon_->disconnect();
        icon_->stop();
        delete icon_;
        icon_ = nullptr;
    }
    isAlert_  = alert;
    realIcon_ = quintptr(icon);
    if (icon) {
        if (!alert)
            icon_ = new PsiIcon(*icon);
        else
            icon_ = new AlertIcon(icon);
        connect(icon_, &PsiIcon::pixmapChanged, this, &PsiTrayIcon::animate);
        icon_->activated();
    } else {
        icon_ = new PsiIcon();
    }
    animate();
}

void PsiTrayIcon::setAlert(const PsiIcon *icon) { setIcon(icon, true); }
void PsiTrayIcon::show() { trayicon_->show(); }
void PsiTrayIcon::hide() { trayicon_->hide(); }

QPixmap PsiTrayIcon::makeIcon()
{
    if (!icon_)
        return QPixmap();
    return icon_->pixmap(trayicon_->geometry().size());
}

void PsiTrayIcon::trayicon_activated(QSystemTrayIcon::ActivationReason reason)
{
#ifdef Q_OS_MAC
    Q_UNUSED(reason)
#else
    if (reason == QSystemTrayIcon::Trigger)
        emit clicked(QPoint(), Qt::LeftButton);
    else if (reason == QSystemTrayIcon::MiddleClick || (isKde() && reason == QSystemTrayIcon::Context))
        emit clicked(QPoint(), Qt::MiddleButton);
#ifdef Q_OS_WIN
    else if (reason == QSystemTrayIcon::DoubleClick)
        emit doubleClicked(QPoint());
#endif
#endif
}

void PsiTrayIcon::animate()
{
    if (!icon_)
        return;
    QString cachedName = QString("PsiTray/%1/%2/%3")
                             .arg(icon_->name(),
                                  QString::number(realIcon_),
                                  QString::number(icon_->frameNumber()));
    QPixmap p;
    if (!QPixmapCache::find(cachedName, &p)) {
        p = makeIcon();
        QPixmapCache::insert(cachedName, p);
    }
    trayicon_->setIcon(p);
}

bool PsiTrayIcon::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == trayicon_ && event->type() == QEvent::ToolTip) {
        emit doToolTip(obj, (static_cast<QHelpEvent *>(event))->globalPos());
        return true;
    }
    return QObject::eventFilter(obj, event);
}

#endif // PSI_USE_KSNI
