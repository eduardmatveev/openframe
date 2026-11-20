#include "BookmarkFilter.h"
#include <algorithm>
namespace openframe {
BookmarkContent bookmarkContent(const Project &project, const Bookmark &bookmark) {
    BookmarkContent result;
    result.notes = !bookmark.text.trimmed().isEmpty();
    const auto last = std::max(bookmark.frame, bookmark.endFrame);
    for (const auto &note : project.notes)
        if (note.timelineId == bookmark.timelineId && note.frame >= bookmark.frame &&
            note.frame <= last && !note.text.trimmed().isEmpty())
            result.notes = true;
    for (const auto &annotation : project.annotations)
        if ((annotation.timelineId == bookmark.timelineId || annotation.timelineId.isEmpty()) &&
            !annotation.background && annotation.frame >= bookmark.frame &&
            annotation.frame <= last &&
            (!annotation.points.isEmpty() || !annotation.text.isEmpty()))
            result.annotations = true;
    return result;
}
bool bookmarkMatches(const Project &project, const Bookmark &bookmark, BookmarkFilter filter) {
    if (filter == BookmarkFilter::All)
        return true;
    const auto content = bookmarkContent(project, bookmark);
    if (filter == BookmarkFilter::Notes)
        return content.notes;
    if (filter == BookmarkFilter::Annotations)
        return content.annotations;
    return !content.notes && !content.annotations;
}
} // namespace openframe
