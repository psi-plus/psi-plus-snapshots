/*
 * Copyright (C) 2008  Barracuda Networks, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "gssapi_p.h"
#include <QtCrypto>
#include <QtPlugin>
#include <qcaprovider.h>

using namespace QCA;

#define PROVIDER_NAME "qca-wingss"

namespace wingssQCAPlugin {

//----------------------------------------------------------------------------
// SaslWinGss
//----------------------------------------------------------------------------
class SaslWinGss : public SASLContext
{
    Q_OBJECT

public:
    SaslGssapiSession  *sess;
    bool                authed         = false;
    Result              _result        = Error;
    SASL::AuthCondition _authCondition = SASL::NoMechanism;
    QByteArray          _step_to_net;
    QByteArray          _to_net, _to_app;
    int                 enc = 0;
    SafeTimer           resultsReadyTrigger;

    QString opt_service, opt_host, opt_ext_id;
    int     opt_ext_ssf = 0;
    int     opt_flags   = 0;
    int     opt_minssf = 0, opt_maxssf = 0;

    QString opt_authzid;

    SaslWinGss(Provider *p)
        : SASLContext(p)
        , sess(0)
        , resultsReadyTrigger(this)
    {
        connect(&resultsReadyTrigger, SIGNAL(timeout()), SIGNAL(resultsReady()));
        resultsReadyTrigger.setSingleShot(true);
    }

    Provider::Context *clone() const
    {
        return 0;
    }

    ~SaslWinGss() override
    {
        reset();
    }

    virtual void reset()
    {
        delete sess;
        sess   = 0;
        authed = false;
        _step_to_net.clear();
        _to_net.clear();
        _to_app.clear();
        enc            = 0;
        _result        = Error;
        _authCondition = SASL::NoMechanism;
        resultsReadyTrigger.stop();

        opt_service.clear();
        opt_host.clear();
        opt_ext_id.clear();
        opt_authzid.clear();
    }

    virtual void setup(const QString  &service,
                       const QString  &host,
                       const HostPort *local,
                       const HostPort *remote,
                       const QString  &ext_id,
                       int             ext_ssf)
    {
        // unused by this provider
        Q_UNUSED(local);
        Q_UNUSED(remote);

        opt_service = service;
        opt_host    = host;
        opt_ext_id  = ext_id;
        opt_ext_ssf = ext_ssf;
    }

    virtual void setConstraints(SASL::AuthFlags f, int minSSF, int maxSSF)
    {
        opt_flags  = (int)f;
        opt_minssf = minSSF;
        opt_maxssf = maxSSF;
    }

    virtual void startClient(const QStringList &mechlist, bool allowClientSendFirst)
    {
        // we only support GSSAPI
        if (!mechlist.contains(QStringLiteral("GSSAPI"))) {
            _result        = Error;
            _authCondition = SASL::NoMechanism;
            resultsReadyTrigger.start();
            return;
        }

        // GSSAPI (or this provider) doesn't meet these requirements
        if (opt_flags & SASL::RequireForwardSecrecy || opt_flags & SASL::RequirePassCredentials ||
            !allowClientSendFirst || opt_minssf > 56 || opt_minssf > opt_maxssf) {
            _result        = Error;
            _authCondition = SASL::NoMechanism;
            resultsReadyTrigger.start();
            return;
        }

        sess          = new SaslGssapiSession;
        sess->authzid = opt_authzid;

        int secflags = 0;
        if (opt_minssf > 1)
            secflags |= SaslGssapiSession::RequireConf;
        else if (opt_minssf == 1)
            secflags |= SaslGssapiSession::RequireAtLeastInt;
        if (opt_maxssf < 56)
            secflags |= SaslGssapiSession::DisableConf;
        if (opt_maxssf < 1)
            secflags |= SaslGssapiSession::DisableInt;
        if (opt_flags & SASL::RequireMutualAuth)
            secflags |= SaslGssapiSession::RequireMutualAuth;

        SaslGssapiSession::ReturnCode ret = sess->init(opt_service, opt_host, secflags);
        if (ret != SaslGssapiSession::Success) {
            _result        = Error;
            _authCondition = SASL::AuthFail;
            resultsReadyTrigger.start();
            return;
        }

        ret = sess->step(QByteArray(), &_step_to_net, &authed);
        if (ret != SaslGssapiSession::Success) {
            _result        = Error;
            _authCondition = SASL::AuthFail;
            resultsReadyTrigger.start();
            return;
        }

        if (authed)
            _result = Success;
        else
            _result = Continue;

        resultsReadyTrigger.start();
    }

    virtual void startServer(const QString &realm, bool disableServerSendLast)
    {
        // server mode unsupported at this time
        Q_UNUSED(realm);
        Q_UNUSED(disableServerSendLast);

        _result        = Error;
        _authCondition = SASL::AuthFail;
        resultsReadyTrigger.start();
    }

    virtual void serverFirstStep(const QString &mech, const QByteArray *clientInit)
    {
        // server mode unsupported at this time
        Q_UNUSED(mech);
        Q_UNUSED(clientInit);
    }

    virtual void nextStep(const QByteArray &from_net)
    {
        if (!sess) {
            _result        = Error;
            _authCondition = SASL::AuthFail;
            resultsReadyTrigger.start();
            return;
        }
        SaslGssapiSession::ReturnCode ret = sess->step(from_net, &_step_to_net, &authed);
        if (ret != SaslGssapiSession::Success) {
            _result        = Error;
            _authCondition = SASL::AuthFail;
            resultsReadyTrigger.start();
            return;
        }

        if (authed)
            _result = Success;
        else
            _result = Continue;

        resultsReadyTrigger.start();
    }

    virtual void tryAgain()
    {
        // we never ask for params, so this function should never be
        //   called
    }

    virtual void update(const QByteArray &from_net, const QByteArray &from_app)
    {
        if (!sess || !authed || from_app.size() > INT_MAX - enc) {
            _result        = Error;
            _authCondition = SASL::AuthFail;
            resultsReadyTrigger.start();
            return;
        }
        SaslGssapiSession::ReturnCode ret;
        QByteArray                    a;

        if (!from_net.isEmpty()) {
            ret = sess->decode(from_net, &a);
            if (ret != SaslGssapiSession::Success) {
                _result = Error;
                resultsReadyTrigger.start();
                return;
            }

            _to_app += a;
        }

        if (!from_app.isEmpty()) {
            ret = sess->encode(from_app, &a);
            if (ret != SaslGssapiSession::Success) {
                _result = Error;
                resultsReadyTrigger.start();
                return;
            }

            _to_net += a;
            enc += from_app.size();
        }

        _result = Success;
        resultsReadyTrigger.start();
    }

    virtual bool waitForResultsReady(int msecs)
    {
        // all results are ready instantly
        Q_UNUSED(msecs);
        resultsReadyTrigger.stop();
        return true;
    }

    virtual Result result() const
    {
        return _result;
    }

    virtual QStringList mechlist() const
    {
        // server mode unsupported at this time
        return QStringList();
    }

    virtual QString mech() const
    {
        // only mech we support :)
        return QStringLiteral("GSSAPI");
    }

    virtual bool haveClientInit() const
    {
        // GSSAPI always has a client init response
        return true;
    }

    virtual QByteArray stepData() const
    {
        return _step_to_net;
    }

    virtual QByteArray to_net()
    {
        QByteArray a = _to_net;
        _to_net.clear();
        enc = 0;
        return a;
    }

    virtual int encoded() const
    {
        return enc;
    }

    virtual QByteArray to_app()
    {
        QByteArray a = _to_app;
        _to_app.clear();
        return a;
    }

    virtual int ssf() const
    {
        if (!sess || !sess->do_layer)
            return 0;

        if (sess->do_conf) {
            // TODO: calculate this value somehow?  for now we'll
            //   just hard code it to 56, which is basically what
            //   cyrus does.
            return 56;
        } else
            return 1;
    }

    virtual SASL::AuthCondition authCondition() const
    {
        return _authCondition;
    }

    virtual SASL::Params clientParams() const
    {
        // we never ask for params
        return SASL::Params();
    }

    virtual void
    setClientParams(const QString *user, const QString *authzid, const SecureArray *pass, const QString *realm)
    {
        // unused by this provider
        Q_UNUSED(user);
        Q_UNUSED(pass);
        Q_UNUSED(realm);

        if (authzid) {
            opt_authzid = *authzid;
            if (sess)
                sess->authzid = opt_authzid;
        } else {
            opt_authzid.clear();
            if (sess)
                sess->authzid.clear();
        }
    }

    virtual QStringList realmlist() const
    {
        // unused by this provider
        return QStringList();
    }

    virtual QString username() const
    {
        // server mode unsupported at this time
        return QString();
    }

    virtual QString authzid() const
    {
        // server mode unsupported at this time
        return QString();
    }
};

//----------------------------------------------------------------------------
// MetaSasl
//----------------------------------------------------------------------------
class wingssProvider;
static bool wingssProvider_have_sspi(wingssProvider *provider);

class MetaSasl : public SASLContext
{
    Q_OBJECT

public:
    SASLContext *s;

    Result              _result        = Error;
    SASL::AuthCondition _authCondition = SASL::NoMechanism;
    SafeTimer           resultsReadyTrigger;
    Synchronizer        sync;
    bool                waiting;

    QString         opt_service, opt_host;
    bool            have_opt_local = false, have_opt_remote = false;
    HostPort        opt_local, opt_remote;
    QString         opt_ext_id;
    int             opt_ext_ssf = 0;
    SASL::AuthFlags opt_flags   = SASL::AuthFlagsNone;
    int             opt_minssf = 0, opt_maxssf = 0;

    bool        have_opt_user, have_opt_authzid, have_opt_pass, have_opt_realm;
    QString     opt_user, opt_authzid, opt_realm;
    SecureArray opt_pass;
    QString     bindingType;
    QByteArray  bindingData;
    bool        bindingCritical = true;

    class SaslProvider
    {
    public:
        SASLContext *sasl;
        bool         ready;
        QStringList  mechlist;

        SaslProvider()
            : sasl(0)
            , ready(false)
        {
        }
    };

    QList<SaslProvider> saslProviders;
    bool                serverInit_active;
    Result              serverInit_result = Error;
    QStringList         serverInit_mechlist;

    MetaSasl(Provider *p)
        : SASLContext(p)
        , resultsReadyTrigger(this)
        , sync(this)
        , waiting(false)
        , serverInit_active(false)
    {
        s = 0;

        have_opt_user    = false;
        have_opt_authzid = false;
        have_opt_pass    = false;
        have_opt_realm   = false;

        connect(&resultsReadyTrigger, SIGNAL(timeout()), SIGNAL(resultsReady()));
        resultsReadyTrigger.setSingleShot(true);
    }

    ~MetaSasl()
    {
        reset();
    }

    virtual Provider::Context *clone() const
    {
        return 0;
    }

    void clearSaslProviders()
    {
        foreach (const SaslProvider &sp, saslProviders)
            delete sp.sasl;

        saslProviders.clear();
    }

    virtual void reset()
    {
        delete s;
        s = 0;

        resultsReadyTrigger.stop();

        opt_service.clear();
        opt_host.clear();
        opt_ext_id.clear();
        opt_user.clear();
        opt_authzid.clear();
        opt_realm.clear();
        opt_pass.clear();
        bindingType.clear();
        bindingData.clear();
        bindingCritical = true;

        have_opt_user    = false;
        have_opt_authzid = false;
        have_opt_pass    = false;
        have_opt_realm   = false;

        clearSaslProviders();
        serverInit_active = false;
        serverInit_mechlist.clear();
        _result           = Error;
        _authCondition    = SASL::NoMechanism;
        serverInit_result = Error;
    }

    bool supportsChannelBinding() const override
    {
        if (s)
            return s->supportsChannelBinding();
        for (Provider *candidate : providers()) {
            if (candidate->name() != QStringLiteral(PROVIDER_NAME) &&
                candidate->features().contains(QStringLiteral("sasl"))) {
                SASL probe(nullptr, candidate->name());
                if (probe.supportsChannelBinding())
                    return true;
            }
        }
        return false;
    }

    bool setChannelBinding(const QString &type, const QByteArray &data, bool critical) override
    {
        if (type.isEmpty() || data.isEmpty())
            return false;
        if (s)
            return s->setChannelBinding(type, data, critical);
        for (Provider *candidate : providers()) {
            if (candidate->name() != QStringLiteral(PROVIDER_NAME) &&
                candidate->features().contains(QStringLiteral("sasl"))) {
                SASL probe(nullptr, candidate->name());
                if (probe.setChannelBinding(type, data, critical)) {
                    bindingType     = type;
                    bindingData     = data;
                    bindingCritical = critical;
                    return true;
                }
            }
        }
        return false;
    }

    virtual void setup(const QString  &service,
                       const QString  &host,
                       const HostPort *local,
                       const HostPort *remote,
                       const QString  &ext_id,
                       int             ext_ssf)
    {
        opt_service     = service;
        opt_host        = host;
        have_opt_local  = false;
        have_opt_remote = false;
        if (local) {
            have_opt_local = true;
            opt_local      = *local;
        }
        if (remote) {
            have_opt_remote = true;
            opt_remote      = *remote;
        }
        opt_ext_id  = ext_id;
        opt_ext_ssf = ext_ssf;
    }

    virtual void setConstraints(SASL::AuthFlags f, int minSSF, int maxSSF)
    {
        opt_flags  = f;
        opt_minssf = minSSF;
        opt_maxssf = maxSSF;
    }

    virtual void startClient(const QStringList &mechlist, bool allowClientSendFirst)
    {
        if (mechlist.contains(QStringLiteral("GSSAPI")) && (bindingData.isEmpty() || !bindingCritical) &&
            wingssProvider_have_sspi((wingssProvider *)provider())) {
            s = new SaslWinGss(provider());
        } else {
            // collect providers supporting sasl, in priority order.
            //   (note: providers() is in priority order already)
            ProviderList list;
            foreach (Provider *p, providers()) {
                QString name = p->name();

                // skip ourself
                if (name == QStringLiteral(PROVIDER_NAME))
                    continue;

                if (p->features().contains(QStringLiteral("sasl"))) {
                    // FIXME: improve qca so this isn't needed
                    SASL tmp_object_to_cause_plugin_init(0, name);

                    // add to the list
                    list += p;
                }
            }

            for (Provider *candidate : list) {
                auto *context = static_cast<SASLContext *>(candidate->createContext(QStringLiteral("sasl")));
                if (!context)
                    continue;
                if (!bindingData.isEmpty() && bindingCritical &&
                    !context->setChannelBinding(bindingType, bindingData, bindingCritical)) {
                    delete context;
                    continue;
                }
                s = context;
                break;
            }
        }

        if (!s) {
            // no usable provider?  throw error
            _result        = Error;
            _authCondition = SASL::NoMechanism;
            resultsReadyTrigger.start();
            return;
        }

        // proper parenting
        s->setParent(this);

        const HostPort *pLocal  = 0;
        const HostPort *pRemote = 0;
        if (have_opt_local)
            pLocal = &opt_local;
        if (have_opt_remote)
            pRemote = &opt_remote;
        s->setup(opt_service, opt_host, pLocal, pRemote, opt_ext_id, opt_ext_ssf);
        s->setConstraints(opt_flags, opt_minssf, opt_maxssf);
        if (!bindingData.isEmpty() && !s->setChannelBinding(bindingType, bindingData, bindingCritical) &&
            bindingCritical) {
            delete s;
            s              = nullptr;
            _result        = Error;
            _authCondition = SASL::NoMechanism;
            resultsReadyTrigger.start();
            return;
        }

        const QString     *pUser    = 0;
        const QString     *pAuthzid = 0;
        const SecureArray *pPass    = 0;
        const QString     *pRealm   = 0;
        if (have_opt_user)
            pUser = &opt_user;
        if (have_opt_authzid)
            pAuthzid = &opt_authzid;
        if (have_opt_pass)
            pPass = &opt_pass;
        if (have_opt_realm)
            pRealm = &opt_realm;
        s->setClientParams(pUser, pAuthzid, pPass, pRealm);
        opt_pass.clear();
        have_opt_pass = false;
        connect(s, SIGNAL(resultsReady()), SLOT(s_resultsReady()));

        QString str = QStringLiteral("MetaSasl: client using %1 with %2 mechs")
                          .arg(s->provider()->name(), QString::number(mechlist.count()));
        QCA_logTextMessage(str, Logger::Debug);
        s->startClient(mechlist, allowClientSendFirst);
    }

    virtual void startServer(const QString &realm, bool disableServerSendLast)
    {
        // collect mechs of all providers, by starting all of them
        serverInit_active = true;
        serverInit_result = Continue;

        ProviderList list;
        foreach (Provider *p, providers()) {
            QString name = p->name();

            // skip ourself
            if (name == QStringLiteral(PROVIDER_NAME))
                continue;

            if (p->features().contains(QStringLiteral("sasl"))) {
                // FIXME: improve qca so this isn't needed
                SASL tmp_object_to_cause_plugin_init(0, name);

                // add to the list
                list += p;
            }
        }

        foreach (Provider *p, list) {
            SaslProvider sp;

            sp.sasl = static_cast<SASLContext *>(p->createContext(QStringLiteral("sasl")));

            if (!sp.sasl)
                continue;
            // proper parenting
            sp.sasl->setParent(this);

            const HostPort *pLocal  = 0;
            const HostPort *pRemote = 0;
            if (have_opt_local)
                pLocal = &opt_local;
            if (have_opt_remote)
                pRemote = &opt_remote;
            sp.sasl->setup(opt_service, opt_host, pLocal, pRemote, opt_ext_id, opt_ext_ssf);
            sp.sasl->setConstraints(opt_flags, opt_minssf, opt_maxssf);
            if (!bindingData.isEmpty() && !sp.sasl->setChannelBinding(bindingType, bindingData, bindingCritical) &&
                bindingCritical) {
                delete sp.sasl;
                continue;
            }
            connect(sp.sasl, SIGNAL(resultsReady()), SLOT(serverInit_resultsReady()));

            saslProviders += sp;

            sp.sasl->startServer(realm, disableServerSendLast);
        }
        if (saslProviders.isEmpty()) {
            serverInit_result = Error;
            _authCondition    = SASL::NoMechanism;
            resultsReadyTrigger.start();
        }
    }

    virtual void serverFirstStep(const QString &mech, const QByteArray *clientInit)
    {
        // choose a provider based on the mech
        int at = choose_provider(mech);
        if (saslProviders.isEmpty() || at < 0 || at >= saslProviders.size()) {
            serverInit_active = false;
            _result           = Error;
            _authCondition    = SASL::NoMechanism;
            resultsReadyTrigger.start();
            return;
        }

        // extract it and clean up the rest
        SASLContext *sasl = saslProviders[at].sasl;
        sasl->disconnect(this);
        saslProviders.removeAt(at);
        clearSaslProviders();
        serverInit_active = false;

        // use the chosen provider
        s = sasl;
        connect(s, SIGNAL(resultsReady()), SLOT(s_resultsReady()));
        s->serverFirstStep(mech, clientInit);
    }

    virtual void nextStep(const QByteArray &from_net)
    {
        if (s)
            s->nextStep(from_net);
        else
            failNoMechanism();
    }

    virtual void tryAgain()
    {
        if (s)
            s->tryAgain();
        else
            failNoMechanism();
    }

    virtual void update(const QByteArray &from_net, const QByteArray &from_app)
    {
        if (s)
            s->update(from_net, from_app);
        else
            failNoMechanism();
    }

    virtual bool waitForResultsReady(int msecs)
    {
        if (serverInit_active && serverInit_result == Continue) {
            waiting  = true;
            bool ret = sync.waitForCondition(msecs);
            waiting  = false;
            return ret;
        } else if (s)
            return s->waitForResultsReady(msecs);
        else
            return true;
    }

    virtual Result result() const
    {
        if (serverInit_active)
            return serverInit_result;
        else if (s)
            return s->result();
        else
            return _result;
    }

    virtual QStringList mechlist() const
    {
        return serverInit_mechlist;
    }

    virtual QString mech() const
    {
        if (s)
            return s->mech();
        else
            return QString();
    }

    virtual bool haveClientInit() const
    {
        return s ? s->haveClientInit() : false;
    }

    virtual QByteArray stepData() const
    {
        return s ? s->stepData() : QByteArray();
    }

    virtual QByteArray to_net()
    {
        return s ? s->to_net() : QByteArray();
    }

    virtual int encoded() const
    {
        return s ? s->encoded() : 0;
    }

    virtual QByteArray to_app()
    {
        return s ? s->to_app() : QByteArray();
    }

    virtual int ssf() const
    {
        return s ? s->ssf() : 0;
    }

    virtual SASL::AuthCondition authCondition() const
    {
        if (s)
            return s->authCondition();
        else
            return _authCondition;
    }

    virtual SASL::Params clientParams() const
    {
        return s ? s->clientParams() : SASL::Params();
    }

    virtual void
    setClientParams(const QString *user, const QString *authzid, const SecureArray *pass, const QString *realm)
    {
        if (!s) {
            if (user) {
                have_opt_user = true;
                opt_user      = *user;
            }
            if (authzid) {
                have_opt_authzid = true;
                opt_authzid      = *authzid;
            }
            if (pass) {
                have_opt_pass = true;
                opt_pass      = *pass;
            }
            if (realm) {
                have_opt_realm = true;
                opt_realm      = *realm;
            }
        } else {
            s->setClientParams(user, authzid, pass, realm);
        }
    }

    virtual QStringList realmlist() const
    {
        return s ? s->realmlist() : QStringList();
    }

    virtual QString username() const
    {
        return s ? s->username() : QString();
    }

    virtual QString authzid() const
    {
        return s ? s->authzid() : QString();
    }

private Q_SLOTS:
    void s_resultsReady()
    {
        Q_EMIT resultsReady();
    }

    void serverInit_resultsReady()
    {
        SASLContext *sasl = (SASLContext *)sender();

        int at = -1;
        for (int n = 0; n < saslProviders.count(); ++n) {
            if (saslProviders[n].sasl == sasl) {
                at = n;
                break;
            }
        }
        if (at == -1)
            return;

        if (sasl->result() == Success) {
            saslProviders[at].ready    = true;
            saslProviders[at].mechlist = sasl->mechlist();
        } else {
            sasl->deleteLater();
            saslProviders.removeAt(at);
        }
        if (saslProviders.isEmpty()) {
            serverInit_result = Error;
            _authCondition    = SASL::NoMechanism;
        } else {
            for (const auto &entry : saslProviders) {
                if (!entry.ready)
                    return;
            }
            serverInit_result   = Success;
            serverInit_mechlist = combine_mechlists();
        }
        if (waiting)
            sync.conditionMet();
        else
            Q_EMIT resultsReady();
    }

private:
    void failNoMechanism()
    {
        _result        = Error;
        _authCondition = SASL::NoMechanism;
        resultsReadyTrigger.start();
    }

    QStringList combine_mechlists()
    {
        QStringList out;

        // FIXME: consider prioritizing certain mechs?
        foreach (const SaslProvider &sp, saslProviders) {
            foreach (const QString &mech, sp.mechlist) {
                if (!out.contains(mech))
                    out += mech;
            }
        }

        return out;
    }

    int choose_provider(const QString &mech)
    {
        int at = -1;

        // find a provider for this mech
        for (int n = 0; n < saslProviders.count(); ++n) {
            const SaslProvider &sp = saslProviders[n];
            if (sp.mechlist.contains(mech)) {
                at = n;
                break;
            }
        }

        return at;
    }
};

class wingssProvider : public Provider
{
public:
    mutable QMutex m;
    mutable bool   forced_priority;
    bool           have_sspi;

    wingssProvider()
        : forced_priority(false)
        , have_sspi(false)
    {
    }

    virtual void init()
    {
        sspi_set_logger(do_log);
        have_sspi = sspi_load();
    }

    ~wingssProvider()
    {
        if (have_sspi)
            sspi_unload();
    }

    virtual int qcaVersion() const
    {
        return QCA_VERSION;
    }

    virtual QString name() const
    {
        return QStringLiteral(PROVIDER_NAME);
    }

    virtual QStringList features() const
    {
        m.lock();
        // FIXME: we need to prioritize this plugin to be higher
        //   than other plugins by default.  unfortunately there's
        //   no clean way to do this.  we can't change our priority
        //   until we are slotted into the qca provider system.  the
        //   constructor, qcaVersion, and name functions are all
        //   guaranteed to be called, but unfortunately they are
        //   only guaranteed to be called before the slotting.  the
        //   features function is essentially guaranteed to be called
        //   after the slotting though, since QCA::isSupported()
        //   trips it, and any proper QCA app will call isSupported.
        if (!forced_priority) {
            forced_priority = true;
            setProviderPriority(QStringLiteral(PROVIDER_NAME), 0);
        }
        m.unlock();

        QStringList list;
        list += QStringLiteral("sasl");
        return list;
    }

    virtual Context *createContext(const QString &type)
    {
        if (type == QStringLiteral("sasl"))
            return new MetaSasl(this);
        else
            return 0;
    }

    static void do_log(const QString &str)
    {
        QCA_logTextMessage(str, Logger::Debug);
    }
};

bool wingssProvider_have_sspi(wingssProvider *provider)
{
    return provider->have_sspi;
}

}

using namespace wingssQCAPlugin;

//----------------------------------------------------------------------------
// wingssPlugin
//----------------------------------------------------------------------------

class qca_wingss : public QObject, public QCAPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "com.affinix.qca.Plugin/1.0")
    Q_INTERFACES(QCAPlugin)

public:
    virtual Provider *createProvider()
    {
        return new wingssProvider;
    }
};

#include "qca-wingss.moc"
