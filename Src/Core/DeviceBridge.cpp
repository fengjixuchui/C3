#include "StdAfx.h"
#include "DeviceBridge.h"
#include "Relay.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::C3::Core::DeviceBridge::DeviceBridge(std::shared_ptr<Relay>&& relay, DeviceId did, HashT typeNameHash, std::shared_ptr<Device>&& device, bool isNegotiationChannel, bool isSlave, ByteVector args /*= ByteVector()*/)
	: m_Relay{ relay }
	, m_Device{ std::move(device) }
	, m_Did{ did }
	, m_TypeNameHash(typeNameHash)
	, m_IsNegotiationChannel(isNegotiationChannel)
	, m_IsSlave(isSlave)
{
	if (!isNegotiationChannel)
		return;

	auto readView = ByteView{ args };
	std::tie(m_InputId, m_OutpuId) = readView.Read<ByteVector, ByteVector>();
	m_NonNegotiatiedArguments = readView;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::OnAttach()
{
	GetDevice()->OnAttach(shared_from_this());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::Detach()
{
	m_IsAlive = false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::Close()
{
	auto relay = GetRelay();
	relay->DetachDevice(GetDid());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::OnReceive()
{
	GetDevice()->OnReceive();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::PassNetworkPacket(ByteView packet)
{
	if (m_IsNegotiationChannel && !m_IsSlave) // negotiation channel does not support chunking. Just pass packet and leave.
		return GetRelay()->OnPacketReceived(packet, shared_from_this());

	m_QoS.PushReceivedChunk(packet);
	auto nextPacket = m_QoS.GetNextPacket();
	if (!nextPacket.empty())
		GetRelay()->OnPacketReceived(nextPacket, shared_from_this());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::OnPassNetworkPacket(ByteView packet)
{
	auto lock = std::lock_guard<std::mutex>{ m_ProtectWriteInConcurrentThreads };

	if (m_IsNegotiationChannel) // negotiation channel does not support chunking. Just pass packet and leave.
	{
		auto sent = GetDevice()->OnSendToChannelInternal(packet);
		if (sent != packet.size())
			throw std::runtime_error{OBF("Negotiation channel does not support chunking. Packet size: ") + std::to_string(packet.size()) + OBF(" Channel sent: ") + std::to_string(sent)};

		return;
	}

	auto oryginalSize = static_cast<uint32_t>(packet.size());
	auto messageId = m_QoS.GetOutgouingPacketId();
	uint32_t chunkId = 0u;
	while (!packet.empty())
	{
		auto data = ByteVector{}.Write(messageId, chunkId, oryginalSize).Concat(packet);
		auto sent = GetDevice()->OnSendToChannelInternal(data);

		if (sent >= QualityOfService::s_MinFrameSize || sent == data.size()) // if this condition were not channel must resend data.
		{
			chunkId++;
			packet.remove_prefix(sent - QualityOfService::s_HeaderSize);
		}
	}

}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::PostCommandToConnector(ByteView packet)
{
	GetRelay()->PostCommandToConnector(packet, shared_from_this());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::OnCommandFromConnector(ByteView command)
{
	auto lock = std::lock_guard<std::mutex>{ m_ProtectWriteInConcurrentThreads };
	GetDevice()->OnCommandFromConnector(command);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::ByteVector MWR::C3::Core::DeviceBridge::RunCommand(ByteView command)
{
	return GetDevice()->OnRunCommand(command);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::ByteVector MWR::C3::Core::DeviceBridge::WhoAreYou()
{
	return GetDevice()->OnWhoAmI();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::Log(LogMessage const& message)
{
	GetRelay()->Log(message, GetDid());
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::C3::DeviceId MWR::C3::Core::DeviceBridge::GetDid() const
{
	return m_Did;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::StartUpdatingInSeparateThread()
{
	std::thread{
		[this, self = shared_from_this()]()
		{
			WinTools::StructuredExceptionHandling::SehWrapper([&]()
			{
				while (m_IsAlive)
					try
					{
						std::this_thread::sleep_for(GetDevice()->GetUpdateDelay());
						OnReceive();
					}
					catch (std::exception const& exception)
					{
						Log({ OBF_SEC("std::exception while updating: ") + exception.what(), LogMessage::Severity::Error });
					}
					catch (...)
					{
						Log({ OBF_SEC("Unknown exception while updating."), LogMessage::Severity::Error });
					}
			}, [this]()
			{
#				if defined _DEBUG
						Log({ "Signal captured, ending thread execution.", LogMessage::Severity::Error });
#				endif
			});
		}}.detach();
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::SetUpdateDelay(std::chrono::milliseconds minUpdateDelayInMs, std::chrono::milliseconds maxUpdateDelayInMs)
{
	GetDevice()->SetUpdateDelay(minUpdateDelayInMs, maxUpdateDelayInMs);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::SetUpdateDelay(std::chrono::milliseconds frequencyInMs)
{
	GetDevice()->SetUpdateDelay(frequencyInMs);
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<MWR::C3::Device> MWR::C3::Core::DeviceBridge::GetDevice() const
{
	return m_Device;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::shared_ptr<MWR::C3::Core::Relay> MWR::C3::Core::DeviceBridge::GetRelay() const
{
	return m_Relay;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
MWR::HashT MWR::C3::Core::DeviceBridge::GetTypeNameHash() const
{
	return m_TypeNameHash;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool MWR::C3::Core::DeviceBridge::IsChannel() const
{
	auto device = GetDevice();

	return device ? device->IsChannel() : false;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool MWR::C3::Core::DeviceBridge::IsNegotiationChannel() const
{
	return m_IsNegotiationChannel && IsChannel();
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void MWR::C3::Core::DeviceBridge::SetErrorStatus(std::string_view errorMessage)
{
	m_Error = errorMessage;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
std::string MWR::C3::Core::DeviceBridge::GetErrorStatus()
{
	return m_Error;
}
