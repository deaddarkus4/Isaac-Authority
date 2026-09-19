#include <winsock2.h>
#include <ws2tcpip.h>
#include "authority.hpp"
#include "wire.hpp"
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using namespace authority;
using Clock = std::chrono::steady_clock;
namespace {
class Socket {
public:
    explicit Socket(unsigned short port) {
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) throw std::runtime_error("WSAStartup failed");
        value = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (value == INVALID_SOCKET) { WSACleanup(); throw std::runtime_error("socket failed"); }
        sockaddr_in address{}; address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK); address.sin_port = htons(port);
        u_long nonblocking = 1;
        if (bind(value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
            ioctlsocket(value, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            closesocket(value); WSACleanup(); throw std::runtime_error("bind/nonblocking failed");
        }
    }
    ~Socket() { closesocket(value); WSACleanup(); }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    unsigned short Port() const {
        sockaddr_in address{}; int size = sizeof(address);
        if (getsockname(value, reinterpret_cast<sockaddr*>(&address), &size))
            throw std::runtime_error("getsockname failed");
        return ntohs(address.sin_port);
    }
    void Send(const Packet& p, const sockaddr_in& peer) {
        const auto b = Encode(p);
        if (sendto(value, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0,
            reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) == SOCKET_ERROR &&
            WSAGetLastError() != WSAEWOULDBLOCK) throw std::runtime_error("sendto failed");
    }
    // 0 = empty, 1 = valid, 2 = rejected. Bounded caller drain avoids packet-flood starvation.
    int Receive(std::uint64_t session, Packet& p, sockaddr_in& from) {
        std::uint8_t bytes[2048]; int size = sizeof(from);
        const auto n = recvfrom(value, reinterpret_cast<char*>(bytes), sizeof(bytes), 0,
            reinterpret_cast<sockaddr*>(&from), &size);
        if (n == SOCKET_ERROR) {
            const auto error = WSAGetLastError();
            if (error == WSAEWOULDBLOCK) return 0;
            if (error == WSAEMSGSIZE || error == WSAECONNRESET) return 2;
            throw std::runtime_error("recvfrom failed");
        }
        if (from.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) return 2;
        return Decode(bytes, static_cast<std::size_t>(n), session, p) ? 1 : 2;
    }
private:
    SOCKET value = INVALID_SOCKET;
};
bool SamePeer(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}
Packet StatePacket(std::uint64_t session, const Host& h) {
    Packet p; p.kind = Kind::Snapshot; p.session = session;
    p.epoch = h.State().epoch; p.snapshot = h.State(); return p;
}
void PrintBody(const Body& b) {
    std::cout << "[" << b.position.x << ',' << b.position.y << ',' << b.velocity.x << ',' << b.velocity.y << ']';
}
int RunHost(unsigned short port, std::uint64_t session) {
    Socket socket(port); Host h; sockaddr_in peer{}; bool connected = false;
    unsigned received = 0, rejected = 0, withheld = 0;
    std::cout << "{\"port\":" << socket.Port() << "}" << std::endl;
    const auto start = Clock::now();
    for (unsigned tick = 0; tick < 400; ++tick) {
        for (unsigned drain = 0; drain < 128; ++drain) {
            Packet p; sockaddr_in from{}; const auto result = socket.Receive(session, p, from);
            if (!result) break;
            if (result == 2) { ++rejected; continue; }
            if (!connected && p.kind == Kind::Hello) { peer = from; connected = true; }
            if (!connected || !SamePeer(peer, from)) { ++rejected; continue; }
            if (p.kind == Kind::Input) {
                if (h.Receive(p.epoch, p.input)) ++received; else ++rejected;
            } else if (p.kind == Kind::Hello) socket.Send(StatePacket(session, h), peer);
            else ++rejected;
        }
        h.Tick({0.05f, 0});
        if (connected) {
            if (tick % 11 == 0 && tick != 399) ++withheld;
            else socket.Send(StatePacket(session, h), peer);
        }
        std::this_thread::sleep_until(start + std::chrono::milliseconds((tick + 1) * 10));
    }
    if (connected) for (unsigned i = 0; i < 3; ++i) socket.Send(StatePacket(session, h), peer);
    std::cout.precision(9);
    std::cout << "{\"role\":\"host\",\"ticks\":" << h.State().tick << ",\"ack\":"
        << h.State().acknowledged << ",\"receivedInputs\":" << received << ",\"rejected\":" << rejected
        << ",\"withheldSnapshots\":" << withheld << ",\"remote\":";
    PrintBody(h.State().players[1]);
    std::cout << ",\"elapsedMs\":" << std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()
        << "}\n";
    return connected && received ? 0 : 1;
}
int RunClient(unsigned short port, std::uint64_t session) {
    Socket socket(0); Client c; sockaddr_in server{}; server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_LOOPBACK); server.sin_port = htons(port);
    unsigned dropped = 0, snapshots = 0, paused = 0;
    const auto start = Clock::now();
    for (unsigned tick = 0; tick < 440; ++tick) {
        const bool pause = tick >= 60 && tick < 140;
        if (pause) ++paused;
        else {
            for (unsigned drain = 0; drain < 128; ++drain) {
                Packet p; sockaddr_in from{}; const auto result = socket.Receive(session, p, from);
                if (!result) break;
                if (result == 1 && SamePeer(server, from) && p.kind == Kind::Snapshot && c.Reconcile(p.snapshot))
                    ++snapshots;
            }
            if (!c.Ready()) {
                Packet hello; hello.kind = Kind::Hello; hello.session = session; socket.Send(hello, server);
            } else {
                if (tick < 220) {
                    Input generated; c.Predict({tick < 170 ? 0.5f : -0.5f, -0.1f}, generated);
                }
                unsigned count = 0;
                for (const auto& in : c.PendingInputs()) {
                    if (++count > 8) break;
                    if ((in.sequence + tick) % 7 == 0) { ++dropped; continue; }
                    Packet p; p.session = session; p.epoch = c.Epoch(); p.input = in;
                    socket.Send(p, server);
                }
            }
        }
        std::this_thread::sleep_until(start + std::chrono::milliseconds((tick + 1) * 10));
    }
    std::cout.precision(9);
    std::cout << "{\"role\":\"client\",\"hostTick\":" << c.Authoritative().tick
        << ",\"pending\":" << c.Pending() << ",\"snapshots\":" << snapshots
        << ",\"pausedTicks\":" << paused << ",\"droppedInputs\":" << dropped
        << ",\"corrections\":" << c.Corrections() << ",\"predicted\":";
    PrintBody(c.Predicted()); std::cout << "}\n";
    return c.Ready() && !c.Pending() && c.Authoritative().tick == 400 ? 0 : 1;
}
int RunObserver(unsigned short port, std::uint64_t session) {
    Socket socket(port); Client mirror; sockaddr_in peer{}; bool pinned = false;
    unsigned accepted = 0, rejected = 0;
    std::cout << "{\"port\":" << socket.Port() << "}" << std::endl;
    const auto start = Clock::now(); auto last = start;
    while (Clock::now() - start < std::chrono::seconds(15)) {
        for (unsigned drain = 0; drain < 128; ++drain) {
            Packet packet; sockaddr_in from{}; const auto result = socket.Receive(session, packet, from);
            if (!result) break;
            if (result != 1 || packet.kind != Kind::Snapshot ||
                (pinned && !SamePeer(peer, from)) || !mirror.Reconcile(packet.snapshot)) {
                ++rejected; continue;
            }
            peer = from; pinned = true; ++accepted; last = Clock::now();
        }
        if (accepted && Clock::now() - last > std::chrono::seconds(2)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cout.precision(9);
    std::cout << "{\"role\":\"observer\",\"accepted\":" << accepted << ",\"rejected\":" << rejected
        << ",\"lastFrame\":" << mirror.Authoritative().tick << ",\"epoch\":" << mirror.Epoch()
        << ",\"players\":[";
    PrintBody(mirror.Authoritative().players[0]); std::cout << ',';
    PrintBody(mirror.Authoritative().players[1]); std::cout << "]}\n";
    return accepted ? 0 : 1;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 4) throw std::invalid_argument("Usage: IsaacAuthorityLab host|client|observer PORT SESSION_NUMBER (loopback only)");
        const std::string role = argv[1];
        std::size_t used = 0;
        const auto port = std::stoul(argv[2], &used);
        if (used != std::string(argv[2]).size() || port > 65535) throw std::invalid_argument("invalid port");
        const auto session = std::stoull(argv[3], &used);
        if (!session || used != std::string(argv[3]).size()) throw std::invalid_argument("invalid session");
        if (role == "host") return RunHost(static_cast<unsigned short>(port), session);
        if (role == "client" && port) return RunClient(static_cast<unsigned short>(port), session);
        if (role == "observer") return RunObserver(static_cast<unsigned short>(port), session);
        throw std::invalid_argument("invalid role or client port");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 2; }
}
