#pragma once
#include <QStringList>

namespace openframe::fileDialogDetail {
// Shared by the native shell adapter and the deterministic fallback. These
// functions do not display a dialog or inspect an unvalidated remote location.
enum class Selection { OpenFile, OpenFiles, SaveFile, Directory };
bool useNativeWindowsDialogs(const QString &platformName, bool nativeDialogsDisabled);
QString safeInitialPath(const QString &initial, bool directoryOnly = false);
QStringList validateSelection(const QStringList &paths, Selection mode, QString *error = nullptr);
} // namespace openframe::fileDialogDetail
