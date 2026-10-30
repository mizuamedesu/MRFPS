#include "MRFPSHealthResponder.h"

#if defined(__linux__) && (defined(MRFPS_HEALTH_STANDALONE) || (defined(UE_SERVER) && UE_SERVER))
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#define MRFPS_HEALTH_UNIX 1
#else
#define MRFPS_HEALTH_UNIX 0
#endif

#if MRFPS_HEALTH_UNIX
namespace {
constexpr char SocketName[] = "mrfps-health-v1";
constexpr std::uint8_t Magic[] = {'M', 'R', 'F', 'H', '1'};
constexpr std::uint64_t ClientTimeoutMs = 2000;
}
#endif

FMRFPSHealthResponder::~FMRFPSHealthResponder() { Stop(); }

bool FMRFPSHealthResponder::Start()
{
#if MRFPS_HEALTH_UNIX
    if (Listener >= 0) return true;
    const int Socket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (Socket < 0) return false;
    sockaddr_un Address{};
    Address.sun_family = AF_UNIX;
    std::memcpy(Address.sun_path + 1, SocketName, sizeof(SocketName) - 1);
    const auto Length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + sizeof(SocketName));
    if (::bind(Socket, reinterpret_cast<const sockaddr*>(&Address), Length) != 0 || ::listen(Socket, static_cast<int>(MaxClients)) != 0) {
        ::close(Socket);
        return false;
    }
    Listener = Socket;
    return true;
#else
    return false;
#endif
}

void FMRFPSHealthResponder::Tick(bool bReady, int Port, std::uint64_t NowMilliseconds)
{
#if MRFPS_HEALTH_UNIX
    if (Listener < 0) return;
    auto Reset = [](FClient& Client) {
        if (Client.Descriptor >= 0) ::close(Client.Descriptor);
        Client = FClient{};
    };
    for (FClient& Client : Clients) {
        if (Client.Descriptor >= 0) continue;
        const int Accepted = ::accept(Listener, nullptr, nullptr);
        if (Accepted < 0) break;
        struct FPeerCredentials { pid_t Pid; uid_t Uid; gid_t Gid; } Peer{};
        socklen_t PeerSize = sizeof(Peer);
        if (::getsockopt(Accepted, SOL_SOCKET, SO_PEERCRED, &Peer, &PeerSize) != 0 ||
            PeerSize != sizeof(Peer) || Peer.Pid <= 0 || Peer.Uid != ::geteuid()) {
            ::close(Accepted);
            continue;
        }
        const int Flags = ::fcntl(Accepted, F_GETFL, 0);
        const int DescriptorFlags = ::fcntl(Accepted, F_GETFD, 0);
        if (Flags < 0 || DescriptorFlags < 0 || ::fcntl(Accepted, F_SETFL, Flags | O_NONBLOCK) != 0 ||
            ::fcntl(Accepted, F_SETFD, DescriptorFlags | FD_CLOEXEC) != 0) {
            ::close(Accepted);
            continue;
        }
        Client.Descriptor = Accepted;
        Client.Deadline = NowMilliseconds + ClientTimeoutMs;
    }
    for (FClient& Client : Clients) {
        if (Client.Descriptor < 0) continue;
        if (NowMilliseconds >= Client.Deadline || !bReady || Port < 1 || Port > 65535) {
            Reset(Client);
            continue;
        }
        if (!Client.bResponsePrepared) {
            const auto Count = ::recv(Client.Descriptor, Client.Request + Client.Received,
                                      RequestBytes - Client.Received, MSG_DONTWAIT);
            if (Count == 0 || (Count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
                Reset(Client);
                continue;
            }
            if (Count > 0) Client.Received += static_cast<std::size_t>(Count);
            if (Client.Received != RequestBytes) continue;
            if (std::memcmp(Client.Request, Magic, sizeof(Magic)) != 0) {
                Reset(Client);
                continue;
            }
            std::memcpy(Client.Response, Client.Request, RequestBytes);
            Client.Response[21] = static_cast<std::uint8_t>((Port >> 8) & 0xff);
            Client.Response[22] = static_cast<std::uint8_t>(Port & 0xff);
            Client.Response[23] = 0x03; // verified admission and live NetDriver
            Client.bResponsePrepared = true;
        }
        const auto Count = ::send(Client.Descriptor, Client.Response + Client.Sent,
                                  ResponseBytes - Client.Sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (Count == 0 || (Count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            Reset(Client);
            continue;
        }
        if (Count > 0) Client.Sent += static_cast<std::size_t>(Count);
        if (Client.Sent == ResponseBytes) Reset(Client);
    }
#else
    (void)bReady; (void)Port; (void)NowMilliseconds;
#endif
}

void FMRFPSHealthResponder::Stop()
{
#if MRFPS_HEALTH_UNIX
    for (FClient& Client : Clients) {
        if (Client.Descriptor >= 0) ::close(Client.Descriptor);
        Client = FClient{};
    }
    if (Listener >= 0) ::close(Listener);
#endif
    Listener = -1;
}
