#ifndef MS_DEP_USRSCTP_HPP
#define MS_DEP_USRSCTP_HPP

// #include "common.hpp"
#include "SctpAssociation.hpp"
// #include "handles/Timer.hpp"
#include <unordered_map>

#include <QElapsedTimer>
#include <QTimer>
#include <QtGlobal>

class DepUsrSCTP {
private:
    class Checker {
    public:
        Checker();
        ~Checker();

    public:
        void Start();
        void Stop();

        /* Pure virtual methods inherited from Timer::Listener. */
    public:
        void OnTimer();

    private:
        QTimer       *timer { nullptr };
        QElapsedTimer elapsedTimer;
    };

public:
    static void                  ClassInit();
    static void                  ClassDestroy();
    static uintptr_t             GetNextSctpAssociationId();
    static void                  RegisterSctpAssociation(RTC::SctpAssociation *sctpAssociation);
    static void                  DeregisterSctpAssociation(RTC::SctpAssociation *sctpAssociation);
    static RTC::SctpAssociation *RetrieveSctpAssociation(uintptr_t id);

private:
    static Checker                                              *checker;
    static uint64_t                                              numSctpAssociations;
    static uintptr_t                                             nextSctpAssociationId;
    static std::unordered_map<uintptr_t, RTC::SctpAssociation *> mapIdSctpAssociation;
};

#endif
