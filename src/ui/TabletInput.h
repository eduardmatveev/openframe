#pragma once
#include <QString>
class QPointingDevice;
namespace openframe {
bool tabletBackendSwitchSupported();
QString activeTabletBackend();
bool applyTabletBackend(const QString &backend, QString *error = nullptr);
bool acceptsTabletDevice(const QPointingDevice *device, const QString &backend);
} // namespace openframe
