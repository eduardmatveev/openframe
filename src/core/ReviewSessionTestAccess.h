#pragma once
#ifdef OPENFRAME_TESTING
#include <functional>
namespace openframe {
class ReviewSession;
// A deterministic worker barrier for concurrency regressions. No QObject slot
// or external API exposes this; omitted when building without tests.
class ReviewSessionTestAccess {
  public:
    static void enqueueWorker(ReviewSession &, std::function<void()> operation);
};
} // namespace openframe
#endif
