#pragma once
#include "core/Project.h"
#include "MediaDecoder.h"
namespace openframe {
bool validateExportSettings(const Project &, const ExportSettings &, QString *error = nullptr);
// Returns the published file, or the first published image for a sequence.
QString exportProject(const Project &, const ExportSettings &, const CancelCheck &,
                      const ProgressCallback &);
}
