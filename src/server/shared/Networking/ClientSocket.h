/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __CLIENT_SOCKET_H__
#define __CLIENT_SOCKET_H__

#include <atomic>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/connect.hpp>
#include <functional>
#include <memory>
#include <queue>
#include <type_traits>
#include "Log.h"
#include "MessageBuffer.h"
#include "Socket.h"

using boost::asio::ip::tcp;

#define READ_BLOCK_SIZE 4096
#ifdef BOOST_ASIO_HAS_IOCP
#    define TC_SOCKET_USE_IOCP
#endif

template <class T>
class ClientSocket : public Socket<T>
{
  public:
    using SocketBase = Socket<T>;

    explicit ClientSocket(Trinity::Asio::IoContext& ioContext)
    : SocketBase(ioContext), _resolver(ioContext), _hostAddress(), _hostPort()
    {
        
    }

    void Connect(const std::string& host, uint16_t port)
    {
        _hostAddress = host;
        _hostPort    = port;

        Reconnect();
    }

    void Reconnect() {
        this->_closed.exchange(false);
        this->_closing.exchange(false);

        auto self = this->shared_from_this();
        _resolver.async_resolve(
            _hostAddress, std::to_string(_hostPort),
            [this, self](const boost::system::error_code& ec, tcp::resolver::results_type results)
            {
                if (ec)
                {
                    TC_LOG_ERROR("network", "ClientSocket: Resolve failed: {}", ec.message());
                    this->OnClose();
                    return;
                }

                boost::asio::async_connect(
                    this->_socket, results,
                    [this, self](const boost::system::error_code& ec, const tcp::endpoint& /*endpoint*/)
                    {
                        if (ec)
                        {
                            TC_LOG_ERROR("network", "ClientSocket: Connect failed: {}", ec.message());
                            this->OnClose();
                            return;
                        }

                        this->_remoteAddress = this->_socket.remote_endpoint().address();
                        this->_remotePort    = this->_socket.remote_endpoint().port();

                        this->SetNoDelay(true);
                        this->Start(); // Kick off reading or state machine
                    });
            });
    }

  private:
    tcp::resolver _resolver;

    std::string _hostAddress;
    uint16_t _hostPort;
};

#endif // __CLIENT_SOCKET_H__
