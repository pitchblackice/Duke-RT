// Win32 threading/timing shims for the FidelityFX VK frame-interpolation
// swapchain, which is written against Win32 primitives even in the Vulkan
// backend. Only the subset the SDK actually calls is implemented:
//
//   CRITICAL_SECTION / Initialize|Delete|Enter|LeaveCriticalSection
//   CreateEvent / SetEvent / WaitForSingleObject / CloseHandle
//   CreateThread / SetThreadPriority
//   QueryPerformanceCounter / QueryPerformanceFrequency
//
// Events are auto-reset in every SDK call site, but manual-reset is supported
// for completeness. WaitForSingleObject accepts both event and thread handles,
// because the SDK uses it to join its presenter/interpolation threads.
#pragma once

#ifndef _WIN32

#include <pthread.h>
#include <cstdint>
#include <cstddef>
#include <ctime>
#include <new>

#ifndef WINAPI
#define WINAPI
#endif

#ifndef INFINITE
#define INFINITE 0xFFFFFFFFu
#endif

#ifndef WAIT_OBJECT_0
#define WAIT_OBJECT_0 0x00000000u
#endif

#ifndef WAIT_TIMEOUT
#define WAIT_TIMEOUT 0x00000102u
#endif

#ifndef THREAD_PRIORITY_HIGHEST
#define THREAD_PRIORITY_HIGHEST 2
#endif

#ifndef TEXT
#define TEXT(x) x
#endif

typedef uint32_t DWORD;
typedef int      BOOL;
typedef void*    HANDLE;
typedef void*    LPVOID;
typedef uint64_t UINT64;
typedef uint32_t UINT32;
typedef uint32_t UINT;
typedef int64_t  INT64;

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

union LARGE_INTEGER
{
    int64_t QuadPart;
};

// ---------------------------------------------------------------------------
// Critical sections (recursive, matching Win32 semantics)
// ---------------------------------------------------------------------------

struct CRITICAL_SECTION
{
    pthread_mutex_t mutex;
    bool            initialized;
};

inline void InitializeCriticalSection(CRITICAL_SECTION* cs)
{
    if (cs == nullptr)
        return;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&cs->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    cs->initialized = true;
}

inline void DeleteCriticalSection(CRITICAL_SECTION* cs)
{
    if (cs == nullptr || !cs->initialized)
        return;
    pthread_mutex_destroy(&cs->mutex);
    cs->initialized = false;
}

inline void EnterCriticalSection(CRITICAL_SECTION* cs)
{
    if (cs == nullptr)
        return;
    // Tolerate a zero-initialised section that was never explicitly created.
    if (!cs->initialized)
        InitializeCriticalSection(cs);
    pthread_mutex_lock(&cs->mutex);
}

inline void LeaveCriticalSection(CRITICAL_SECTION* cs)
{
    if (cs == nullptr || !cs->initialized)
        return;
    pthread_mutex_unlock(&cs->mutex);
}

// ---------------------------------------------------------------------------
// Handles: events and threads share the HANDLE type, so tag them.
// ---------------------------------------------------------------------------

namespace ffx_compat
{
    enum HandleKind
    {
        kHandleEvent = 1,
        kHandleThread = 2,
    };

    struct HandleBase
    {
        HandleKind kind;
    };

    struct EventHandle
    {
        HandleBase      base;
        pthread_mutex_t mutex;
        pthread_cond_t  cond;
        bool            signaled;
        bool            manualReset;
    };

    struct ThreadHandle
    {
        HandleBase base;
        pthread_t  thread;
        DWORD (*entry)(void*);
        void*      param;
        bool       joined;
    };

    inline void* ThreadTrampoline(void* raw)
    {
        ThreadHandle* handle = static_cast<ThreadHandle*>(raw);
        if (handle != nullptr && handle->entry != nullptr)
            handle->entry(handle->param);
        return nullptr;
    }
}

inline HANDLE CreateEvent(void* /*securityAttributes*/, BOOL manualReset, BOOL initialState, const char* /*name*/)
{
    ffx_compat::EventHandle* ev = new (std::nothrow) ffx_compat::EventHandle();
    if (ev == nullptr)
        return nullptr;
    ev->base.kind   = ffx_compat::kHandleEvent;
    ev->signaled    = initialState != FALSE;
    ev->manualReset = manualReset != FALSE;
    pthread_mutex_init(&ev->mutex, nullptr);
    pthread_cond_init(&ev->cond, nullptr);
    return ev;
}

inline BOOL SetEvent(HANDLE handle)
{
    ffx_compat::HandleBase* base = static_cast<ffx_compat::HandleBase*>(handle);
    if (base == nullptr || base->kind != ffx_compat::kHandleEvent)
        return FALSE;
    ffx_compat::EventHandle* ev = reinterpret_cast<ffx_compat::EventHandle*>(base);
    pthread_mutex_lock(&ev->mutex);
    ev->signaled = true;
    // Auto-reset events release exactly one waiter; manual-reset release all.
    if (ev->manualReset)
        pthread_cond_broadcast(&ev->cond);
    else
        pthread_cond_signal(&ev->cond);
    pthread_mutex_unlock(&ev->mutex);
    return TRUE;
}

inline BOOL ResetEvent(HANDLE handle)
{
    ffx_compat::HandleBase* base = static_cast<ffx_compat::HandleBase*>(handle);
    if (base == nullptr || base->kind != ffx_compat::kHandleEvent)
        return FALSE;
    ffx_compat::EventHandle* ev = reinterpret_cast<ffx_compat::EventHandle*>(base);
    pthread_mutex_lock(&ev->mutex);
    ev->signaled = false;
    pthread_mutex_unlock(&ev->mutex);
    return TRUE;
}

inline DWORD WaitForSingleObject(HANDLE handle, DWORD milliseconds)
{
    ffx_compat::HandleBase* base = static_cast<ffx_compat::HandleBase*>(handle);
    if (base == nullptr)
        return WAIT_TIMEOUT;

    if (base->kind == ffx_compat::kHandleThread)
    {
        // The SDK only ever waits INFINITE on a thread handle, i.e. a join.
        ffx_compat::ThreadHandle* th = reinterpret_cast<ffx_compat::ThreadHandle*>(base);
        if (!th->joined)
        {
            pthread_join(th->thread, nullptr);
            th->joined = true;
        }
        return WAIT_OBJECT_0;
    }

    ffx_compat::EventHandle* ev = reinterpret_cast<ffx_compat::EventHandle*>(base);
    pthread_mutex_lock(&ev->mutex);
    DWORD result = WAIT_OBJECT_0;
    if (milliseconds == INFINITE)
    {
        while (!ev->signaled)
            pthread_cond_wait(&ev->cond, &ev->mutex);
    }
    else
    {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec += (time_t)(milliseconds / 1000u);
        deadline.tv_nsec += (long)(milliseconds % 1000u) * 1000000L;
        if (deadline.tv_nsec >= 1000000000L)
        {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        while (!ev->signaled)
        {
            if (pthread_cond_timedwait(&ev->cond, &ev->mutex, &deadline) != 0)
            {
                result = ev->signaled ? WAIT_OBJECT_0 : WAIT_TIMEOUT;
                break;
            }
        }
    }
    if (result == WAIT_OBJECT_0 && !ev->manualReset)
        ev->signaled = false;
    pthread_mutex_unlock(&ev->mutex);
    return result;
}

inline BOOL CloseHandle(HANDLE handle)
{
    ffx_compat::HandleBase* base = static_cast<ffx_compat::HandleBase*>(handle);
    if (base == nullptr)
        return FALSE;

    if (base->kind == ffx_compat::kHandleThread)
    {
        ffx_compat::ThreadHandle* th = reinterpret_cast<ffx_compat::ThreadHandle*>(base);
        if (!th->joined)
        {
            pthread_detach(th->thread);
            th->joined = true;
        }
        delete th;
        return TRUE;
    }

    ffx_compat::EventHandle* ev = reinterpret_cast<ffx_compat::EventHandle*>(base);
    pthread_cond_destroy(&ev->cond);
    pthread_mutex_destroy(&ev->mutex);
    delete ev;
    return TRUE;
}

inline HANDLE CreateThread(void* /*securityAttributes*/,
                           size_t /*stackSize*/,
                           DWORD (*startAddress)(void*),
                           void* parameter,
                           DWORD /*creationFlags*/,
                           DWORD* threadId)
{
    ffx_compat::ThreadHandle* th = new (std::nothrow) ffx_compat::ThreadHandle();
    if (th == nullptr)
        return nullptr;
    th->base.kind = ffx_compat::kHandleThread;
    th->entry     = startAddress;
    th->param     = parameter;
    th->joined    = false;
    if (pthread_create(&th->thread, nullptr, ffx_compat::ThreadTrampoline, th) != 0)
    {
        delete th;
        return nullptr;
    }
    if (threadId != nullptr)
        *threadId = 0;
    return th;
}

// Debug thread naming is a diagnostic nicety with no Linux equivalent that the
// SDK depends on; accept and ignore it.
inline long SetThreadDescription(HANDLE /*thread*/, const wchar_t* /*description*/)
{
    return 0;   // S_OK
}

// Raising thread priority requires privileges we cannot assume; the SDK treats
// this as a best-effort hint, so report success without changing scheduling.
inline BOOL SetThreadPriority(HANDLE /*thread*/, int /*priority*/)
{
    return TRUE;
}

// ---------------------------------------------------------------------------
// High-resolution timing: report a 1 GHz counter backed by CLOCK_MONOTONIC.
// ---------------------------------------------------------------------------

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* counter)
{
    if (counter == nullptr)
        return FALSE;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    counter->QuadPart = (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
    return TRUE;
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* frequency)
{
    if (frequency == nullptr)
        return FALSE;
    frequency->QuadPart = 1000000000LL;
    return TRUE;
}

#endif // !_WIN32
