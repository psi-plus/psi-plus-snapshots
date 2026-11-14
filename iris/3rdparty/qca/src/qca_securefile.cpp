/*
 * qca_securefile.cpp - secure file I/O
 * Copyright (C) 2026  Sergei Ilinykh <rion4ik@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#include "qca_securefile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QUuid>

#include <algorithm>
#include <climits>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace QCA {

class SecureFile::Private
{
public:
    QString           fileName;
    qint64            maximumSize = SecureFile::DefaultMaximumSize;
    SecureFile::Error error       = SecureFile::NoError;
    QString           errorString;

    void clearError()
    {
        error = SecureFile::NoError;
        errorString.clear();
    }

    void setError(SecureFile::Error value, const QString &text)
    {
        error       = value;
        errorString = text;
    }

    qint64 effectiveMaximumSize() const
    {
        return std::min<qint64>(maximumSize, INT_MAX);
    }
};

SecureFile::SecureFile(const QString &fileName)
    : d(new Private)
{
    d->fileName = fileName;
}

SecureFile::~SecureFile() = default;

void SecureFile::setFileName(const QString &fileName)
{
    d->fileName = fileName;
    d->clearError();
}

QString SecureFile::fileName() const
{
    return d->fileName;
}

void SecureFile::setMaximumSize(qint64 bytes)
{
    d->maximumSize = std::max<qint64>(bytes, 0);
}

qint64 SecureFile::maximumSize() const
{
    return d->maximumSize;
}

SecureFile::Error SecureFile::error() const
{
    return d->error;
}

QString SecureFile::errorString() const
{
    return d->errorString;
}

#ifdef Q_OS_WIN

static bool validExistingWindowsTarget(const QString &fileName)
{
    const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(fileName.utf16()));
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
    }

    return !(attributes & FILE_ATTRIBUTE_DIRECTORY) && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT);
}

SecureArray SecureFile::read()
{
    d->clearError();
    if (d->fileName.isEmpty()) {
        d->setError(InvalidPath, QStringLiteral("Secure file path is empty"));
        return SecureArray();
    }

    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(d->fileName.utf16()),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        d->setError(OpenError, QStringLiteral("Unable to open secure file"));
        return SecureArray();
    }

    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(handle, &info) || GetFileType(handle) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        CloseHandle(handle);
        d->setError(InvalidFile, QStringLiteral("Secure file is not a regular file"));
        return SecureArray();
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0 || size.QuadPart > d->effectiveMaximumSize()) {
        CloseHandle(handle);
        d->setError(TooLarge, QStringLiteral("Secure file exceeds the configured size limit"));
        return SecureArray();
    }

    const qint64 limit    = d->effectiveMaximumSize();
    qint64       used     = 0;
    int          capacity = static_cast<int>(std::min<qint64>(size.QuadPart, limit));
    SecureArray  result(capacity);

    for (;;) {
        if (used == capacity) {
            if (used == limit) {
                SecureArray probe(1);
                DWORD       count = 0;
                if (!ReadFile(handle, probe.data(), 1, &count, nullptr)) {
                    CloseHandle(handle);
                    d->setError(ReadError, QStringLiteral("Unable to read secure file"));
                    return SecureArray();
                }
                if (count != 0) {
                    CloseHandle(handle);
                    d->setError(TooLarge, QStringLiteral("Secure file exceeds the configured size limit"));
                    return SecureArray();
                }
                break;
            }

            const qint64 next = std::min<qint64>(limit, used + 64 * 1024);
            if (!result.resize(static_cast<int>(next))) {
                CloseHandle(handle);
                d->setError(ReadError, QStringLiteral("Unable to allocate secure file buffer"));
                return SecureArray();
            }
            capacity = static_cast<int>(next);
        }

        DWORD       count  = 0;
        const DWORD wanted = static_cast<DWORD>(capacity - used);
        if (!ReadFile(handle, result.data() + used, wanted, &count, nullptr)) {
            CloseHandle(handle);
            d->setError(ReadError, QStringLiteral("Unable to read secure file"));
            return SecureArray();
        }
        if (count == 0)
            break;
        used += count;
    }

    CloseHandle(handle);
    if (result.size() != used)
        result.resize(static_cast<int>(used));
    return result;
}

bool SecureFile::write(const SecureArray &data)
{
    d->clearError();
    if (d->fileName.isEmpty()) {
        d->setError(InvalidPath, QStringLiteral("Secure file path is empty"));
        return false;
    }
    if (data.size() > d->effectiveMaximumSize()) {
        d->setError(TooLarge, QStringLiteral("Secure data exceeds the configured size limit"));
        return false;
    }

    if (!validExistingWindowsTarget(d->fileName)) {
        d->setError(InvalidFile, QStringLiteral("Secure file target is not a regular file"));
        return false;
    }

    const QFileInfo targetInfo(d->fileName);
    const QDir      directory(targetInfo.absolutePath());
    if (!directory.exists()) {
        d->setError(InvalidPath, QStringLiteral("Secure file directory does not exist"));
        return false;
    }

    QString temporaryName;
    HANDLE  temporary = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 64; ++attempt) {
        const QString suffix = QUuid::createUuid().toString(QUuid::Id128);
        temporaryName        = directory.filePath(QStringLiteral(".%1.qca-%2.tmp").arg(targetInfo.fileName(), suffix));
        temporary            = CreateFileW(reinterpret_cast<LPCWSTR>(temporaryName.utf16()),
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_NEW,
                                FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_FLAG_WRITE_THROUGH,
                                nullptr);
        if (temporary != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            break;
    }

    if (temporary == INVALID_HANDLE_VALUE) {
        d->setError(OpenError, QStringLiteral("Unable to create secure temporary file"));
        return false;
    }

    qint64 written = 0;
    while (written < data.size()) {
        const DWORD chunk = static_cast<DWORD>(std::min<qint64>(data.size() - written, 1024 * 1024));
        DWORD       count = 0;
        if (!WriteFile(temporary, data.constData() + written, chunk, &count, nullptr) || count == 0) {
            CloseHandle(temporary);
            DeleteFileW(reinterpret_cast<LPCWSTR>(temporaryName.utf16()));
            d->setError(WriteError, QStringLiteral("Unable to write secure temporary file"));
            return false;
        }
        written += count;
    }

    if (!FlushFileBuffers(temporary)) {
        CloseHandle(temporary);
        DeleteFileW(reinterpret_cast<LPCWSTR>(temporaryName.utf16()));
        d->setError(WriteError, QStringLiteral("Unable to flush secure temporary file"));
        return false;
    }
    CloseHandle(temporary);

    if (!MoveFileExW(reinterpret_cast<LPCWSTR>(temporaryName.utf16()),
                     reinterpret_cast<LPCWSTR>(d->fileName.utf16()),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(reinterpret_cast<LPCWSTR>(temporaryName.utf16()));
        d->setError(CommitError, QStringLiteral("Unable to atomically replace secure file"));
        return false;
    }

    return true;
}

#else

static void setCloseOnExec(int fd)
{
#ifndef O_CLOEXEC
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0)
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
#else
    Q_UNUSED(fd)
#endif
}

static void enableCacheAvoidance(int fd)
{
#if defined(Q_OS_MACOS) && defined(F_NOCACHE)
    (void)fcntl(fd, F_NOCACHE, 1);
#else
    Q_UNUSED(fd)
#endif
}

static void discardFileCache(int fd)
{
#ifdef POSIX_FADV_DONTNEED
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
#else
    Q_UNUSED(fd)
#endif
}

static void closeReadFile(int fd)
{
    discardFileCache(fd);
    ::close(fd);
}

SecureArray SecureFile::read()
{
    d->clearError();
    if (d->fileName.isEmpty()) {
        d->setError(InvalidPath, QStringLiteral("Secure file path is empty"));
        return SecureArray();
    }

    const QByteArray encodedName = QFile::encodeName(d->fileName);
    int              flags       = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#else
    struct stat pathInfo;
    if (lstat(encodedName.constData(), &pathInfo) != 0) {
        d->setError(OpenError, QStringLiteral("Unable to inspect secure file"));
        return SecureArray();
    }
    if (S_ISLNK(pathInfo.st_mode)) {
        d->setError(InvalidFile, QStringLiteral("Secure file must not be a symbolic link"));
        return SecureArray();
    }
#endif

    int fd;
    do {
        fd = ::open(encodedName.constData(), flags);
    } while (fd < 0 && errno == EINTR);
    if (fd < 0) {
        d->setError(errno == ELOOP ? InvalidFile : OpenError,
                    errno == ELOOP ? QStringLiteral("Secure file must not be a symbolic link")
                                   : QStringLiteral("Unable to open secure file"));
        return SecureArray();
    }
    setCloseOnExec(fd);
    enableCacheAvoidance(fd);

    struct stat info;
    if (fstat(fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        closeReadFile(fd);
        d->setError(InvalidFile, QStringLiteral("Secure file is not a regular file"));
        return SecureArray();
    }
#ifndef O_NOFOLLOW
    struct stat pathInfoAfter;
    if (lstat(encodedName.constData(), &pathInfoAfter) != 0 || S_ISLNK(pathInfoAfter.st_mode) ||
        pathInfoAfter.st_dev != info.st_dev || pathInfoAfter.st_ino != info.st_ino) {
        closeReadFile(fd);
        d->setError(InvalidFile, QStringLiteral("Secure file changed while it was opened"));
        return SecureArray();
    }
#endif

    const qint64 limit = d->effectiveMaximumSize();
    if (info.st_size < 0 || static_cast<quint64>(info.st_size) > static_cast<quint64>(limit)) {
        closeReadFile(fd);
        d->setError(TooLarge, QStringLiteral("Secure file exceeds the configured size limit"));
        return SecureArray();
    }

    qint64      used     = 0;
    int         capacity = static_cast<int>(std::min<qint64>(info.st_size, limit));
    SecureArray result(capacity);

    for (;;) {
        if (used == capacity) {
            if (used == limit) {
                SecureArray probe(1);
                ssize_t     count;
                do {
                    count = ::read(fd, probe.data(), 1);
                } while (count < 0 && errno == EINTR);
                if (count < 0) {
                    closeReadFile(fd);
                    d->setError(ReadError, QStringLiteral("Unable to read secure file"));
                    return SecureArray();
                }
                if (count != 0) {
                    closeReadFile(fd);
                    d->setError(TooLarge, QStringLiteral("Secure file exceeds the configured size limit"));
                    return SecureArray();
                }
                break;
            }

            const qint64 next = std::min<qint64>(limit, used + 64 * 1024);
            if (!result.resize(static_cast<int>(next))) {
                closeReadFile(fd);
                d->setError(ReadError, QStringLiteral("Unable to allocate secure file buffer"));
                return SecureArray();
            }
            capacity = static_cast<int>(next);
        }

        ssize_t count;
        do {
            count = ::read(fd, result.data() + used, static_cast<size_t>(capacity - used));
        } while (count < 0 && errno == EINTR);
        if (count < 0) {
            closeReadFile(fd);
            d->setError(ReadError, QStringLiteral("Unable to read secure file"));
            return SecureArray();
        }
        if (count == 0)
            break;
        used += count;
    }

    closeReadFile(fd);
    if (result.size() != used)
        result.resize(static_cast<int>(used));
    return result;
}

bool SecureFile::write(const SecureArray &data)
{
    d->clearError();
    if (d->fileName.isEmpty()) {
        d->setError(InvalidPath, QStringLiteral("Secure file path is empty"));
        return false;
    }
    if (data.size() > d->effectiveMaximumSize()) {
        d->setError(TooLarge, QStringLiteral("Secure data exceeds the configured size limit"));
        return false;
    }

    const QByteArray encodedTarget = QFile::encodeName(d->fileName);
    struct stat      targetInfo;
    if (lstat(encodedTarget.constData(), &targetInfo) == 0) {
        if (S_ISLNK(targetInfo.st_mode) || !S_ISREG(targetInfo.st_mode)) {
            d->setError(InvalidFile, QStringLiteral("Secure file target is not a regular file"));
            return false;
        }
    } else if (errno != ENOENT) {
        d->setError(OpenError, QStringLiteral("Unable to inspect secure file target"));
        return false;
    }

    const QFileInfo fileInfo(d->fileName);
    const QDir      directory(fileInfo.absolutePath());
    if (!directory.exists()) {
        d->setError(InvalidPath, QStringLiteral("Secure file directory does not exist"));
        return false;
    }

    QString    temporaryPattern = directory.filePath(QStringLiteral(".%1.qca-XXXXXX").arg(fileInfo.fileName()));
    QByteArray encodedTemporary = QFile::encodeName(temporaryPattern);
    int        fd               = mkstemp(encodedTemporary.data());
    if (fd < 0) {
        d->setError(OpenError, QStringLiteral("Unable to create secure temporary file"));
        return false;
    }
    setCloseOnExec(fd);
    enableCacheAvoidance(fd);
    if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        ::close(fd);
        ::unlink(encodedTemporary.constData());
        d->setError(OpenError, QStringLiteral("Unable to restrict secure temporary file permissions"));
        return false;
    }

    qint64 written = 0;
    while (written < data.size()) {
        ssize_t count;
        do {
            count = ::write(fd, data.constData() + written, static_cast<size_t>(data.size() - written));
        } while (count < 0 && errno == EINTR);
        if (count <= 0) {
            ::close(fd);
            ::unlink(encodedTemporary.constData());
            d->setError(WriteError, QStringLiteral("Unable to write secure temporary file"));
            return false;
        }
        written += count;
    }

    int syncResult;
    do {
        syncResult = fsync(fd);
    } while (syncResult != 0 && errno == EINTR);
    if (syncResult != 0) {
        ::close(fd);
        ::unlink(encodedTemporary.constData());
        d->setError(WriteError, QStringLiteral("Unable to flush secure temporary file"));
        return false;
    }
    discardFileCache(fd);
    ::close(fd);

    if (::rename(encodedTemporary.constData(), encodedTarget.constData()) != 0) {
        ::unlink(encodedTemporary.constData());
        d->setError(CommitError, QStringLiteral("Unable to atomically replace secure file"));
        return false;
    }

#ifdef O_DIRECTORY
    const QByteArray encodedDirectory = QFile::encodeName(directory.absolutePath());
    int              directoryFlags   = O_RDONLY | O_DIRECTORY;
#ifdef O_CLOEXEC
    directoryFlags |= O_CLOEXEC;
#endif
    int directoryFd;
    do {
        directoryFd = ::open(encodedDirectory.constData(), directoryFlags);
    } while (directoryFd < 0 && errno == EINTR);
    if (directoryFd >= 0) {
        setCloseOnExec(directoryFd);
        do {
            syncResult = fsync(directoryFd);
        } while (syncResult != 0 && errno == EINTR);
        ::close(directoryFd);
    }
#endif

    return true;
}

#endif

} // namespace QCA
