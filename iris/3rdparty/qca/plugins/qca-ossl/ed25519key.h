/*
 * Ed25519 public/private key provider for QCA.
 * RFC 8032 signatures use a one-shot message API and no external digest.
 */
#pragma once

#include "qcaprovider.h"

#include <openssl/evp.h>

namespace opensslQCAPlugin {

#ifdef EVP_PKEY_ED25519
class Ed25519Key final : public QCA::Ed25519Context
{
public:
    // Takes ownership of the supplied EVP_PKEY.
    Ed25519Key(QCA::Provider *provider, EVP_PKEY *key, bool isPrivate);
    ~Ed25519Key() override;

    QCA::Provider::Context *clone() const override;
    bool isNull() const override;
    QCA::PKey::Type type() const override;
    bool isPrivate() const override;
    bool canExport() const override;
    void convertToPublic() override;
    int bits() const override;
    void createPrivate(bool block) override;

    void startSign(QCA::SignatureAlgorithm alg, QCA::SignatureFormat format) override;
    void startVerify(QCA::SignatureAlgorithm alg, QCA::SignatureFormat format) override;
    void update(const QCA::MemoryRegion &message) override;
    QByteArray endSign() override;
    bool endVerify(const QByteArray &signature) override;

    EVP_PKEY *pkey() const { return _key; }

private:
    enum class Operation { Idle, Signing, Verifying, Error };
    EVP_PKEY *_key = nullptr;
    bool _private = false;
    Operation _operation = Operation::Idle;
    QByteArray _message;
};
#endif

} // namespace opensslQCAPlugin
