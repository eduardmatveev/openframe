#include "TabletInput.h"
#include <QGuiApplication>
#include <QPointingDevice>
#if defined(Q_OS_WIN) && QT_VERSION == QT_VERSION_CHECK(6, 8, 3)
#include <QtGui/private/qguiapplication_p.h>
#define OPENFRAME_QT_WINTAB_INTERFACE 1
#endif
namespace openframe {
bool tabletBackendSwitchSupported() {
#ifdef OPENFRAME_QT_WINTAB_INTERFACE
    return QGuiApplication::platformName() == "windows" && qGuiApp &&
           qGuiApp->nativeInterface<QNativeInterface::Private::QWindowsApplication>();
#else
    return false;
#endif
}
QString activeTabletBackend() {
#ifdef OPENFRAME_QT_WINTAB_INTERFACE
    if (tabletBackendSwitchSupported()) {
        auto native = qGuiApp->nativeInterface<QNativeInterface::Private::QWindowsApplication>();
        return native->isWinTabEnabled() ? "wintab" : "windows_ink";
    }
#endif
    return "qt";
}
bool applyTabletBackend(const QString &backend, QString *error) {
    if (backend != "windows_ink" && backend != "wintab") {
        if (error)
            *error = "Unknown tablet backend.";
        return false;
    }
#ifdef OPENFRAME_QT_WINTAB_INTERFACE
    if (tabletBackendSwitchSupported()) {
        auto native = qGuiApp->nativeInterface<QNativeInterface::Private::QWindowsApplication>();
        const bool previous = native->isWinTabEnabled();
        const bool requested = backend == "wintab";
        const bool accepted = native->setWinTabEnabled(requested);
        // Qt 6.8.3 initTablet() can return true after an unsuccessful driver load.
        if (accepted && native->isWinTabEnabled() == requested)
            return true;
        native->setWinTabEnabled(previous);
        if (error)
            *error = "WinTab could not be enabled. A working tablet driver is required; the "
                     "previous input backend remains active.";
        return false;
    }
#endif
    if (error)
        *error = "Tablet backend switching requires the Windows platform plugin in Qt 6.8.3. "
                 "Qt tablet events remain available on supported platforms.";
    return false;
}
bool acceptsTabletDevice(const QPointingDevice *device, const QString &backend) {
    if (!device)
        return false;
    const auto name = device->name();
    if (name == "wintab")
        return backend == "wintab";
    if (name == "wmpointer")
        return backend != "wintab";
    return true;
}
} // namespace openframe
