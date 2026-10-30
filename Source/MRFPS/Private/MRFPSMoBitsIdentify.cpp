#include "MRFPSMoBitsIdentify.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"

#if PLATFORM_ANDROID
#include <dlfcn.h>
#include "mobits_connect.h"
#include "mobits_connect_sender.h"
#endif

void MRFPSMoBitsIdentify::Start(const FString& DeviceId, FRequest Request, TFunction<void(bool, int32)> Completion)
{
    Async(EAsyncExecution::ThreadPool, [DeviceId, Request, Completion=MoveTemp(Completion)]() mutable {
        bool Sent=false;
        int32 Result=-1;
#if PLATFORM_ANDROID
        // Reuse the library loaded by the Quest standalone component. These
        // entry points send LRA only; no EMS session or output is enabled.
        void* Library=dlopen("libmobits_connect.so", RTLD_NOW | RTLD_NOLOAD);
        if (Library) {
            const auto Acquire=reinterpret_cast<decltype(&mobits_connect_device_acquire_by_addr)>(dlsym(Library,"mobits_connect_device_acquire_by_addr"));
            const auto Release=reinterpret_cast<decltype(&mobits_connect_device_release)>(dlsym(Library,"mobits_connect_device_release"));
            const auto Snapshot=reinterpret_cast<decltype(&mobits_connect_haptics_runtime_snapshot)>(dlsym(Library,"mobits_connect_haptics_runtime_snapshot"));
            const auto StartLra=reinterpret_cast<decltype(&mobits_connect_haptics_send_lra_start)>(dlsym(Library,"mobits_connect_haptics_send_lra_start"));
            const auto Send=reinterpret_cast<decltype(&mobits_connect_haptics_send_evt5_expressive_command)>(dlsym(Library,"mobits_connect_haptics_send_evt5_expressive_command"));
            const auto StopLra=reinterpret_cast<decltype(&mobits_connect_haptics_send_lra_stop)>(dlsym(Library,"mobits_connect_haptics_send_lra_stop"));
            if (Acquire && Release && Snapshot && StartLra && Send && StopLra) {
                FTCHARToUTF8 Address(*DeviceId);
                for (int32 Attempt=0; Attempt<4 && !Request->Cancelled.load(); ++Attempt) {
                    auto* Device=Acquire(Address.Get());
                    mobits_haptics_runtime_snapshot_t State{};
                    const bool Ready=Device && Device->is_connected && Device->model_number==50 &&
                        Device->connect_type==MOBITS_CONNECT_BLE && Snapshot(Device,&State) && State.lra_available;
                    if (Ready) {
                        Result=static_cast<int32>(StartLra(Device));
                        if (Result==MOBITS_SUCCESS && !Request->Cancelled.load()) {
                            mobits_evt5_expressive_command_t Command{};
                            Command.flags=2; // LRA only; EMS fields remain zero.
                            Command.lra.duration_ms=400;
                            Command.lra.attack_ms=30;
                            Command.lra.release_ms=30;
                            Command.lra.intensity=40;
                            Result=static_cast<int32>(Send(Device,&Command));
                            Sent=Result==MOBITS_SUCCESS;
                            for (int32 Tick=0; Sent && Tick<20 && !Request->Cancelled.load(); ++Tick)
                                FPlatformProcess::Sleep(.02f);
                        }
                        // Always try to turn off LRA after a start attempt.
                        StopLra(Device);
                        Release(Device);
                        break;
                    }
                    if (Device) Release(Device);
                    Result=MOBITS_ERR_UNAVAILABLE;
                    for (int32 Tick=0; Tick<20 && !Request->Cancelled.load(); ++Tick)
                        FPlatformProcess::Sleep(.025f);
                }
            }
            dlclose(Library);
        }
#endif
        if (Request->Cancelled.load()) Sent=false;
        AsyncTask(ENamedThreads::GameThread, [Completion=MoveTemp(Completion), Sent, Result]() mutable {
            Completion(Sent,Result);
        });
    });
}
