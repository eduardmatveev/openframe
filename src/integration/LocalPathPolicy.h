#pragma once

#include <QString>

namespace openframe {

// Local paths only. Deliberately rejects all symlinks/reparse points: validating
// a target after following it could itself contact a remote filesystem.
class LocalPathPolicy final {
  public:
    static QString validateInput(const QString &path, QString *error = nullptr);
    static QString validateOutput(const QString &path, QString *error = nullptr);
    static QString validateDirectory(const QString &path, QString *error = nullptr);
    // Allows an absent final directory, but requires all existing parents local.
    static QString validateDirectoryOutput(const QString &path, QString *error = nullptr);
    static bool isAllowedAddress(const QString &address);

  private:
    enum class Kind { Input, Output, Directory, DirectoryOutput };
    static QString validate(const QString &path, Kind kind, QString *error);
};

} // namespace openframe
