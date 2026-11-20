#pragma once
#include "core/Project.h"
namespace openframe {
enum class BookmarkFilter { All, Empty, Notes, Annotations };
struct BookmarkContent {
    bool notes = false;
    bool annotations = false;
};
BookmarkContent bookmarkContent(const Project &, const Bookmark &);
bool bookmarkMatches(const Project &, const Bookmark &, BookmarkFilter);
} // namespace openframe
