/*
 * Copyright (C) 2026  Psi IM Team
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
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 *
 */

#pragma once

// Block cipher modes of operation for the Windows CNG provider.
//
// CNG supplies the AES block function; the modes are implemented here, on
// whole blocks in ECB mode. CNG's own chaining modes cannot be used for all
// of QCA's ciphers: it has no OFB or CTR, its CFB needs whole blocks per
// call, and its GCM takes only 96-bit nonces and, when chained, only whole
// blocks. This file has no Windows dependency, so the modes can be checked
// against other implementations on any platform.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace wincryptoQCAPlugin {

// Overwrites memory in a way the compiler does not remove.
void secureWipe(void *data, std::size_t size);

// Wipe old allocations too, including those discarded when a vector grows.
template<class T> class WipingAllocator : public std::allocator<T>
{
public:
    using value_type = T;
    template<class U> struct rebind
    {
        using other = WipingAllocator<U>;
    };
    WipingAllocator() = default;
    template<class U> WipingAllocator(const WipingAllocator<U> &)
    {
    }
    void deallocate(T *p, std::size_t count)
    {
        secureWipe(p, count * sizeof(T));
        std::allocator<T>::deallocate(p, count);
    }
};
template<class T, class U> bool operator==(const WipingAllocator<T> &, const WipingAllocator<U> &)
{
    return true;
}
template<class T, class U> bool operator!=(const WipingAllocator<T> &, const WipingAllocator<U> &)
{
    return false;
}
using Bytes = std::vector<std::uint8_t, WipingAllocator<std::uint8_t>>;

void secureWipe(Bytes &bytes);

// A block cipher with 16-byte blocks, used in ECB mode on whole blocks.
class BlockCipher
{
public:
    static constexpr std::size_t BlockSize = 16;

    virtual ~BlockCipher()                             = default;
    virtual std::unique_ptr<BlockCipher> clone() const = 0;
    virtual bool                         encrypt(const std::uint8_t *in, std::uint8_t *out, std::size_t blocks) = 0;
    virtual bool                         decrypt(const std::uint8_t *in, std::uint8_t *out, std::size_t blocks) = 0;
    virtual bool encryptCbc(const std::uint8_t *in, std::uint8_t *out, std::size_t blocks, std::uint8_t *iv);
};

enum class CipherMode
{
    ECB,
    CBC,
    CFB, // 128-bit feedback, as OpenSSL's aes-*-cfb
    OFB,
    CTR, // 128-bit big-endian counter, as OpenSSL's aes-*-ctr
    GCM,
};

// A streaming cipher in one of the modes, with the semantics of QCA's
// OpenSSL provider:
//
// - ECB and CBC process whole blocks. Without padding, a partial block left
//   at the end is an error. With PKCS#7 padding (CBC and ECB), decryption
//   holds back the last block until final() checks the padding.
// - CFB, OFB and CTR are streams of any length.
// - GCM has no additional authenticated data (QCA's cipher API has none).
//   Encryption computes the tag at final(), as long as the tag given at
//   setup (up to 16 bytes). Decryption checks the given tag at final() and
//   releases no plaintext before it has been verified.
class ModeEngine
{
public:
    ModeEngine(CipherMode                   mode,
               bool                         pkcs7,
               bool                         encrypt,
               std::unique_ptr<BlockCipher> cipher,
               const Bytes                 &iv,
               const Bytes                 &tag);
    ModeEngine(const ModeEngine &other);
    ModeEngine &operator=(const ModeEngine &) = delete;
    ~ModeEngine();

    // False when the key, IV or tag cannot be used with the mode.
    bool isValid() const
    {
        return _valid;
    }

    // Appends the output for `size` more bytes of input to `out`.
    bool update(const std::uint8_t *in, std::size_t size, Bytes &out);
    // Appends the remaining output to `out`. False on a padding, length or
    // tag error.
    bool final(Bytes &out);

    // The GCM tag computed by encryption, after final().
    const Bytes &tag() const
    {
        return _tag;
    }

    // 16 for the block modes ECB and CBC, 1 for the stream modes.
    static std::size_t blockSize(CipherMode mode);

private:
    using Block = std::uint8_t[BlockCipher::BlockSize];

    bool updateImpl(const std::uint8_t *in, std::size_t size, Bytes &out);
    bool finalImpl(Bytes &out);
    void clearSecrets();
    bool processBlocks(const std::uint8_t *in, std::size_t blocks, Bytes &out);
    bool stream(const std::uint8_t *in, std::size_t size, Bytes &out);
    bool nextKeystream();
    void ghashBlock(const std::uint8_t *block);
    void ghashBytes(const std::uint8_t *data, std::size_t size);
    bool gcmTag(Block tag);

    CipherMode                   _mode;
    bool                         _pkcs7;
    bool                         _encrypt;
    bool                         _valid = false;
    std::unique_ptr<BlockCipher> _cipher;

    // CBC: the previous ciphertext block. CFB: the feedback register.
    // OFB: the last output block. CTR and GCM: the counter.
    Block       _register {};
    Block       _keystream {};
    std::size_t _used = BlockCipher::BlockSize; // Keystream bytes used.
    Bytes       _pending;                       // Unprocessed input.

    // GCM
    Block         _h {};
    Block         _j0 {};
    Block         _ghash {};
    Block         _ghashPartial {};
    std::size_t   _ghashFill = 0;
    std::uint64_t _textBytes = 0;
    Bytes         _expectedTag;
    std::size_t   _tagSize = 0;
    Bytes         _tag;
};

}
