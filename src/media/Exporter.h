#pragma once
#include "core/Project.h"
#include "MediaDecoder.h"
namespace openframe {
bool validateExportSettings(const Project &, const ExportSettings &, QString *error = nullptr);
void exportProject(const Project &, const ExportSettings &, const CancelCheck &,
                   const ProgressCallback &);
}
