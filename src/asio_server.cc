/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2014 Tatsuhiro Tsujikawa
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
// We wrote this code based on the original code which has the
// following license:
//
// server.cpp
// ~~~~~~~~~~
//
// Copyright (c) 2003-2013 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//

#include "asio_server.h"

#include <boost/date_time/posix_time/posix_time.hpp>

namespace nghttp2 {
namespace asio_http2 {
namespace server {

server::server(const std::string& address, uint16_t port,
               std::size_t io_service_pool_size,
               std::size_t thread_pool_size,
               request_cb cb,
               std::unique_ptr<boost::asio::ssl::context> ssl_ctx)
  : io_service_pool_(io_service_pool_size, thread_pool_size),
    signals_(io_service_pool_.get_io_service()),
    tick_timer_(io_service_pool_.get_io_service(),
                boost::posix_time::seconds(1)),
    acceptor_(io_service_pool_.get_io_service()),
    ssl_ctx_(std::move(ssl_ctx)),
    request_cb_(std::move(cb))
{
  // Register to handle the signals that indicate when the server should exit.
  // It is safe to register for the same signal multiple times in a program,
  // provided all registration for the specified signal is made through Asio.
  signals_.add(SIGINT);
  signals_.add(SIGTERM);
#if defined(SIGQUIT)
  signals_.add(SIGQUIT);
#endif // defined(SIGQUIT)
  signals_.async_wait([this](const boost::system::error_code& error,
                             int signal_number)
                      {
                        io_service_pool_.stop();
                      });

  // Open the acceptor with the option to reuse the address (i.e. SO_REUSEADDR).
  boost::asio::ip::tcp::resolver resolver(acceptor_.get_io_service());
  boost::asio::ip::tcp::resolver::query query(address, std::to_string(port));
  boost::asio::ip::tcp::endpoint endpoint = *resolver.resolve(query);
  acceptor_.open(endpoint.protocol());
  acceptor_.set_option(boost::asio::ip::tcp::acceptor::reuse_address(true));
  acceptor_.bind(endpoint);
  acceptor_.listen();

  start_accept();

  start_timer();
}

void server::run()
{
  io_service_pool_.run();
}

std::shared_ptr<std::string> cached_date;

namespace {
void update_date()
{
  cached_date = std::make_shared<std::string>(util::http_date(time(nullptr)));
}
} // namespace

void server::start_timer()
{
  update_date();

  tick_timer_.async_wait
    ([this](const boost::system::error_code& e)
     {
       tick_timer_.expires_at(tick_timer_.expires_at() +
                              boost::posix_time::seconds(1));
       start_timer();
     });
}

typedef boost::asio::ssl::stream<boost::asio::ip::tcp::socket> ssl_socket;

void server::start_accept()
{
  if(ssl_ctx_) {
    auto new_connection =
      std::make_shared<connection<ssl_socket>>
      (request_cb_, io_service_pool_.get_task_io_service(),
       io_service_pool_.get_io_service(), *ssl_ctx_);

    acceptor_.async_accept
      (new_connection->socket().lowest_layer(),
       [this, new_connection](const boost::system::error_code& e)
       {
         if(!e) {
           new_connection->socket().lowest_layer().set_option
             (boost::asio::ip::tcp::no_delay(true));
           new_connection->socket().async_handshake
             (boost::asio::ssl::stream_base::server,
              [new_connection](const boost::system::error_code& e)
              {
                if(!e) {
                  new_connection->start();
                }
              });
         }

         start_accept();
       });
  } else {
    auto new_connection =
      std::make_shared<connection<boost::asio::ip::tcp::socket>>
      (request_cb_, io_service_pool_.get_task_io_service(),
       io_service_pool_.get_io_service());

    acceptor_.async_accept
      (new_connection->socket(),
       [this, new_connection](const boost::system::error_code& e)
       {
         if (!e) {
           new_connection->socket().set_option
             (boost::asio::ip::tcp::no_delay(true));
           new_connection->start();
         }

         start_accept();
       });
  }
}

} // namespace server
} // namespace asio_http2
} // namespace nghttp2
