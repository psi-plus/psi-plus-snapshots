/*
 * qca_systemstore_android.cpp - Qt Cryptographic Architecture
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301  USA
 *
 */

#include "qca_systemstore.h"

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QtGlobal>

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
#include <QJniEnvironment>
#else
#include <QAndroidJniEnvironment>
#endif

namespace QCA {

namespace {

template<typename T> class LocalReference
{
public:
    LocalReference(JNIEnv *environment, T reference)
        : m_environment(environment)
        , m_reference(reference)
    {
    }

    ~LocalReference()
    {
        if (m_reference)
            m_environment->DeleteLocalRef(m_reference);
    }

    LocalReference(const LocalReference &)            = delete;
    LocalReference &operator=(const LocalReference &) = delete;

    T get() const
    {
        return m_reference;
    }

    explicit operator bool() const
    {
        return m_reference != nullptr;
    }

private:
    JNIEnv *m_environment;
    T       m_reference;
};

bool clearPendingException(JNIEnv *environment)
{
    if (!environment->ExceptionCheck())
        return false;

    environment->ExceptionClear();
    return true;
}

QList<QByteArray> loadAndroidSystemCertificates()
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    QJniEnvironment jniEnvironment;
    JNIEnv         *environment = jniEnvironment.jniEnv();
#else
    QAndroidJniEnvironment jniEnvironment;
    JNIEnv                *environment = jniEnvironment;
#endif

    if (!environment)
        return {};

    LocalReference<jclass> trustManagerFactoryClass(environment,
                                                    environment->FindClass("javax/net/ssl/TrustManagerFactory"));
    if (clearPendingException(environment) || !trustManagerFactoryClass)
        return {};

    LocalReference<jclass> x509TrustManagerClass(environment, environment->FindClass("javax/net/ssl/X509TrustManager"));
    if (clearPendingException(environment) || !x509TrustManagerClass)
        return {};

    LocalReference<jclass> x509CertificateClass(environment,
                                                environment->FindClass("java/security/cert/X509Certificate"));
    if (clearPendingException(environment) || !x509CertificateClass)
        return {};

    const jmethodID getDefaultAlgorithm =
        environment->GetStaticMethodID(trustManagerFactoryClass.get(), "getDefaultAlgorithm", "()Ljava/lang/String;");
    if (clearPendingException(environment) || !getDefaultAlgorithm)
        return {};

    const jmethodID getInstance = environment->GetStaticMethodID(
        trustManagerFactoryClass.get(), "getInstance", "(Ljava/lang/String;)Ljavax/net/ssl/TrustManagerFactory;");
    if (clearPendingException(environment) || !getInstance)
        return {};

    const jmethodID init =
        environment->GetMethodID(trustManagerFactoryClass.get(), "init", "(Ljava/security/KeyStore;)V");
    if (clearPendingException(environment) || !init)
        return {};

    const jmethodID getTrustManagers =
        environment->GetMethodID(trustManagerFactoryClass.get(), "getTrustManagers", "()[Ljavax/net/ssl/TrustManager;");
    if (clearPendingException(environment) || !getTrustManagers)
        return {};

    const jmethodID getAcceptedIssuers = environment->GetMethodID(
        x509TrustManagerClass.get(), "getAcceptedIssuers", "()[Ljava/security/cert/X509Certificate;");
    if (clearPendingException(environment) || !getAcceptedIssuers)
        return {};

    const jmethodID getEncoded = environment->GetMethodID(x509CertificateClass.get(), "getEncoded", "()[B");
    if (clearPendingException(environment) || !getEncoded)
        return {};

    LocalReference<jstring> algorithm(
        environment,
        static_cast<jstring>(environment->CallStaticObjectMethod(trustManagerFactoryClass.get(), getDefaultAlgorithm)));
    if (clearPendingException(environment) || !algorithm)
        return {};

    LocalReference<jobject> factory(
        environment, environment->CallStaticObjectMethod(trustManagerFactoryClass.get(), getInstance, algorithm.get()));
    if (clearPendingException(environment) || !factory)
        return {};

    environment->CallVoidMethod(factory.get(), init, static_cast<jobject>(nullptr));
    if (clearPendingException(environment))
        return {};

    LocalReference<jobjectArray> trustManagers(
        environment, static_cast<jobjectArray>(environment->CallObjectMethod(factory.get(), getTrustManagers)));
    if (clearPendingException(environment) || !trustManagers)
        return {};

    QList<QByteArray> certificates;
    QSet<QByteArray>  uniqueCertificates;

    const jsize trustManagerCount = environment->GetArrayLength(trustManagers.get());
    if (clearPendingException(environment))
        return {};

    for (jsize managerIndex = 0; managerIndex < trustManagerCount; ++managerIndex) {
        LocalReference<jobject> trustManager(environment,
                                             environment->GetObjectArrayElement(trustManagers.get(), managerIndex));
        if (clearPendingException(environment) || !trustManager)
            continue;

        if (!environment->IsInstanceOf(trustManager.get(), x509TrustManagerClass.get())) {
            clearPendingException(environment);
            continue;
        }

        LocalReference<jobjectArray> acceptedIssuers(
            environment,
            static_cast<jobjectArray>(environment->CallObjectMethod(trustManager.get(), getAcceptedIssuers)));
        if (clearPendingException(environment) || !acceptedIssuers)
            continue;

        const jsize issuerCount = environment->GetArrayLength(acceptedIssuers.get());
        if (clearPendingException(environment))
            continue;

        for (jsize issuerIndex = 0; issuerIndex < issuerCount; ++issuerIndex) {
            LocalReference<jobject> certificate(environment,
                                                environment->GetObjectArrayElement(acceptedIssuers.get(), issuerIndex));
            if (clearPendingException(environment) || !certificate)
                continue;

            LocalReference<jbyteArray> encoded(
                environment, static_cast<jbyteArray>(environment->CallObjectMethod(certificate.get(), getEncoded)));
            if (clearPendingException(environment) || !encoded)
                continue;

            const jsize encodedSize = environment->GetArrayLength(encoded.get());
            if (clearPendingException(environment) || encodedSize <= 0)
                continue;

            QByteArray der(encodedSize, '\0');
            environment->GetByteArrayRegion(encoded.get(), 0, encodedSize, reinterpret_cast<jbyte *>(der.data()));
            if (clearPendingException(environment))
                continue;

            if (!uniqueCertificates.contains(der)) {
                uniqueCertificates.insert(der);
                certificates.append(der);
            }
        }
    }

    return certificates;
}

QList<QByteArray> androidSystemCertificates()
{
    // Android trust anchors normally change only after an OS or user store
    // update. Cache a successful read for the process lifetime. An empty read
    // is retried, because it can also mean that JNI or the trust provider was
    // temporarily unavailable during early application startup.
    static QMutex            mutex;
    static QList<QByteArray> certificates;

    const QMutexLocker locker(&mutex);
    if (certificates.isEmpty())
        certificates = loadAndroidSystemCertificates();

    return certificates;
}

} // namespace

bool qca_have_systemstore()
{
    return !androidSystemCertificates().isEmpty();
}

CertificateCollection qca_get_systemstore(const QString &provider)
{
    CertificateCollection collection;

    const QList<QByteArray> systemCertificates = androidSystemCertificates();
    for (const QByteArray &der : systemCertificates) {
        ConvertResult result      = ErrorDecode;
        Certificate   certificate = Certificate::fromDER(der, &result, provider);
        if (result == ConvertGood)
            collection.addCertificate(certificate);
    }

    return collection;
}

} // namespace QCA
