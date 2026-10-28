#include "MoBitsClient.h"

// Bounded game-thread notification delivery and detached pending completions.
void FMoBitsClient::Emit(FEvent&& Event)
{
    Event.Generation = Generation;
    // No unbounded network or Blueprint event backlog.
    if (Events.Num() >= 64)
    {
        CloseTransports(); Pending.Reset(); State = EMoBitsConnectionState::Faulted; ScheduleRetry(); Events.Reset();
        FEvent Error; Error.Generation = Generation; Error.Kind = EEventKind::Error;
        Error.Error = TEXT("MoBits event consumer fell behind; the session was closed."); Events.Add(MoveTemp(Error));
        FEvent Changed; Changed.Generation = Generation; Changed.Kind = EEventKind::State; Changed.State = State; Events.Add(MoveTemp(Changed));
        return;
    }
    Events.Add(MoveTemp(Event));
}
void FMoBitsClient::Report(const FString& Error)
{
    if (!Events.IsEmpty() && Events.Last().Kind == EEventKind::Error) { Events.Last().Error = Error; return; }
    FEvent E; E.Kind = EEventKind::Error; E.Error = Error; Emit(MoveTemp(E));
}
void FMoBitsClient::SetState(EMoBitsConnectionState NewState)
{
    if (State == NewState) return;
    State = NewState; FEvent E; E.Kind = EEventKind::State; E.State = State; Emit(MoveTemp(E));
}
void FMoBitsClient::DiscardTrackingEvents()
{
    Events.RemoveAll([](const FEvent& E) {
        return E.Kind == EEventKind::Motion || E.Kind == EEventKind::Devices ||
            (E.Kind == EEventKind::State && (E.State == EMoBitsConnectionState::Connecting || E.State == EMoBitsConnectionState::Connected));
    });
}
void FMoBitsClient::CompletePending(const FString& Reason)
{
    // Emit can itself close a backlogged transport and clear Pending. Detach
    // the completion batch first so reentrant queue overflow cannot invalidate it.
    auto Ended = MoveTemp(Pending); Pending.Reset();
    TArray<int64> Ids;
    Ended.GetKeys(Ids); Ids.Sort();
    for (int64 Id : Ids)
    {
        FEvent E; E.Kind = EEventKind::Receipt; E.Receipt.Id = Id;
        E.Receipt.Status = Ended[Id].Kind == ECommand::Stop ? TEXT("stop_unconfirmed") : TEXT("rejected");
        E.Receipt.Reason = Reason; Emit(MoveTemp(E));
    }
    Pending.Reset();
}
