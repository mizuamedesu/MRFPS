#pragma once

#include <atomic>
#include <cstdint>

// This state never waits for the JPEG mailbox mutex. Generations are nonzero,
// monotonically increasing, and published only by the game thread while it
// owns that mailbox. Delayed callbacks cannot retire a newer generation.
namespace MRFPSCaptureSessionGuards
{
enum class EBusyDecision : std::uint8_t { NotBusy, Waiting, Retry };
inline void Advance(std::atomic<std::uint64_t>& Value, std::uint64_t Generation)
{
    std::uint64_t Previous = Value.load(std::memory_order_relaxed);
    while (Previous < Generation && !Value.compare_exchange_weak(Previous, Generation,
            std::memory_order_release, std::memory_order_relaxed)) {}
}

struct FSessionState
{
    std::atomic<std::uint64_t> Active{0}, Ready{0}, Busy{0}, Failed{0}, FrameSession{0};
    std::atomic<double> FrameAt{0};

    bool IsActive(std::uint64_t Generation) const
    { return Generation && Active.load(std::memory_order_acquire) == Generation; }

    void Start(std::uint64_t Generation)
    {
        FrameSession.store(0, std::memory_order_release);
        Active.store(Generation, std::memory_order_release);
    }

    bool Retire(std::uint64_t Generation)
    {
        if (!Generation || !Active.compare_exchange_strong(Generation, 0,
                std::memory_order_acq_rel, std::memory_order_acquire)) return false;
        ClearFrame(Generation);
        return true;
    }

    void ClearFrame(std::uint64_t Generation)
    { FrameSession.compare_exchange_strong(Generation, 0, std::memory_order_acq_rel); }

    void MarkReady(std::uint64_t Generation)
    { if (IsActive(Generation)) Advance(Ready, Generation); }

    // A Busy response means Java processed this generation but did not open a
    // new consent dialog because an older dialog is still outstanding. Record
    // it independently of the mailbox lock so it cannot be lost on contention.
    void MarkBusy(std::uint64_t Generation)
    { if (IsActive(Generation)) Advance(Busy, Generation); }

    void MarkFailed(std::uint64_t Generation)
    {
        if (IsActive(Generation)) { Advance(Failed, Generation); ClearFrame(Generation); }
    }

    bool IsReady(std::uint64_t Generation) const
    { return IsActive(Generation) && Ready.load(std::memory_order_acquire) == Generation; }

    bool IsBusy(std::uint64_t Generation) const
    { return IsActive(Generation) && Busy.load(std::memory_order_acquire) == Generation; }

    bool IsFailed(std::uint64_t Generation) const
    { return IsActive(Generation) && Failed.load(std::memory_order_acquire) == Generation; }

    EBusyDecision DecideBusy(std::uint64_t Generation, bool Known, bool OldConsentPending)
    {
        if (!IsBusy(Generation)) return EBusyDecision::NotBusy;
        if (!Known || OldConsentPending) return EBusyDecision::Waiting;
        // Java already rejected this generation; when the old dialog drains it
        // needs a fresh explicit request, not another 180 seconds of waiting.
        MarkFailed(Generation);
        return EBusyDecision::Retry;
    }

    // Caller owns the JPEG mailbox. A concurrent retirement still makes Fresh
    // false; a newer session cannot publish until this mailbox is unlocked.
    bool PublishFrame(std::uint64_t Generation, double Now)
    {
        if (!IsActive(Generation) || IsFailed(Generation)) return false;
        FrameAt.store(Now, std::memory_order_relaxed);
        FrameSession.store(Generation, std::memory_order_release);
        return IsActive(Generation) && !IsFailed(Generation);
    }

    bool Fresh(std::uint64_t Generation, double Now, double MaximumAge) const
    {
        if (!IsActive(Generation) || IsFailed(Generation) || FrameSession.load(std::memory_order_acquire) != Generation) return false;
        const double At = FrameAt.load(std::memory_order_relaxed);
        const double Age = Now - At;
        return At > 0 && Age >= 0 && Age <= MaximumAge && IsActive(Generation) && !IsFailed(Generation);
    }
};

// UE's FCriticalSection and test mutex adapters expose TryLock/Unlock. Never
// wait on a JNI worker from the game thread or Android's UI thread.
template<typename MutexType> class TTryLock
{
public:
    explicit TTryLock(MutexType& InMutex) : Mutex(InMutex), bLocked(Mutex.TryLock()) {}
    ~TTryLock() { if (bLocked) Mutex.Unlock(); }
    explicit operator bool() const { return bLocked; }
    TTryLock(const TTryLock&) = delete;
    TTryLock& operator=(const TTryLock&) = delete;
private:
    MutexType& Mutex;
    bool bLocked;
};
}
