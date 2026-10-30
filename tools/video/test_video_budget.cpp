#include "../../Source/MRFPS/Private/MRFPSVideoWorkBudget.h"

#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

int main()
{
    using namespace MRFPSVideoWorkBudget;
    FProcessBudget Process;
    FReceiverBudget First, Second;
    if (Acquire(Process, First, -1) || Acquire(Process, First, SlotCount)) return 1;
    if (!Acquire(Process, First, 0) || !Acquire(Process, First, 1)) return 2;
    if (Process.Active.load() != 2 || BusyMask(First) != 3) return 3;
    if (Acquire(Process, First, 0) || Acquire(Process, Second, 0)) return 4;
    if (!Release(Process, First, 0) || !Acquire(Process, Second, 0)) return 5;
    if (!Release(Process, First, 1) || !Release(Process, Second, 0) || Release(Process, Second, 0)) return 6;
    if (Process.Active.load() != 0 || BusyMask(First) || BusyMask(Second)) return 7;

    // Reproduce fixed Tick order: receiver A runs first every frame. Receiver
    // B must get an available permit before A can take it again.
    FProcessBudget Fair;
    FReceiverBudget A, B;
    if (!AcquireFair(Fair,A,0,10) || !AcquireFair(Fair,A,1,10)) return 10;
    if (AcquireFair(Fair,B,0,10.01) || !Release(Fair,A,0)) return 11;
    if (AcquireFair(Fair,A,0,10.02) || !AcquireFair(Fair,B,0,10.03)) return 12;
    if (!Release(Fair,A,1) || AcquireFair(Fair,B,1,10.04) || !AcquireFair(Fair,A,1,10.05)) return 13;
    if (!Release(Fair,A,1) || !Release(Fair,B,0)) return 14;
    CancelDemand(Fair,B);
    if (Fair.WaiterCount) return 15;
    if (!AcquireFair(Fair,A,0,11) || !AcquireFair(Fair,A,1,11) || AcquireFair(Fair,B,0,11)) return 16;
    if (!Release(Fair,A,0) || !AcquireFair(Fair,A,0,11 + DemandFreshSeconds)) return 17;
    if (!Release(Fair,A,0) || !Release(Fair,A,1)) return 18;

    // Waiting metadata is bounded. An overflow receiver may retry after a
    // place is released without overwriting another receiver's queue entry.
    FProcessBudget Bounded;
    FReceiverBudget Occupier, Waiting[MaxWaitingReceivers + 1];
    if (!AcquireFair(Bounded,Occupier,0,20) || !AcquireFair(Bounded,Occupier,1,20)) return 19;
    for (int I = 0; I < MaxWaitingReceivers + 1; ++I)
        if (AcquireFair(Bounded,Waiting[I],0,20.01)) return 20;
    if (Bounded.WaiterCount != MaxWaitingReceivers) return 21;
    CancelDemand(Bounded,Waiting[0]);
    if (AcquireFair(Bounded,Waiting[MaxWaitingReceivers],0,20.02) || Bounded.WaiterCount != MaxWaitingReceivers) return 22;
    if (!Release(Bounded,Occupier,0) || !Release(Bounded,Occupier,1)) return 23;

    constexpr int Receivers = 3, Threads = 8, Attempts = 5000;
    FReceiverBudget Budgets[Receivers];
    std::atomic<int> Inside{0}, SlotInside[Receivers * SlotCount]{};
    std::atomic<int> Violations{0}, Acquisitions{0};
    std::vector<std::thread> Workers;
    for (int T = 0; T < Threads; ++T)
    {
        Workers.emplace_back([&, T] {
            const int Receiver = (T / SlotCount) % Receivers, Slot = T % SlotCount;
            for (int I = 0; I < Attempts; ++I)
            {
                if (!Acquire(Process, Budgets[Receiver], Slot)) { std::this_thread::yield(); continue; }
                ++Acquisitions;
                if (Inside.fetch_add(1) + 1 > MaxInFlight) ++Violations;
                if (SlotInside[Receiver * SlotCount + Slot].fetch_add(1) != 0) ++Violations;
                if (Process.Active.load() < 1 || Process.Active.load() > MaxInFlight) ++Violations;
                std::this_thread::yield();
                --SlotInside[Receiver * SlotCount + Slot]; --Inside;
                if (!Release(Process, Budgets[Receiver], Slot)) ++Violations;
            }
        });
    }
    for (auto& Worker : Workers) Worker.join();
    if (Violations.load() || !Acquisitions.load() || Process.Active.load() || Inside.load()) return 8;
    for (const auto& Budget : Budgets) if (BusyMask(Budget)) return 9;
    std::cout << "PASS: bounded FIFO fairness, process cap, per-slot exclusion and concurrent stress\n";
    return 0;
}
