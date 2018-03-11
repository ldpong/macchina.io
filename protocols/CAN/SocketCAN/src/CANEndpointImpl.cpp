//
// CANEndpointImpl.cpp
//
// Copyright (c) 2018, Applied Informatics Software Engineering GmbH.
// All rights reserved.
//
// SPDX-License-Identifier: Apache-2.0
//


#include "CANEndpointImpl.h"
#ifdef MACCHINA_HAVE_SOCKETCAN
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <errno.h>
#endif // MACCHINA_HAVE_SOCKETCAN
#include <cstring>


namespace IoT {
namespace CAN {
namespace SocketCAN {


bool operator == (const Filter& f1, const Filter& f2)
{
	return f1.id == f2.id && f1.mask == f2.mask;
}


CANEndpointImpl::CANEndpointImpl(const std::string& interfc):
	_interfc(interfc),
	_socket(interfc),
	_filterMode(CAN_FILTER_MODE_OR),
	_enableEvents(0),
	_logger(Poco::Logger::get("IoT.CAN.SocketCAN.CANEndpointImpl"))
{
}


CANEndpointImpl::~CANEndpointImpl()
{
	Poco::ScopedLockWithUnlock<Poco::FastMutex> lock(_mutex);

	if (_enableEvents)
	{
		_enableEvents = 0;
		lock.unlock();
		_thread.join();
	}
}


void CANEndpointImpl::run()
{
	Poco::Timespan timeout(CAN_POLL_TIMEOUT);

	while (eventsEnabled())
	{
		try
		{
#ifdef MACCHINA_HAVE_SOCKETCAN
			if (_socket.poll(timeout, Poco::Net::Socket::SELECT_READ))
			{
#ifdef CAN_RAW_FD_FRAMES
				struct canfd_frame scFrame;
				int n = _socket.receiveBytes(&scFrame, sizeof(scFrame));
				if (n == CAN_MTU)
				{
					CANFrame::Flags flags(0);
					if (scFrame.can_id & CAN_EFF_FLAG) flags |= CANFrame::CAN_FLAG_IDE;
					if (scFrame.can_id & CAN_RTR_FLAG) flags |= CANFrame::CAN_FLAG_RTR;
					CANFrame::ID id = scFrame.can_id & CAN_EFF_MASK;

					CANFrame frame(id, flags, scFrame.len, reinterpret_cast<char*>(&scFrame.data[0]));
					frameReceived(this, frame);
				}
				else if (n == CANFD_MTU)
				{
					CANFDFrame::Flags flags(0);
					if (scFrame.can_id & CAN_EFF_FLAG) flags |= CANFrame::CAN_FLAG_IDE;
					if (scFrame.can_id & CAN_RTR_FLAG) flags |= CANFrame::CAN_FLAG_RTR;
					CANFDFrame::ID id = scFrame.can_id & CAN_EFF_MASK;

					CANFDFrame frame(id, flags, scFrame.len, reinterpret_cast<char*>(&scFrame.data[0]));
					sdFrameReceived(this, frame);
				}
#else
				struct can_frame scFrame;
				int n = _socket.receiveBytes(&scFrame, sizeof(scFrame));
				if (n == CAN_MTU)
				{
					CANFrame::Flags flags(0);
					if (scFrame.can_id & CAN_EFF_FLAG) flags |= CANFrame::CAN_FLAG_IDE;
					if (scFrame.can_id & CAN_RTR_FLAG) flags |= CANFrame::CAN_FLAG_RTR;
					CANFrame::ID id = scFrame.can_id & CAN_EFF_MASK;

					CANFrame frame(id, flags, scFrame.can_dlc, reinterpret_cast<char*>(&scFrame.data[0]));
					frameReceived(this, frame);
				}
#endif // CAN_RAW_FD_FRAMES
			}
#endif // MACCHINA_HAVE_SOCKETCAN
		}
		catch (Poco::Exception& exc)
		{
			_logger.log(exc);
		}
	}
}


std::string CANEndpointImpl::device() const
{
	return _interfc;
}


void CANEndpointImpl::setFilter(const std::vector<Filter>& filter)
{
	Poco::FastMutex::ScopedLock lock(_mutex);

	_filter = filter;
	applyFilter();
}


std::vector<Filter> CANEndpointImpl::getFilter() const
{
	Poco::FastMutex::ScopedLock lock(_mutex);

	return _filter;
}


bool CANEndpointImpl::addFilter(const Filter& filter)
{
	Poco::FastMutex::ScopedLock lock(_mutex);

	for (std::vector<Filter>::iterator it = _filter.begin(); it != _filter.end(); ++it)
	{
		if (filter == *it) return false;
	}
	_filter.push_back(filter);
	applyFilter();
	return true;
}


bool CANEndpointImpl::removeFilter(const Filter& filter)
{
	Poco::FastMutex::ScopedLock lock(_mutex);

	for (std::vector<Filter>::iterator it = _filter.begin(); it != _filter.end(); ++it)
	{
		if (filter == *it)
		{
			_filter.erase(it);
			applyFilter();
			return true;
		}
	}
	return false;
}


void CANEndpointImpl::setFilterMode(CANEndpoint::FilterMode mode)
{
#ifdef MACCHINA_HAVE_SOCKETCAN
	if (mode == CAN_FILTER_MODE_AND)
	{
		_socket.setOption(SOL_CAN_RAW, CAN_RAW_JOIN_FILTERS, 1);
	}
	else
	{
		_socket.setOption(SOL_CAN_RAW, CAN_RAW_JOIN_FILTERS, 0);
	}
#endif
	_filterMode = mode;
}


CANEndpoint::FilterMode CANEndpointImpl::getFilterMode() const
{
	return _filterMode;
}


void CANEndpointImpl::sendFrame(const CANFrame& frame)
{
	if (_logger.debug())
	{
		_logger.dump("Sending CAN frame", frame.payload().data(), frame.dlc());
	}
#ifdef MACCHINA_HAVE_SOCKETCAN
	struct can_frame scFrame;
	scFrame.can_id = frame.id();
	if (frame.flags() & CANFrame::CAN_FLAG_IDE) scFrame.can_id |= CAN_EFF_FLAG;
	if (frame.flags() & CANFrame::CAN_FLAG_RTR) scFrame.can_id |= CAN_RTR_FLAG;
	scFrame.can_dlc = frame.dlc();
	std::memcpy(scFrame.data, frame.payload().data(), frame.dlc());
	_socket.sendBytes(&scFrame, sizeof(scFrame));
#endif
}


void CANEndpointImpl::sendFDFrame(const CANFDFrame& frame)
{
	if (_logger.debug())
	{
		_logger.dump("Sending CAN-FD frame", frame.payload().data(), frame.length());
	}
#ifdef MACCHINA_HAVE_SOCKETCAN
	struct canfd_frame scFrame;
	scFrame.can_id = frame.id();
	if (frame.flags() & CANFrame::CAN_FLAG_IDE) scFrame.can_id |= CAN_EFF_FLAG;
	if (frame.flags() & CANFrame::CAN_FLAG_RTR) scFrame.can_id |= CAN_RTR_FLAG;
	scFrame.len = frame.length();
	std::memcpy(scFrame.data, frame.payload().data(), frame.length());
	_socket.sendBytes(&scFrame, sizeof(scFrame));
#endif
}


void CANEndpointImpl::enableEvents(bool enable)
{
	Poco::FastMutex::ScopedLock lock(_mutex);

	if (enable)
	{
		if (!_enableEvents++)
		{
			_thread.start(*this);
		}
	}
	else if (_enableEvents > 0)
	{
		if (--_enableEvents == 0)
		{
			_thread.join();
		}
	}
}


bool CANEndpointImpl::eventsEnabled() const
{
	Poco::FastMutex::ScopedLock lock(_mutex);

	return _enableEvents > 0;
}


void CANEndpointImpl::enableFDEvents(bool enable)
{
#ifdef CAN_RAW_FD_FRAMES
	Poco::FastMutex::ScopedLock lock(_mutex);

	if (enable)
	{
		if (!_enableFDEvents++)
		{
			_socket.setOption(SOL_CAN_RAW, CAN_RAW_FD_FRAMES, 1);
		}
	}
	else if (_enableFDEvents > 0)
	{
		if (--_enableFDEvents == 0)
		{
			_socket.setOption(SOL_CAN_RAW, CAN_RAW_FD_FRAMES, 0);
		}
	}
#else
	if (enable) throw Poco::NotImplementedException("CAN FD frames not supported by platform's SocketCAN");
#endif
}


bool CANEndpointImpl::fdEventsEnabled() const
{
	Poco::FastMutex::ScopedLock lock(_mutex);

	return _enableFDEvents > 0;
}


bool CANEndpointImpl::fdFramesSupported() const
{
#ifdef CAN_RAW_FD_FRAMES
	return true;
#else
	return false;
#endif
}


void CANEndpointImpl::applyFilter()
{
#ifdef MACCHINA_HAVE_SOCKETCAN
	std::vector<struct can_filter> fvec;
	fvec.resize(_filter.size());
	for (std::size_t i = 0; i < _filter.size(); i++)
	{
		fvec[i].can_id = _filter[i].id;
		fvec[i].can_mask = _filter[i].mask;
		if (_filter[i].invert) fvec[i].can_id |= CAN_INV_FILTER;
	}

	if (setsockopt(_socket.impl()->sockfd(), SOL_CAN_RAW, CAN_RAW_FILTER, &fvec[0], sizeof(struct can_filter)*fvec.size()) == -1)
		throw Poco::IOException("Failed to set CAN frame filter", errno);
#endif
}


} } } // namespace IoT::CAN::SocketCAN