/**
 * @file event_loop_factory.cc
 * @brief Selects and constructs the platform's EventLoop backend.
 */

#include "codicis/event/event_loop.h"

#if defined(CODICIS_HAVE_KQUEUE)
#include "kqueue_loop.h"
#elif defined(CODICIS_HAVE_EPOLL)
#include "epoll_loop.h"
#endif

namespace codicis {

Result<std::unique_ptr<EventLoop>> MakeEventLoop(Clock* clock) {
#if defined(CODICIS_HAVE_KQUEUE)
  Result<std::unique_ptr<KqueueLoop>> r = KqueueLoop::Create(clock);
  if (!r.ok()) {
    return r.error();
  }
  return std::unique_ptr<EventLoop>(std::move(r.value()));
#elif defined(CODICIS_HAVE_EPOLL)
  Result<std::unique_ptr<EpollLoop>> r = EpollLoop::Create(clock);
  if (!r.ok()) {
    return r.error();
  }
  return std::unique_ptr<EventLoop>(std::move(r.value()));
#else
  (void)clock;
  return MakeError(ErrorCode::kInternal, "no event backend compiled in");
#endif
}

}  // namespace codicis
