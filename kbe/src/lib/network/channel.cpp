/*
This source file is part of KBEngine
For the latest info, see http://www.kbengine.org/

Copyright (c) 2008-2012 KBEngine.

KBEngine is free software: you can redistribute it and/or modify
it under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

KBEngine is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU Lesser General Public License for more details.
 
You should have received a copy of the GNU Lesser General Public License
along with KBEngine.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "channel.h"
#ifndef CODE_INLINE
#include "channel.inl"
#endif

#include "network/websocket_protocol.h"
#include "network/html5_packet_filter.h"
#include "network/html5_packet_reader.h"
#include "network/bundle.h"
#include "network/packet_reader.h"
#include "network/network_interface.h"
#include "network/tcp_packet_receiver.h"
#include "network/udp_packet_receiver.h"
#include "network/tcp_packet.h"
#include "network/udp_packet.h"
#include "network/message_handler.h"
#include "network/network_stats.h"

namespace KBEngine { 
namespace Network
{

//-------------------------------------------------------------------------------------
void Channel::onReclaimObject()
{
	this->clearState();
}

//-------------------------------------------------------------------------------------
bool Channel::destructorPoolObject()
{
	this->decRef();
	return true;
}

//-------------------------------------------------------------------------------------
Channel::Channel(NetworkInterface & networkInterface,
		const EndPoint * endpoint, Traits traits, ProtocolType pt,
		PacketFilterPtr pFilter, ChannelID id):
	pNetworkInterface_(&networkInterface),
	traits_(traits),
	protocoltype_(pt),
	id_(id),
	inactivityTimerHandle_(),
	inactivityExceptionPeriod_(0),
	lastReceivedTime_(0),
	bundles_(),
	bufferedReceivesIdx_(0),
	pPacketReader_(0),
	isDestroyed_(false),
	numPacketsSent_(0),
	numPacketsReceived_(0),
	numBytesSent_(0),
	numBytesReceived_(0),
	lastTickBytesReceived_(0),
	pFilter_(pFilter),
	pEndPoint_(NULL),
	pPacketReceiver_(NULL),
	isCondemn_(false),
	proxyID_(0),
	strextra_(),
	channelType_(CHANNEL_NORMAL),
	componentID_(UNKNOWN_COMPONENT_TYPE),
	pMsgHandlers_(NULL)
{
	this->incRef();
	this->clearBundle();
	this->endpoint(endpoint);
	
	if(protocoltype_ == PROTOCOL_TCP)
	{
		pPacketReceiver_ = new TCPPacketReceiver(*pEndPoint_, networkInterface);

		// UDP不需要注册描述符
		pNetworkInterface_->dispatcher().registerReadFileDescriptor(*pEndPoint_, pPacketReceiver_);
	}
	else
		pPacketReceiver_ = new UDPPacketReceiver(*pEndPoint_, networkInterface);
	
	startInactivityDetection((traits == INTERNAL) ? g_channelInternalTimeout : g_channelExternalTimeout);
}

//-------------------------------------------------------------------------------------
Channel::Channel():
	pNetworkInterface_(NULL),
	traits_(EXTERNAL),
	protocoltype_(PROTOCOL_TCP),
	id_(0),
	inactivityTimerHandle_(),
	inactivityExceptionPeriod_(0),
	lastReceivedTime_(0),
	bundles_(),
	bufferedReceivesIdx_(0),
	pPacketReader_(0),
	isDestroyed_(false),
	// Stats
	numPacketsSent_(0),
	numPacketsReceived_(0),
	numBytesSent_(0),
	numBytesReceived_(0),
	lastTickBytesReceived_(0),
	pFilter_(NULL),
	pEndPoint_(NULL),
	pPacketReceiver_(NULL),
	isCondemn_(false),
	proxyID_(0),
	strextra_(),
	channelType_(CHANNEL_NORMAL),
	componentID_(UNKNOWN_COMPONENT_TYPE),
	pMsgHandlers_(NULL)
{
	this->incRef();
	this->clearBundle();
	this->endpoint(NULL);
}

//-------------------------------------------------------------------------------------
Channel::~Channel()
{
	//DEBUG_MSG(fmt::format("Channel::~Channel(): {}\n", this->c_str()));
	if(pNetworkInterface_ != NULL && pEndPoint_ != NULL && !isDestroyed_)
	{
		pNetworkInterface_->onChannelGone(this);

		if(protocoltype_ == PROTOCOL_TCP)
		{
			pNetworkInterface_->dispatcher().deregisterReadFileDescriptor(*pEndPoint_);
			pEndPoint_->close();
		}
	}

	this->clearState();
	
	SAFE_RELEASE(pPacketReceiver_);
	SAFE_RELEASE(pEndPoint_);
	SAFE_RELEASE(pPacketReader_);
}

//-------------------------------------------------------------------------------------
Channel * Channel::get(NetworkInterface & networkInterface,
			const Address& addr)
{
	return networkInterface.findChannel(addr);
}

//-------------------------------------------------------------------------------------
Channel * get(NetworkInterface & networkInterface,
		const EndPoint* pSocket)
{
	return networkInterface.findChannel(pSocket->addr());
}

//-------------------------------------------------------------------------------------
void Channel::startInactivityDetection( float period, float checkPeriod )
{
	stopInactivityDetection();

	// 如果周期为负数则不检查
	if(period > 0.f)
	{
		inactivityExceptionPeriod_ = uint64( period * stampsPerSecond() );
		lastReceivedTime_ = timestamp();

		inactivityTimerHandle_ =
			this->dispatcher().addTimer( int( checkPeriod * 1000000 ),
										this, (void *)TIMEOUT_INACTIVITY_CHECK );
	}
}

//-------------------------------------------------------------------------------------
void Channel::stopInactivityDetection()
{
	inactivityTimerHandle_.cancel();
}

//-------------------------------------------------------------------------------------
void Channel::endpoint(const EndPoint* endpoint)
{
	if (pEndPoint_ != endpoint)
	{
		delete pEndPoint_;
		pEndPoint_ = const_cast<EndPoint*>(endpoint);
	}
	
	lastReceivedTime_ = timestamp();
}

//-------------------------------------------------------------------------------------
void Channel::destroy()
{
	if(isDestroyed_)
	{
		CRITICAL_MSG("is channel has Destroyed!\n");
		return;
	}

	if(pNetworkInterface_ != NULL && pEndPoint_ != NULL)
	{
		pNetworkInterface_->onChannelGone(this);

		if(protocoltype_ == PROTOCOL_TCP)
		{
			pNetworkInterface_->dispatcher().deregisterReadFileDescriptor(*pEndPoint_);
			pEndPoint_->close();
		}
	}

	stopInactivityDetection();
	isDestroyed_ = true;
	this->decRef();
}

//-------------------------------------------------------------------------------------
void Channel::clearState( bool warnOnDiscard /*=false*/ )
{
	// 清空未处理的接受包缓存
	for(uint8 i=0; i<2; ++i)
	{
		if (bufferedReceives_[i].size() > 0)
		{
			BufferedReceives::iterator iter = bufferedReceives_[i].begin();
			int hasDiscard = 0;
			
			for(; iter != bufferedReceives_[i].end(); ++iter)
			{
				Packet* pPacket = (*iter);
				if(pPacket->length() > 0)
					hasDiscard++;

				if(pPacket->isTCPPacket())
					TCPPacket::ObjPool().reclaimObject(static_cast<TCPPacket*>(pPacket));
				else
					UDPPacket::ObjPool().reclaimObject(static_cast<UDPPacket*>(pPacket));
			}

			if (hasDiscard > 0 && warnOnDiscard)
			{
				WARNING_MSG(fmt::format("Channel::clearState( {} ): "
					"Discarding {} buffered packet(s)\n",
					this->c_str(), hasDiscard));
			}

			bufferedReceives_[i].clear();
		}
	}

	clearBundle();

	lastReceivedTime_ = timestamp();

	isCondemn_ = false;
	numPacketsSent_ = 0;
	numPacketsReceived_ = 0;
	numBytesSent_ = 0;
	numBytesReceived_ = 0;
	lastTickBytesReceived_ = 0;
	proxyID_ = 0;
	strextra_ = "";
	channelType_ = CHANNEL_NORMAL;
	bufferedReceivesIdx_ = 0;

	SAFE_RELEASE(pPacketReader_);
	pFilter_ = NULL;

	stopInactivityDetection();
	this->endpoint(NULL);
}

//-------------------------------------------------------------------------------------
Channel::Bundles & Channel::bundles()
{
	return bundles_;
}

//-------------------------------------------------------------------------------------
const Channel::Bundles & Channel::bundles() const
{
	return bundles_;
}

//-------------------------------------------------------------------------------------
int32 Channel::bundlesLength()
{
	int32 len = 0;
	Bundles::iterator iter = bundles_.begin();
	for(; iter != bundles_.end(); ++iter)
	{
		len += (*iter)->packetsLength();
	}

	return len;
}

//-------------------------------------------------------------------------------------
void Channel::pushBundle(Bundle* pBundle)
{
	bundles_.push_back(pBundle);

	// 如果打开了消息跟踪开关，这里就应该实时的发送出去
	if(Network::g_trace_packet > 0)
	{
		send();
	}
}

//-------------------------------------------------------------------------------------
void Channel::send(Bundle * pBundle)
{
	if (this->isDestroyed())
	{
		ERROR_MSG(fmt::format("Channel::send({}): Channel is destroyed.\n", 
			this->c_str()));
		
		this->clearBundle();
		return;
	}
	
	if(pBundle)
		bundles_.push_back(pBundle);

	if(bundles_.size() == 0)
		return;

	Bundles::iterator iter = bundles_.begin();
	for(; iter != bundles_.end(); ++iter)
	{
		(*iter)->send(*pNetworkInterface_, this);

		++numPacketsSent_;
		++g_numPacketsSent;
		numBytesSent_ += (*iter)->totalSize();
		g_numBytesSent += (*iter)->totalSize();
	}

	this->clearBundle();
}

//-------------------------------------------------------------------------------------
void Channel::delayedSend()
{
	this->networkInterface().delayedSend(*this);
}

//-------------------------------------------------------------------------------------
const char * Channel::c_str() const
{
	static char dodgyString[ MAX_BUF ] = {"None"};
	char tdodgyString[ MAX_BUF ] = {0};

	if(pEndPoint_ && !pEndPoint_->addr().isNone())
		pEndPoint_->addr().writeToString(tdodgyString, MAX_BUF);

	kbe_snprintf(dodgyString, MAX_BUF, "%s/%d/%d/%d", tdodgyString, id_, 
		this->isCondemn(), this->isDead());

	return dodgyString;
}

//-------------------------------------------------------------------------------------
void Channel::clearBundle()
{
	Bundles::iterator iter = bundles_.begin();
	for(; iter != bundles_.end(); ++iter)
	{
		Bundle::ObjPool().reclaimObject((*iter));
	}

	bundles_.clear();
}

//-------------------------------------------------------------------------------------
void Channel::handleTimeout(TimerHandle, void * arg)
{
	switch (reinterpret_cast<uintptr>(arg))
	{
		case TIMEOUT_INACTIVITY_CHECK:
		{
			if (timestamp() - lastReceivedTime_ > inactivityExceptionPeriod_)
			{
				this->networkInterface().onChannelTimeOut(this);
			}
			break;
		}
		default:
			break;
	}
}

//-------------------------------------------------------------------------------------
void Channel::reset(const EndPoint* endpoint, bool warnOnDiscard)
{
	// 如果地址没有改变则不重置
	if (endpoint == pEndPoint_)
	{
		return;
	}

	// 让网络接口下一个tick处理自己
	if(pNetworkInterface_)
		pNetworkInterface_->sendIfDelayed(*this);

	this->clearState(warnOnDiscard);
	this->endpoint(endpoint);
}

//-------------------------------------------------------------------------------------
void Channel::onPacketReceived(int bytes)
{
	lastReceivedTime_ = timestamp();
	++numPacketsReceived_;
	++g_numPacketsReceived;

	numBytesReceived_ += bytes;
	lastTickBytesReceived_ += bytes;
	g_numBytesReceived += bytes;

	if(this->isExternal())
	{
		if(g_extReceiveWindowBytesOverflow > 0 && 
			lastTickBytesReceived_ >= g_extReceiveWindowBytesOverflow)
		{
			ERROR_MSG(fmt::format("Channel::onPacketReceived[{:p}]: external channel({}), bufferedBytes is overflow({} > {}), Try adjusting the kbengine_defs.xml->receiveWindowOverflow.\n", 
				(void*)this, this->c_str(), lastTickBytesReceived_, g_extReceiveWindowBytesOverflow));

			this->condemn();
		}
	}
	else
	{
		if(g_intReceiveWindowBytesOverflow > 0 && 
			lastTickBytesReceived_ >= g_intReceiveWindowBytesOverflow)
		{
			WARNING_MSG(fmt::format("Channel::onPacketReceived[{:p}]: internal channel({}), bufferedBytes is overflow({} > {}).\n", 
				(void*)this, this->c_str(), lastTickBytesReceived_, g_intReceiveWindowBytesOverflow));
		}
	}
}

//-------------------------------------------------------------------------------------
void Channel::addReceiveWindow(Packet* pPacket)
{
	bufferedReceives_[bufferedReceivesIdx_].push_back(pPacket);

	if(Network::g_receiveWindowMessagesOverflowCritical > 0 && bufferedReceives_[bufferedReceivesIdx_].size() > Network::g_receiveWindowMessagesOverflowCritical)
	{
		if(this->isExternal())
		{
			WARNING_MSG(fmt::format("Channel::addReceiveWindow[{:p}]: external channel({}), bufferedMessages is overflow({} > {}).\n", 
				(void*)this, this->c_str(), (int)bufferedReceives_[bufferedReceivesIdx_].size(), Network::g_receiveWindowMessagesOverflowCritical));

			if(Network::g_extReceiveWindowMessagesOverflow > 0 && 
				bufferedReceives_[bufferedReceivesIdx_].size() >  Network::g_extReceiveWindowMessagesOverflow)
			{
				ERROR_MSG(fmt::format("Channel::addReceiveWindow[{:p}]: external channel({}), bufferedMessages is overflow({} > {}), Try adjusting the kbengine_defs.xml->receiveWindowOverflow.\n", 
					(void*)this, this->c_str(), (int)bufferedReceives_[bufferedReceivesIdx_].size(), Network::g_extReceiveWindowMessagesOverflow));

				this->condemn();
			}
		}
		else
		{
			if(Network::g_intReceiveWindowMessagesOverflow > 0 && 
				bufferedReceives_[bufferedReceivesIdx_].size() > Network::g_intReceiveWindowMessagesOverflow)
			{
				WARNING_MSG(fmt::format("Channel::addReceiveWindow[{:p}]: internal channel({}), bufferedMessages is overflow({} > {}).\n", 
					(void*)this, this->c_str(), (int)bufferedReceives_[bufferedReceivesIdx_].size(), Network::g_intReceiveWindowMessagesOverflow));
			}
		}
	}
}

//-------------------------------------------------------------------------------------
void Channel::condemn()
{ 
	isCondemn_ = true; 
	ERROR_MSG(fmt::format("Channel::condemn[{:p}]: channel({}).\n", (void*)this, this->c_str())); 
}

//-------------------------------------------------------------------------------------
void Channel::handshake()
{
	if(bufferedReceives_[bufferedReceivesIdx_].size() > 0)
	{
		BufferedReceives::iterator packetIter = bufferedReceives_[bufferedReceivesIdx_].begin();
		Packet* pPacket = (*packetIter);
		
		// 此处判定是否为websocket或者其他协议的握手
		if(html5::WebSocketProtocol::isWebSocketProtocol(pPacket))
		{
			channelType_ = CHANNEL_WEB;
			if(html5::WebSocketProtocol::handshake(this, pPacket))
			{
				if(pPacket->totalSize() == 0)
				{
					bufferedReceives_[bufferedReceivesIdx_].erase(packetIter);
				}

				pPacketReader_ = new HTML5PacketReader(this);
				pFilter_ = new HTML5PacketFilter(this);
				DEBUG_MSG(fmt::format("Channel::handshake: websocket({}) successfully!\n", this->c_str()));
				return;
			}
			else
			{
				DEBUG_MSG(fmt::format("Channel::handshake: websocket({}) error!\n", this->c_str()));
			}
		}

		pPacketReader_ = new PacketReader(this);
	}
}

//-------------------------------------------------------------------------------------
void Channel::processPackets(KBEngine::Network::MessageHandlers* pMsgHandlers)
{
	lastTickBytesReceived_ = 0;

	if(pMsgHandlers_ != NULL)
	{
		pMsgHandlers = pMsgHandlers_;
	}

	if (this->isDestroyed())
	{
		ERROR_MSG(fmt::format("Channel::processPackets({}): channel[{:p}] is destroyed.\n", 
			this->c_str(), (void*)this));

		return;
	}

	if(this->isCondemn())
	{
		ERROR_MSG(fmt::format("Channel::processPackets({}): channel[{:p}] is condemn.\n", 
			this->c_str(), (void*)this));

		//this->destroy();
		return;
	}
	
	if(pPacketReader_ == NULL)
	{
		handshake();
	}
	
	uint8 idx = bufferedReceivesIdx_;
	bufferedReceivesIdx_ = 1 - bufferedReceivesIdx_;

	try
	{
		BufferedReceives::iterator packetIter = bufferedReceives_[idx].begin();
		for(; packetIter != bufferedReceives_[idx].end(); ++packetIter)
		{
			Packet* pPacket = (*packetIter);

			pPacketReader_->processMessages(pMsgHandlers, pPacket);

			if(pPacket->isTCPPacket())
				TCPPacket::ObjPool().reclaimObject(static_cast<TCPPacket*>(pPacket));
			else
				UDPPacket::ObjPool().reclaimObject(static_cast<UDPPacket*>(pPacket));

		}
	}catch(MemoryStreamException &)
	{
		Network::MessageHandler* pMsgHandler = pMsgHandlers->find(pPacketReader_->currMsgID());
		WARNING_MSG(fmt::format("Channel::processPackets({}): packet invalid. currMsg=(name={}, id={}, len={}), currMsgLen={}\n",
			this->c_str()
			, (pMsgHandler == NULL ? "unknown" : pMsgHandler->name) 
			, pPacketReader_->currMsgID() 
			, (pMsgHandler == NULL ? -1 : pMsgHandler->msgLen) 
			, pPacketReader_->currMsgLen()));

		pPacketReader_->currMsgID(0);
		pPacketReader_->currMsgLen(0);
		condemn();
	}

	bufferedReceives_[idx].clear();
	this->send();
}

//-------------------------------------------------------------------------------------
bool Channel::waitSend()
{
	return endpoint()->waitSend();
}

//-------------------------------------------------------------------------------------
EventDispatcher & Channel::dispatcher()
{
	return pNetworkInterface_->dispatcher();
}

//-------------------------------------------------------------------------------------

}
}
