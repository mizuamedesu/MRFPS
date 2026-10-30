#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>

// Pure atomic admission policy shared by the receiver and its standalone
// stress test. A permit is held from decode through render acknowledgement.
namespace MRFPSVideoWorkBudget
{
constexpr int SlotCount = 2;
constexpr int MaxInFlight = 2;
constexpr int MaxWaitingReceivers = 64;
constexpr double DemandFreshSeconds = 0.5;

struct FWaiter
{
    std::uint64_t Id = 0;
    double LastSeen = 0;
};

struct FProcessBudget
{
    std::atomic<int> Active{0};
    // FIFO bookkeeping is accessed only on the game thread. It carries no
    // UObject, image or callback references and has a fixed upper bound.
    FWaiter Waiters[MaxWaitingReceivers]{};
    int WaiterCount = 0;
    std::uint64_t NextReceiverId = 1;
};

struct FReceiverBudget
{
    std::atomic<bool> Active[SlotCount];
    std::uint64_t Id = 0;
    const FProcessBudget* Process = nullptr;
    FReceiverBudget()
    {
        for (auto& Slot : Active) Slot.store(false, std::memory_order_relaxed);
    }
};

inline bool Acquire(FProcessBudget& Process, FReceiverBudget& Receiver, int Slot)
{
    if (Slot < 0 || Slot >= SlotCount) return false;
    bool Available = false;
    if (!Receiver.Active[Slot].compare_exchange_strong(Available, true,
            std::memory_order_acq_rel, std::memory_order_relaxed)) return false;
    int Current = Process.Active.load(std::memory_order_relaxed);
    while (Current < MaxInFlight)
    {
        if (Process.Active.compare_exchange_weak(Current, Current + 1,
                std::memory_order_acq_rel, std::memory_order_relaxed)) return true;
    }
    Receiver.Active[Slot].store(false, std::memory_order_release);
    return false;
}

// Exactly one release is made by the last reference to each permit. Returning
// false for an already idle slot prevents an accidental counter underflow.
inline bool Release(FProcessBudget& Process, FReceiverBudget& Receiver, int Slot)
{
    if (Slot < 0 || Slot >= SlotCount || !Receiver.Active[Slot].exchange(false, std::memory_order_acq_rel)) return false;
    Process.Active.fetch_sub(1, std::memory_order_acq_rel);
    return true;
}

inline unsigned BusyMask(const FReceiverBudget& Receiver)
{
    unsigned Result = 0;
    for (int Slot = 0; Slot < SlotCount; ++Slot)
        if (Receiver.Active[Slot].load(std::memory_order_acquire)) Result |= 1u << Slot;
    return Result;
}

// The following FIFO operations are game-thread only; the worker and render
// callbacks only call Release above. Unregistered receiver instances never
// consume storage, and stopped instances expire even if they cannot cancel.
inline void RemoveWaiter(FProcessBudget& Process, int Index)
{
    for (int I = Index + 1; I < Process.WaiterCount; ++I) Process.Waiters[I - 1] = Process.Waiters[I];
    --Process.WaiterCount;
    Process.Waiters[Process.WaiterCount] = {};
}

inline void CancelDemand(FProcessBudget& Process, const FReceiverBudget& Receiver)
{
    if (!Receiver.Id || Receiver.Process != &Process) return;
    for (int I = 0; I < Process.WaiterCount; ++I)
        if (Process.Waiters[I].Id == Receiver.Id) { RemoveWaiter(Process, I); return; }
}

inline bool AcquireFair(FProcessBudget& Process, FReceiverBudget& Receiver, int Slot, double Now)
{
    if (Slot < 0 || Slot >= SlotCount || !std::isfinite(Now) || Now < 0 ||
        Receiver.Active[Slot].load(std::memory_order_acquire)) return false;
    if (Receiver.Process && Receiver.Process != &Process) return false;
    if (!Receiver.Id)
    {
        if (!Process.NextReceiverId) return false;
        Receiver.Id = Process.NextReceiverId++;
        Receiver.Process = &Process;
    }
    for (int I = 0; I < Process.WaiterCount;)
    {
        const auto& Entry = Process.Waiters[I];
        if (Now < Entry.LastSeen || Now - Entry.LastSeen >= DemandFreshSeconds) RemoveWaiter(Process, I);
        else ++I;
    }
    int Position = 0;
    while (Position < Process.WaiterCount && Process.Waiters[Position].Id != Receiver.Id) ++Position;
    if (Position == Process.WaiterCount)
    {
        if (Process.WaiterCount == MaxWaitingReceivers) return false;
        Process.Waiters[Process.WaiterCount++] = {Receiver.Id, Now};
    }
    else Process.Waiters[Position].LastSeen = Now;
    if (Process.Waiters[0].Id != Receiver.Id || !Acquire(Process, Receiver, Slot)) return false;
    RemoveWaiter(Process, 0);
    return true;
}
}
