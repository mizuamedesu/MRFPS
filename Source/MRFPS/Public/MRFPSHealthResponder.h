#pragma once

#include <cstddef>
#include <cstdint>

// The listener is local to the Linux network namespace. Its only inputs are
// bounded nonces; the caller supplies live admission and NetDriver state on
// the game thread. All operations are non-blocking.
class FMRFPSHealthResponder
{
public:
    FMRFPSHealthResponder() = default;
    ~FMRFPSHealthResponder();
    FMRFPSHealthResponder(const FMRFPSHealthResponder&) = delete;
    FMRFPSHealthResponder& operator=(const FMRFPSHealthResponder&) = delete;

    bool Start();
    void Tick(bool bAdmissionAndDriverReady, int Port, std::uint64_t NowMilliseconds);
    void Stop();

private:
    static constexpr std::size_t RequestBytes = 21;
    static constexpr std::size_t ResponseBytes = 24;
    static constexpr std::size_t MaxClients = 8;
    struct FClient {
        int Descriptor = -1;
        std::uint64_t Deadline = 0;
        std::uint8_t Request[RequestBytes] = {};
        std::uint8_t Response[ResponseBytes] = {};
        std::size_t Received = 0;
        std::size_t Sent = 0;
        bool bResponsePrepared = false;
    };
    int Listener = -1;
    FClient Clients[MaxClients] = {};
};
