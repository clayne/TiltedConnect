#include "Server.hpp"
#include "SteamInterface.hpp"
#include <thread>
#include <algorithm>
#include <TiltedCore/Buffer.hpp>
#include <TiltedCore/StackAllocator.hpp>

#include <cassert>
#include <Packet.hpp>
#include <bit>
#include <google/protobuf/stubs/port.h>
#include <snappy.h>

using namespace std::chrono;

namespace TiltedPhoques
{
    static thread_local Server* s_pServer = nullptr;

    Server::Server() noexcept
        : m_tickRate(10)
        , m_lastUpdateTime(0ns)
        , m_timeBetweenUpdates(100ms)
        , m_lastClockSyncTime(0ns)
    {
        SteamInterface::Acquire();
        m_pInterface = SteamNetworkingSockets();
        m_listenSock = k_HSteamListenSocket_Invalid;
        m_pollGroup = k_HSteamNetPollGroup_Invalid;
    }

    Server::~Server()
    {
        SteamInterface::Release();
    }

    bool Server::Host(const uint16_t aPort, uint32_t aTickRate, bool bEnableDualStackIP) noexcept
    {
        Close();

        SteamNetworkingIPAddr localAddress{};  // NOLINT(cppcoreguidelines-pro-type-member-init)
        if (bEnableDualStackIP)
        {
            localAddress.Clear();
            localAddress.m_port = aPort;
        }
        else
        {
            localAddress.SetIPv4(0, aPort);
        }

        SteamNetworkingConfigValue_t opt = {};
        opt.SetPtr(k_ESteamNetworkingConfig_Callback_ConnectionStatusChanged, reinterpret_cast<void*>(&SteamNetConnectionStatusChangedCallback));
        m_listenSock = m_pInterface->CreateListenSocketIP(localAddress, 1, &opt);

        m_pollGroup = m_pInterface->CreatePollGroup();

        if (m_tickRate == 0 && aTickRate == 0)
        {
            aTickRate = 10;
        }
        // If we pass 0, reuse the previously used tick rate
        else if (aTickRate == 0)
        {
            aTickRate = m_tickRate;
        }

        m_tickRate = aTickRate;

        // update time in MS
        m_timeBetweenUpdates = 1000ms / m_tickRate;
        return IsListening();
    }

    void Server::Close() noexcept
    {
        m_pInterface->DestroyPollGroup(m_pollGroup);
        
        if (IsListening())
        {
            m_pInterface->CloseListenSocket(m_listenSock);
        }

        m_pollGroup = k_HSteamNetPollGroup_Invalid;
        m_listenSock = k_HSteamListenSocket_Invalid;
    }

    void Server::Update() noexcept
    {
        m_currentTick = high_resolution_clock::now();

        if (IsListening())
        {
            s_pServer = this;
            m_pInterface->RunCallbacks();
            s_pServer = nullptr;

            while (true)
            {
                ISteamNetworkingMessage* pIncomingMessage = nullptr;
                const auto messageCount = m_pInterface->ReceiveMessagesOnPollGroup(m_pollGroup, &pIncomingMessage, 1);
                if (messageCount <= 0 || pIncomingMessage == nullptr)
                {
                    break;
                    // TODO: Handle when messageCount is a negative number, it's an error
                }

                HandleMessage(pIncomingMessage->GetData(), pIncomingMessage->GetSize(), pIncomingMessage->GetConnection());

                pIncomingMessage->Release();
            }
        }

        // Sync clocks every 10 seconds
        if (m_currentTick - m_lastClockSyncTime >= 10s)
        {
            m_lastClockSyncTime = m_currentTick;
            SynchronizeClientClocks();
        }

        if (m_currentTick - m_lastUpdateTime >= m_timeBetweenUpdates)
        {
            m_lastUpdateTime = m_currentTick;
            OnUpdate();
        }

        std::this_thread::sleep_for(2ms);
    }

    void Server::SendToAll(Packet* apPacket, const EPacketFlags aPacketFlags) noexcept
    {
        for (const auto conn : m_connections)
        {
            Send(conn, apPacket, aPacketFlags);
        }
    }

    void Server::Send(const ConnectionId_t aConnectionId, Packet* apPacket, EPacketFlags aPacketFlags) const noexcept
    {
        if (apPacket->m_pData[0] == kPayload)
        {
            std::string data;
            snappy::Compress(apPacket->GetData(), apPacket->GetSize(), &data);

            if (data.size() < apPacket->GetSize())
            {
                apPacket->m_pData[0] = kCompressedPayload;
                std::copy(std::begin(data), std::end(data), apPacket->GetData());
                apPacket->m_size = (data.size() + 1) & 0xFFFFFFFF;
            }
        }

        m_pInterface->SendMessageToConnection(aConnectionId, apPacket->m_pData, apPacket->m_size,
            aPacketFlags == kReliable ? k_nSteamNetworkingSend_Reliable : k_nSteamNetworkingSend_Unreliable, nullptr);
    }

    void Server::Kick(const ConnectionId_t aConnectionId) noexcept
    {
        m_pInterface->CloseConnection(aConnectionId, 0, "Kick", true);
        Remove(aConnectionId);

        OnDisconnection(aConnectionId, EDisconnectReason::Kicked);
    }

    uint16_t Server::GetPort() const noexcept
    {
        SteamNetworkingIPAddr address{};
        if (m_pInterface->GetListenSocketAddress(m_listenSock, &address))
        {
            return address.m_port;
        }

        return 0;
    }

    bool Server::IsListening() const noexcept
    {
        return m_listenSock != k_HSteamListenSocket_Invalid;
    }

    uint32_t Server::GetClientCount() const noexcept
    {
        return m_connections.size() & 0xFFFFFFFF;
    }

    uint32_t Server::GetTickRate() const noexcept
    {
        return m_tickRate;
    }

	uint64_t Server::GetTick() const noexcept
	{
		return std::chrono::duration_cast<std::chrono::milliseconds>(m_currentTick.time_since_epoch()).count();
	}

    SteamNetConnectionInfo_t Server::GetConnectionInfo(ConnectionId_t aConnectionId) const noexcept
    {
        SteamNetConnectionInfo_t info{};

        if (aConnectionId != k_HSteamNetConnection_Invalid)
        {
            m_pInterface->GetConnectionInfo(aConnectionId, &info);
        }

        return info;
    }

    bool Server::IsAlive(ConnectionId_t aConnectionId) const noexcept
    {
		const auto it = std::find(std::begin(m_connections), std::end(m_connections), aConnectionId);
        return it != std::end(m_connections);
    }

    void Server::Remove(const ConnectionId_t aId) noexcept
    {
        const auto it = std::find(std::begin(m_connections), std::end(m_connections), aId);
        if (it != std::end(m_connections) && !m_connections.empty())
        {
            std::iter_swap(it, std::end(m_connections) - 1);
            m_connections.pop_back();
        }
    }

    void Server::HandleMessage(const void* apData, const uint32_t aSize, const ConnectionId_t aConnectionId) noexcept
    {
        // We handle the cases where packets target the current stack or the user stack
        if (aSize == 0)
            return;

        const auto pData = static_cast<const uint8_t*>(apData);
        switch (pData[0])
        {
        case kPayload:
            OnConsume(pData + 1, aSize - 1, aConnectionId);
            break;
        case kCompressedPayload:
            HandleCompressedPayload(pData + 1, aSize - 1, aConnectionId);
            break;
        default:
            assert(false);
            break;
        }
    }

    void Server::HandleCompressedPayload(const void* apData, uint32_t aSize, ConnectionId_t aConnectionId) noexcept
    {
        std::string data;
        snappy::Uncompress((const char*)apData, aSize, &data);

        if (!data.empty())
        {
            OnConsume(data.data(), data.size() & 0xFFFFFFFF, aConnectionId);
        }
    }

    void Server::SynchronizeClientClocks(const ConnectionId_t aSpecificConnection) noexcept
    {
        const auto time = GetTick();

        StackAllocator<1 << 10> allocator;
        ScopedAllocator _{ &allocator };

        const auto pBuffer = New<Buffer>(512);

        Buffer::Writer writer(pBuffer);
        writer.WriteBits(kServerTime, 8);
        writer.WriteBits(google::protobuf::BigEndian::FromHost64(time), 64);

        if(aSpecificConnection != k_HSteamNetConnection_Invalid)
        {
            // In this case we probably want it to arrive so send it reliably
            m_pInterface->SendMessageToConnection(aSpecificConnection, pBuffer->GetData(), writer.Size() & 0xFFFFFFFF, k_nSteamNetworkingSend_ReliableNoNagle, nullptr);
        }
        else
        {
            for (const auto cConnection : m_connections)
            {
                m_pInterface->SendMessageToConnection(cConnection, pBuffer->GetData(), writer.Size() & 0xFFFFFFFF, k_nSteamNetworkingSend_UnreliableNoDelay, nullptr);
            }
        }
    }

    void Server::SteamNetConnectionStatusChangedCallback(SteamNetConnectionStatusChangedCallback_t* apInfo)
    {
        if (!apInfo || apInfo->m_hConn == k_HSteamNetConnection_Invalid) [[unlikely]]
            return;

        if (s_pServer)
        {
            s_pServer->OnSteamNetConnectionStatusChanged(apInfo);
        }
    }

    void Server::OnSteamNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* apInfo)
    {
        switch (apInfo->m_info.m_eState)
        {
        case k_ESteamNetworkingConnectionState_None:
            break;
        case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
        case k_ESteamNetworkingConnectionState_ClosedByPeer:
        {
            if (apInfo->m_eOldState == k_ESteamNetworkingConnectionState_Connected)
            {
                Remove(apInfo->m_hConn);

                const auto reason = apInfo->m_info.m_eState == k_ESteamNetworkingConnectionState_ClosedByPeer
                        ? EDisconnectReason::Quit
                        : EDisconnectReason::BadConnection;

                OnDisconnection(apInfo->m_hConn, reason);
            }

            m_pInterface->CloseConnection(apInfo->m_hConn, 0, nullptr, false);
            break;
        }
        case k_ESteamNetworkingConnectionState_Connecting:
        {
            if (m_pInterface->AcceptConnection(apInfo->m_hConn) != k_EResultOK)
            {
                m_pInterface->CloseConnection(apInfo->m_hConn, 0, nullptr, false);
                // TODO: Error handling
                break;
            }

            if(m_pInterface->SetConnectionPollGroup(apInfo->m_hConn, m_pollGroup) != k_EResultOK)
            {
                m_pInterface->CloseConnection(apInfo->m_hConn, 0, nullptr, false);
                // TODO: Error handling
                break;
            }

            m_connections.push_back(apInfo->m_hConn);

            SynchronizeClientClocks(apInfo->m_hConn);

            OnConnection(apInfo->m_hConn);
            break;
        }
        case k_ESteamNetworkingConnectionState_Connected:
            break;
        default:
            break;

        }
    }
}
