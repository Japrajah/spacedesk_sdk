#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include "spacedesk_sdk.h"
#include "wire.h"
#include <algorithm>
#include <cstring>
#include <set>
#include <vector>

namespace {
struct DiscoverySocket {
    bool wsa = false;
    SOCKET value = INVALID_SOCKET;
    ~DiscoverySocket() {
        if (value != INVALID_SOCKET) closesocket(value);
        if (wsa) WSACleanup();
    }
};
}
extern "C" sd_result SD_CALL sd_discover(const char* target, uint16_t port,
    uint32_t timeout, sd_host* hosts, uint32_t capacity, uint32_t* out_count) {
    if (!out_count) return SD_INVALID_ARGUMENT;
    *out_count = 0;
    if (!hosts || !capacity || capacity > 256 || !timeout || timeout > 60000) return SD_INVALID_ARGUMENT;
    for (uint32_t i = 0; i < capacity; ++i)
        if (hosts[i].struct_size != sizeof(sd_host)) return SD_INVALID_ARGUMENT;
    try {
        DiscoverySocket socket;
        WSADATA data{};
        if (WSAStartup(MAKEWORD(2, 2), &data)) return SD_IO_ERROR;
        socket.wsa = true;
        socket.value = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket.value == INVALID_SOCKET) return SD_IO_ERROR;
        BOOL broadcast = TRUE;
        if (setsockopt(socket.value, SOL_SOCKET, SO_BROADCAST,
            reinterpret_cast<const char*>(&broadcast), sizeof(broadcast))) return SD_IO_ERROR;
        u_long nonblocking = 1;
        if (ioctlsocket(socket.value, FIONBIO, &nonblocking)) return SD_IO_ERROR;
        std::set<uint32_t> targets;
        in_addr ip{};
        if (target) {
            if (InetPtonA(AF_INET, target, &ip) != 1) return SD_INVALID_ARGUMENT;
            targets.insert(ip.s_addr);
        } else {
            targets.insert(INADDR_BROADCAST);
            ULONG size = 0;
            if (GetAdaptersInfo(nullptr, &size) == ERROR_BUFFER_OVERFLOW) {
                std::vector<uint8_t> buffer(size);
                auto adapters = reinterpret_cast<IP_ADAPTER_INFO*>(buffer.data());
                if (GetAdaptersInfo(adapters, &size) == NO_ERROR) {
                    for (auto adapter = adapters; adapter; adapter = adapter->Next) {
                        for (auto addr = &adapter->IpAddressList; addr; addr = addr->Next) {
                            in_addr mask{};
                            if (InetPtonA(AF_INET, addr->IpAddress.String, &ip) == 1 && ip.s_addr &&
                                InetPtonA(AF_INET, addr->IpMask.String, &mask) == 1 && mask.s_addr &&
                                ntohl(mask.s_addr) < 0xfffffffeu)
                                targets.insert(ip.s_addr | ~mask.s_addr);
                        }
                    }
                }
            }
        }
        const auto query = sd::protocol::bytes(sd::protocol::DiscoveryQuery{});
        bool sent = false;
        for (auto address : targets) {
            sockaddr_in to{}; to.sin_family = AF_INET; to.sin_port = htons(port ? port : 28252);
            to.sin_addr.s_addr = address;
            if (sendto(socket.value, reinterpret_cast<const char*>(query.data()), int(query.size()), 0,
                reinterpret_cast<sockaddr*>(&to), sizeof(to)) == int(query.size())) sent = true;
        }
        if (!sent) return SD_IO_ERROR;
        const auto deadline = GetTickCount64() + timeout;
        while (GetTickCount64() < deadline && *out_count < capacity) {
            fd_set ready; FD_ZERO(&ready); FD_SET(socket.value, &ready);
            const auto now = GetTickCount64();
            const auto left = deadline > now ? deadline - now : 0;
            timeval tv{long(left / 1000), long((left % 1000) * 1000)};
            const int state = select(0, &ready, nullptr, nullptr, &tv);
            if (state == SOCKET_ERROR) return SD_IO_ERROR;
            if (!state) break;
            uint8_t reply_buffer[4096]{};
            sockaddr_in from{}; int size = sizeof(from);
            const int bytes = recvfrom(socket.value, reinterpret_cast<char*>(reply_buffer), sizeof(reply_buffer), 0,
                reinterpret_cast<sockaddr*>(&from), &size);
            if (bytes == SOCKET_ERROR) {
                const auto error = WSAGetLastError();
                if (error == WSAECONNRESET || error == WSAEWOULDBLOCK || error == WSAEMSGSIZE) continue;
                return SD_IO_ERROR;
            }
            if (bytes != sizeof(sd::protocol::LegacyDiscoveryReply) && bytes != sizeof(sd::protocol::DiscoveryReply)) continue;
            sd::protocol::DiscoveryReply reply{};
            std::memcpy(&reply, reply_buffer, size_t(bytes));
            if (target && from.sin_addr.s_addr != *targets.begin()) continue;
            sd_host host{}; host.struct_size = sizeof(host);
            if (!InetNtopA(AF_INET, &from.sin_addr, host.ipv4, sizeof(host.ipv4))) continue;
            host.port = reply.legacy.tcp_port;
            if (!host.port) continue;
            wchar_t name[129]{};
            for (size_t i = 0; i < 128; ++i) name[i] = wchar_t(reply.legacy.hostname[i]);
            if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1,
                host.hostname, sizeof(host.hostname), nullptr, nullptr)) continue;
            if (bytes == sizeof(sd::protocol::DiscoveryReply)) {
                host.server_os = reply.server_os; host.protocol_major = reply.version_major;
                host.protocol_minor = reply.version_minor; host.feature_flags = reply.feature_flags;
            }
            bool duplicate = false;
            for (uint32_t i = 0; i < *out_count; ++i)
                if (!std::strcmp(host.ipv4, hosts[i].ipv4) && host.port == hosts[i].port) duplicate = true;
            if (!duplicate) hosts[(*out_count)++] = host;
        }
        return SD_OK;
    } catch (const std::bad_alloc&) { return SD_OUT_OF_MEMORY; }
      catch (...) { return SD_INTERNAL_ERROR; }
}
