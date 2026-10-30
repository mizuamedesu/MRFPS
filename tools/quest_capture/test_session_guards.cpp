#include "../../Plugins/MRFPSXR/Source/MRFPSXR/Private/MRFPSCaptureSessionGuards.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <thread>

struct FMutex
{
    std::mutex Mutex;
    bool TryLock() { return Mutex.try_lock(); }
    void Unlock() { Mutex.unlock(); }
};

int main()
{
    using namespace MRFPSCaptureSessionGuards;
    FSessionState State;
    State.Start(1);
    if (State.IsReady(1) || State.IsFailed(1)) return 1; // UI request is not yet a timeout
    State.MarkReady(1);
    if (!State.IsReady(1) || !State.PublishFrame(1, 100) || !State.Fresh(1, 101, 2)) return 2;
    if (State.Fresh(1, 103, 2) || State.Fresh(1, 99, 2)) return 3;

    FMutex Mutex;
    std::atomic<bool> Locked{false}, Release{false};
    std::thread Stalled([&] {
        Mutex.Mutex.lock();
        Locked.store(true, std::memory_order_release);
        while (!Release.load(std::memory_order_acquire)) std::this_thread::yield();
        Mutex.Mutex.unlock();
    });
    while (!Locked.load(std::memory_order_acquire)) std::this_thread::yield();
    {
        // The game/UI path must fail immediately instead of joining the stuck JNI worker.
        TTryLock<FMutex> Guard(Mutex);
        if (Guard || !State.Retire(1) || State.Fresh(1, 101, 2)) { Release = true; Stalled.join(); return 4; }
    }
    Release.store(true, std::memory_order_release);
    Stalled.join();
    {
        TTryLock<FMutex> Guard(Mutex);
        if (!Guard) return 5;
        State.Start(2); // production starts a generation only while holding the mailbox
    }
    State.MarkFailed(1); // old callback after a new session started
    State.MarkReady(1);
    if (State.Retire(1) || State.IsFailed(2) || State.IsReady(2) || State.PublishFrame(1, 200)) return 6;
    if (!State.PublishFrame(2, 200) || !State.Fresh(2, 201, 2)) return 7;
    State.MarkFailed(2);
    if (!State.IsFailed(2) || State.Fresh(2, 201, 2) || State.PublishFrame(2, 201)) return 8;
    if (!State.Retire(2)) return 9;
    State.Start(3);
    State.MarkFailed(2);
    if (State.IsFailed(3) || !State.PublishFrame(3, 300) || !State.Fresh(3, 300, 2)) return 10;
    if (!State.Retire(3)) return 11;
    State.Start(4); // previous OS consent is still open; the new Java request reports Busy
    FMutex BusyMutex;
    std::atomic<bool> BusyLocked{false}, BusyRelease{false};
    std::thread BusyWorker([&] {
        BusyMutex.Mutex.lock();
        BusyLocked.store(true, std::memory_order_release);
        while (!BusyRelease.load(std::memory_order_acquire)) std::this_thread::yield();
        BusyMutex.Mutex.unlock();
    });
    while (!BusyLocked.load(std::memory_order_acquire)) std::this_thread::yield();
    State.MarkBusy(4); // nativeOnState records this before its contested mailbox try-lock
    {
        TTryLock<FMutex> Guard(BusyMutex);
        if (Guard || !State.IsBusy(4) || State.IsFailed(4) ||
            State.DecideBusy(4, true, true) != EBusyDecision::Waiting ||
            State.DecideBusy(4, false, false) != EBusyDecision::Waiting) {
            BusyRelease = true; BusyWorker.join(); return 12;
        }
    }
    // The old OS result is drained while the mailbox is still unavailable.
    if (State.DecideBusy(4, true, false) != EBusyDecision::Retry || !State.IsFailed(4)) {
        BusyRelease = true; BusyWorker.join(); return 13;
    }
    BusyRelease.store(true, std::memory_order_release);
    BusyWorker.join();
    if (!State.Retire(4)) return 14;
    State.Start(5);
    State.MarkBusy(4); // delayed Busy from the rejected old generation
    if (State.DecideBusy(4, true, false) != EBusyDecision::NotBusy ||
        State.DecideBusy(5, true, false) != EBusyDecision::NotBusy ||
        State.IsFailed(5) || !State.PublishFrame(5, 500) || !State.Fresh(5, 500, 2)) return 15;
    std::cout << "PASS capture session guards: mutex, Busy consent, generation, failure and freshness\n";
    return 0;
}
