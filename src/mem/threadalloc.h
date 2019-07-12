#pragma once

#include "../ds/helpers.h"
#include "globalalloc.h"
#if defined(SNMALLOC_USE_THREAD_DESTRUCTOR) && \
  defined(SNMALLOC_USE_THREAD_CLEANUP)
#error At most one out of SNMALLOC_USE_THREAD_CLEANUP and SNMALLOC_USE_THREAD_DESTRUCTOR may be defined.
#endif

namespace snmalloc
{
  extern "C" void _malloc_thread_cleanup(void);

  /**
   * A global fake allocator object.  This never allocates memory and, as a
   * result, never owns any slabs.  On the slow paths, where it would fetch
   * slabs to allocate from, it will discover that it is the placeholder and
   * replace itself with the thread-local allocator, allocating one if
   * required.  This avoids a branch on the fast path.
   */
  HEADER_GLOBAL Alloc GlobalPlaceHolder(
    default_memory_provider, SNMALLOC_DEFAULT_PAGEMAP(), nullptr, true);

#ifdef SNMALLOC_EXTERNAL_THREAD_ALLOC
  /**
   * Version of the `ThreadAlloc` interface that does no management of thread
   * local state, and just assumes that "ThreadAllocUntyped::get" has been
   * declared before including snmalloc.h.  As it is included before, it cannot
   * know the allocator type, hence the casting.
   *
   * This class is used only when snmalloc is compiled as part of a runtime,
   * which has its own management of the thread local allocator pointer.
   */
  class ThreadAllocUntypedWrapper
  {
  public:
    static SNMALLOC_FAST_PATH Alloc*& get()
    {
      return (Alloc*&)ThreadAllocUntyped::get();
    }
    static void register_cleanup() {}
  };
#endif

  /**
   * Version of the `ThreadAlloc` interface that uses a hook provided by libc
   * to destroy thread-local state.  This is the ideal option, because it
   * enforces ordering of destruction such that the malloc state is destroyed
   * after anything that can allocate memory.
   *
   * This class is used only when snmalloc is compiled as part of a compatible
   * libc (for example, FreeBSD libc).
   */
  class ThreadAllocLibcCleanup
  {
    /**
     * Libc will call `_malloc_thread_cleanup` just before a thread terminates.
     * This function must be allowed to call back into this class to destroy
     * the state.
     */
    friend void _malloc_thread_cleanup(void);

    /**
     * Function called when the thread exits.  This is guaranteed to be called
     * precisely once per thread and releases the current allocator.
     */
    static inline void exit()
    {
      auto* per_thread = get();
      if ((per_thread != &GlobalPlaceHolder) && (per_thread != nullptr))
      {
        current_alloc_pool()->release(per_thread);
        per_thread = nullptr;
      }
    }

  public:
    /**
     * Returns a pointer to the allocator associated with this thread.  If
     * `create` is true, it will create an allocator if one does not exist,
     * otherwise it will return `nullptr` in this case.  This should be called
     * with `create == false` only during thread teardown.
     *
     * The non-create case exists so that the `per_thread` variable can be a
     * local static and not a global, allowing ODR to deduplicate it.
     */
    static SNMALLOC_FAST_PATH Alloc*& get()
    {
      static thread_local Alloc* per_thread = &GlobalPlaceHolder;
      return per_thread;
    }
    static void register_cleanup() {}
  };

  /**
   * Helper class to execute a specified function on destruction.
   */
  template<void f()>
  class OnDestruct
  {
  public:
    ~OnDestruct()
    {
      f();
    }
  };

  /**
   * Version of the `ThreadAlloc` interface that uses C++ `thread_local`
   * destructors for cleanup.  If a per-thread allocator is used during the
   * destruction of other per-thread data, this class will create a new
   * instance and register its destructor, so should eventually result in
   * cleanup, but may result in allocators being returned to the global pool
   * and then reacquired multiple times.
   *
   * This implementation depends on nothing outside of a working C++
   * environment and so should be the simplest for initial bringup on an
   * unsupported platform.  It is currently used in the FreeBSD kernel version.
   */
  class ThreadAllocThreadDestructor
  {
    template<void f()>
    friend class OnDestruct;

    /**
     * Releases the allocator owned by this thread.
     */
    static void inner_release()
    {
      if (get() != &GlobalPlaceHolder)
      {
        current_alloc_pool()->release(get());
        get() = &GlobalPlaceHolder;
      }
    }

#ifdef USE_SNMALLOC_STATS
    static void print_stats()
    {
      Stats s;
      current_alloc_pool()->aggregate_stats(s);
      s.print<Alloc>(std::cout);
    }

    static int atexit_print_stats()
    {
      return atexit(print_stats);
    }
#endif

  public:
    /**
     * Public interface, returns the allocator for this thread, constructing
     * one if necessary.
     */
    static inline Alloc*& get()
    {
      static thread_local Alloc* alloc = &GlobalPlaceHolder;
      return alloc;
    }

    static void register_cleanup()
    {
      static thread_local OnDestruct<ThreadAllocThreadDestructor::inner_release>
        tidier;

#ifdef USE_SNMALLOC_STATS
      Singleton<int, atexit_print_stats>::get();
#endif
    }
  };

#ifdef SNMALLOC_USE_THREAD_CLEANUP
  /**
   * Entry point the allows libc to call into the allocator for per-thread
   * cleanup.
   */
  extern "C" void _malloc_thread_cleanup(void)
  {
    ThreadAllocLibcCleanup::exit();
  }
  using ThreadAlloc = ThreadAllocLibcCleanup;
#elif defined(SNMALLOC_USE_THREAD_DESTRUCTOR)
  using ThreadAlloc = ThreadAllocThreadDestructor;
#elif defined(SNMALLOC_EXTERNAL_THREAD_ALLOC)
  using ThreadAlloc = ThreadAllocUntypedWrapper;
#else
  using ThreadAlloc = ThreadAllocThreadDestructor;
#endif

  /**
   * Slow path for the placeholder replacement.  The simple check that this is
   * the global placeholder is inlined, the rest of it is only hit in a very
   * unusual case and so should go off the fast path.
   */
  SNMALLOC_SLOW_PATH inline void* lazy_replacement_slow()
  {
    auto*& local_alloc = ThreadAlloc::get();
    if ((local_alloc != nullptr) && (local_alloc != &GlobalPlaceHolder))
    {
      return local_alloc;
    }
    local_alloc = current_alloc_pool()->acquire();
    assert(local_alloc != &GlobalPlaceHolder);
    ThreadAlloc::register_cleanup();
    return local_alloc;
  }

  /**
   * Function passed as a template parameter to `Allocator` to allow lazy
   * replacement.  This is called on all of the slow paths in `Allocator`.  If
   * the caller is the global placeholder allocator then this function will
   * check if we've already allocated a per-thread allocator, returning it if
   * so.  If we have not allocated a per-thread allocator yet, then this
   * function will allocate one.
   */
  SNMALLOC_FAST_PATH void* lazy_replacement(void* existing)
  {
    if (existing != &GlobalPlaceHolder)
    {
      return nullptr;
    }
    return lazy_replacement_slow();
  }

} // namespace snmalloc
