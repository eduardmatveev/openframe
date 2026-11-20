#include "LocalPathPolicy.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#ifdef Q_OS_LINUX
#include <sys/vfs.h>
#endif
#endif

namespace openframe {
namespace {
QString fail(QString *error, const QString &reason) {
    if (error)
        *error = reason;
    return {};
}
} // namespace

bool LocalPathPolicy::isAllowedAddress(const QString &address) {
    return address == QStringLiteral("127.0.0.1");
}
QString LocalPathPolicy::validateInput(const QString &path, QString *error) {
    return validate(path, Kind::Input, error);
}
QString LocalPathPolicy::validateOutput(const QString &path, QString *error) {
    return validate(path, Kind::Output, error);
}
QString LocalPathPolicy::validateDirectory(const QString &path, QString *error) {
    return validate(path, Kind::Directory, error);
}
QString LocalPathPolicy::validateDirectoryOutput(const QString &path, QString *error) {
    return validate(path, Kind::DirectoryOutput, error);
}
QString LocalPathPolicy::validate(const QString &path, Kind kind, QString *error) {
    if (error)
        error->clear();
    if (path.isEmpty() || path.size() > 32000)
        return fail(error, QStringLiteral("A nonempty local filesystem path is required"));
    for (const auto c : path)
        if (c.unicode() < 32 || c == QChar(127))
            return fail(error, QStringLiteral("Control characters are forbidden in paths"));
    QString normalized = path;
    normalized.replace('\\', '/');
    if (normalized.startsWith("//") || normalized.startsWith("/??/") || normalized.contains("://"))
        return fail(error, QStringLiteral("URLs, UNC paths and device namespaces are forbidden"));
    const bool drivePath = normalized.size() >= 3 && normalized[0].isLetter() &&
                           normalized[1] == ':' && normalized[2] == '/';
    if (normalized.contains(':') && (!drivePath || normalized.mid(2).contains(':')))
        return fail(error, QStringLiteral(
                               "URLs, drive-relative paths and alternate streams are forbidden"));
    // Do not let path normalization hide a reparse component (link/../file).
    const auto rawParts = normalized.split('/', Qt::SkipEmptyParts);
    for (const QString &part : rawParts) {
        if (part == "..")
            return fail(error, QStringLiteral(
                                   "Parent traversal is forbidden; provide a resolved local path"));
#ifdef Q_OS_WIN
        if (part != "." && (part.endsWith(' ') || part.endsWith('.')))
            return fail(error, QStringLiteral("Ambiguous Windows path components are forbidden"));
        const QString stem = part.section('.', 0, 0).toUpper();
        static const QRegularExpression device(
            QStringLiteral("^(CON|PRN|AUX|NUL|COM[0-9]|LPT[0-9])$"));
        if (device.match(stem).hasMatch())
            return fail(error, QStringLiteral("Windows device names are forbidden"));
#endif
    }
    const QString absolute = QDir::cleanPath(QFileInfo(normalized).absoluteFilePath());
#ifdef Q_OS_WIN
    if (absolute.size() < 3 || absolute[1] != ':' || absolute[2] != '/')
        return fail(error, QStringLiteral("A drive-qualified local path is required"));
    const QString root = QDir::toNativeSeparators(absolute.left(3));
    const UINT driveType = GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
    if (driveType != DRIVE_FIXED && driveType != DRIVE_REMOVABLE && driveType != DRIVE_CDROM &&
        driveType != DRIVE_RAMDISK)
        return fail(error, QStringLiteral("The drive is unavailable or is not a local drive"));
    // A substituted drive must not hide another path requiring validation.
    wchar_t deviceTarget[32768]{};
    const QString drive = absolute.left(2);
    if (!QueryDosDeviceW(reinterpret_cast<LPCWSTR>(drive.utf16()), deviceTarget, 32768))
        return fail(error, QStringLiteral("Cannot establish the drive mapping"));
    const QString mapping = QString::fromWCharArray(deviceTarget);
    if (mapping.startsWith("\\??\\") || mapping.contains("Mup", Qt::CaseInsensitive) ||
        mapping.contains("LanmanRedirector", Qt::CaseInsensitive))
        return fail(error, QStringLiteral("Substituted and network drive mappings are forbidden"));
    QString walked = absolute.left(3);
    const auto parts = absolute.mid(3).split('/', Qt::SkipEmptyParts);
    if (parts.isEmpty() && kind != Kind::Directory && kind != Kind::DirectoryOutput)
        return fail(error, QStringLiteral("A file path is required"));
    for (qsizetype i = 0; i <= parts.size(); ++i) {
        if (i > 0) {
            if (!walked.endsWith('/'))
                walked += '/';
            walked += parts[i - 1];
        }
        const bool leaf = i == parts.size();
        const QString native = QDir::toNativeSeparators(walked);
        HANDLE handle = CreateFileW(
            reinterpret_cast<LPCWSTR>(native.utf16()), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD code = GetLastError();
            if (leaf && (kind == Kind::Output || kind == Kind::DirectoryOutput) &&
                code == ERROR_FILE_NOT_FOUND)
                return absolute;
            return fail(error,
                        QStringLiteral("Local path cannot be opened (Windows error %1)").arg(code));
        }
        BY_HANDLE_FILE_INFORMATION info{};
        const bool gotInfo = GetFileInformationByHandle(handle, &info);
        CloseHandle(handle);
        if (!gotInfo || (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
            return fail(error, QStringLiteral("Reparse points and symbolic links are forbidden"));
        const bool directory = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!leaf && !directory)
            return fail(error, QStringLiteral("A parent path is not a directory"));
        if (leaf && directory != (kind == Kind::Directory || kind == Kind::DirectoryOutput))
            return fail(error, QStringLiteral("Unexpected local path type"));
    }
#else
    QString walked = "/";
    const auto parts = absolute.mid(1).split('/', Qt::SkipEmptyParts);
    for (qsizetype i = 0; i <= parts.size(); ++i) {
        if (i > 0)
            walked = QDir(walked).filePath(parts[i - 1]);
        struct stat st{};
        const bool leaf = i == parts.size();
        if (lstat(walked.toUtf8().constData(), &st) != 0) {
            if (leaf && (kind == Kind::Output || kind == Kind::DirectoryOutput) && errno == ENOENT)
                return absolute;
            return fail(error, QStringLiteral("Local path is inaccessible"));
        }
        if (S_ISLNK(st.st_mode))
            return fail(error, QStringLiteral("Symbolic links are forbidden"));
#ifdef Q_OS_LINUX
        struct statfs fs{};
        if (statfs(walked.toUtf8().constData(), &fs) != 0)
            return fail(error, QStringLiteral("Cannot establish filesystem type"));
        // NFS, SMB, CIFS, NCP, CODA, AFS, FUSE and 9P can contact remote hosts.
        const unsigned long type = static_cast<unsigned long>(fs.f_type);
        if (type == 0x6969 || type == 0x517B || type == 0xFF534D42 || type == 0xFE534D42 ||
            type == 0x564C || type == 0x73757245 || type == 0x5346414F || type == 0x65735546 ||
            type == 0x01021997)
            return fail(error, QStringLiteral("Remote or userspace filesystems are forbidden"));
#endif
        if (!leaf && !S_ISDIR(st.st_mode))
            return fail(error, QStringLiteral("Parent is not a directory"));
        if (leaf &&
            ((kind == Kind::Directory || kind == Kind::DirectoryOutput) ? !S_ISDIR(st.st_mode)
                                                                        : !S_ISREG(st.st_mode)))
            return fail(error, QStringLiteral("Only regular local files are accepted"));
    }
#endif
    return absolute;
}
} // namespace openframe
