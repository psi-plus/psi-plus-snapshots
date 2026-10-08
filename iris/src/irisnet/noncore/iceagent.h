#ifndef XMPP_ICEAGENT_H
#define XMPP_ICEAGENT_H

#include <iris/irisnet/corelib/irisnetexport.h>

#include <iris/irisnet/noncore/icecomponent.h>

#include <QObject>
#include <memory>

namespace XMPP {

class IRISNET_EXPORT IceAgent : public QObject {
    Q_OBJECT
public:
    static IceAgent *instance();
    ~IceAgent();

    QString foundation(IceComponent::CandidateType type, const QHostAddress baseAddr,
                       const QHostAddress         &stunServAddr     = QHostAddress(),
                       QAbstractSocket::SocketType stunRequestProto = QAbstractSocket::UnknownSocketType);

    static QString randomCredential(int len);

private:
    explicit IceAgent(QObject *parent = nullptr);

signals:

private:
    struct IRISNET_NO_EXPORT Private;
    std::unique_ptr<Private> d;
};

} // namespace XMPP

#endif // XMPP_ICEAGENT_H
