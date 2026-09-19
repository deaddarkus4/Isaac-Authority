#include "snapshot_receiver.hpp"
#include <bcrypt.h>

namespace authority::bridge {
DWORD Receiver::Start(std::uint32_t seed, bool replica) {
    if (worker_ || winsock_) return ERROR_ALREADY_EXISTS;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&session_), sizeof(session_), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return ERROR_GEN_FAILURE;
    if (!session_) session_ = 1;
    seed_ = seed; replica_ = replica; pending_ = false; accepted_ = 0; rejected_ = 0; replaced_ = 0; errors_ = 0;
    WSADATA data{}; const auto initialized = WSAStartup(MAKEWORD(2, 2), &data);
    if (initialized) return static_cast<DWORD>(initialized);
    winsock_ = true; socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (socket_ == INVALID_SOCKET) { const auto e = WSAGetLastError(); Stop(); return static_cast<DWORD>(e); }
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    u_long nonblocking = 1;
    if (bind(socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) || ioctlsocket(socket_, FIONBIO, &nonblocking)) {
        const auto e = WSAGetLastError(); Stop(); return static_cast<DWORD>(e);
    }
    int size = sizeof(address);
    if (getsockname(socket_, reinterpret_cast<sockaddr*>(&address), &size)) {
        const auto e = WSAGetLastError(); Stop(); return static_cast<DWORD>(e);
    }
    port_ = ntohs(address.sin_port); deadline_ = GetTickCount64() + (replica_ ? kReplicaSessionMs : kSessionMs);
    running_ = true; worker_ = CreateThread(nullptr, 0, &Run, this, 0, nullptr);
    if (!worker_) { const auto e = GetLastError(); Stop(); return e; }
    return ERROR_SUCCESS;
}
void Receiver::Stop() {
    running_ = false;
    // Worker does only bounded nonblocking receives, short mailbox copies and Sleep(1).
    if (worker_) { WaitForSingleObject(worker_, INFINITE); CloseHandle(worker_); worker_ = nullptr; }
    if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
    if (winsock_) { WSACleanup(); winsock_ = false; }
}
bool Receiver::Take(State& state) {
    if (!TryAcquireSRWLockExclusive(&lock_)) return false;
    const bool available = pending_;
    if (available) { state = latest_; pending_ = false; }
    ReleaseSRWLockExclusive(&lock_);
    return available;
}
DWORD WINAPI Receiver::Run(void* context) {
    auto& self = *static_cast<Receiver*>(context);
    Gate gate(self.session_, self.seed_, self.replica_); unsigned short peer = 0;
    while (self.running_.load() && GetTickCount64() <= self.deadline_) {
        for (unsigned drain = 0; drain < 64 && self.running_.load(); ++drain) {
            std::uint8_t bytes[kBytes + 1]{}; sockaddr_in from{}; int size = sizeof(from);
            const auto n = recvfrom(self.socket_, reinterpret_cast<char*>(bytes), sizeof(bytes), 0,
                reinterpret_cast<sockaddr*>(&from), &size);
            if (n == SOCKET_ERROR) {
                const auto e = WSAGetLastError();
                if (e == WSAEWOULDBLOCK) break;
                if (e == WSAEMSGSIZE || e == WSAECONNRESET) { ++self.rejected_; continue; }
                ++self.errors_; self.running_ = false; break;
            }
            State state;
            if (from.sin_family != AF_INET || from.sin_addr.s_addr != htonl(INADDR_LOOPBACK) ||
                (peer && peer != from.sin_port) || !Decode(bytes, static_cast<std::size_t>(n), state) ||
                !gate.Accept(state, GetTickCount64())) { ++self.rejected_; continue; }
            peer = from.sin_port;
            AcquireSRWLockExclusive(&self.lock_);
            if (self.pending_) ++self.replaced_;
            self.latest_ = state; self.pending_ = true;
            ReleaseSRWLockExclusive(&self.lock_);
            ++self.accepted_;
        }
        Sleep(1);
    }
    return 0;
}
}
