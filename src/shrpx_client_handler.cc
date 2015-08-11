/*
 * nghttp2 - HTTP/2.0 C Library
 *
 * Copyright (c) 2012 Tatsuhiro Tsujikawa
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
#include "shrpx_client_handler.h"

#include <unistd.h>
#include <cerrno>

#include "shrpx_upstream.h"
#include "shrpx_http2_upstream.h"
#include "shrpx_https_upstream.h"
#include "shrpx_config.h"
#include "shrpx_http_downstream_connection.h"
#include "shrpx_http2_downstream_connection.h"
#include "shrpx_accesslog.h"
#include "shrpx_ssl.h"
#ifdef HAVE_SPDYLAY
#include "shrpx_spdy_upstream.h"
#endif // HAVE_SPDYLAY
#include "util.h"

using namespace nghttp2;

namespace shrpx {

namespace {
void upstream_readcb(bufferevent *bev, void *arg)
{
  auto handler = static_cast<ClientHandler*>(arg);
  int rv = handler->on_read();
  if(rv != 0) {
    delete handler;
  }
}
} // namespace

namespace {
void upstream_writecb(bufferevent *bev, void *arg)
{
  auto handler = static_cast<ClientHandler*>(arg);
  // We actually depend on write low-warter mark == 0.
  if(handler->get_outbuf_length() > 0) {
    // Possibly because of deferred callback, we may get this callback
    // when the output buffer is not empty.
    return;
  }
  if(handler->get_should_close_after_write()) {
    delete handler;
    return;
  }
  auto upstream = handler->get_upstream();
  if(!upstream) {
    return;
  }
  int rv = upstream->on_write();
  if(rv != 0) {
    delete handler;
  }
}
} // namespace

namespace {
void upstream_eventcb(bufferevent *bev, short events, void *arg)
{
  auto handler = static_cast<ClientHandler*>(arg);
  bool finish = false;
  if(events & BEV_EVENT_EOF) {
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "EOF";
    }
    finish = true;
  }
  if(events & BEV_EVENT_ERROR) {
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "Network error: "
                          << evutil_socket_error_to_string
        (EVUTIL_SOCKET_ERROR());
    }
    finish = true;
  }
  if(events & BEV_EVENT_TIMEOUT) {
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "Time out";
    }
    finish = true;
  }
  if(finish) {
    delete handler;
  } else {
    if(events & BEV_EVENT_CONNECTED) {
      handler->set_tls_handshake(true);
      if(LOG_ENABLED(INFO)) {
        CLOG(INFO, handler) << "SSL/TLS handshake completed";
      }
      if(handler->validate_next_proto() != 0) {
        delete handler;
        return;
      }
      if(LOG_ENABLED(INFO)) {
        if(SSL_session_reused(handler->get_ssl())) {
          CLOG(INFO, handler) << "SSL/TLS session reused";
        }
      }
      // At this point, input buffer is already filled with some
      // bytes.  The read callback is not called until new data
      // come. So consume input buffer here.
      if(handler->get_upstream()->on_read() != 0) {
        delete handler;
        return;
      }
    }
  }
}
} // namespace

namespace {
void upstream_http2_connhd_readcb(bufferevent *bev, void *arg)
{
  // This callback assumes upstream is Http2Upstream.
  uint8_t data[NGHTTP2_CLIENT_CONNECTION_HEADER_LEN];
  auto handler = static_cast<ClientHandler*>(arg);
  auto leftlen = handler->get_left_connhd_len();
  auto input = bufferevent_get_input(bev);
  auto readlen = evbuffer_remove(input, data, leftlen);
  if(readlen == -1) {
    delete handler;
    return;
  }
  if(memcmp(NGHTTP2_CLIENT_CONNECTION_HEADER +
            NGHTTP2_CLIENT_CONNECTION_HEADER_LEN - leftlen,
            data, readlen) != 0) {
    // There is no downgrade path here. Just drop the connection.
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "invalid client connection header";
    }
    delete handler;
    return;
  }
  leftlen -= readlen;
  handler->set_left_connhd_len(leftlen);
  if(leftlen == 0) {
    handler->set_bev_cb(upstream_readcb, upstream_writecb, upstream_eventcb);
    // Run on_read to process data left in buffer since they are not
    // notified further
    if(handler->on_read() != 0) {
      delete handler;
      return;
    }
  }
}
} // namespace

namespace {
void upstream_http1_connhd_readcb(bufferevent *bev, void *arg)
{
  // This callback assumes upstream is HttpsUpstream.
  uint8_t data[NGHTTP2_CLIENT_CONNECTION_HEADER_LEN];
  auto handler = static_cast<ClientHandler*>(arg);
  auto leftlen = handler->get_left_connhd_len();
  auto input = bufferevent_get_input(bev);
  auto readlen = evbuffer_copyout(input, data, leftlen);
  if(readlen == -1) {
    delete handler;
    return;
  }
  if(memcmp(NGHTTP2_CLIENT_CONNECTION_HEADER +
            NGHTTP2_CLIENT_CONNECTION_HEADER_LEN - leftlen,
            data, readlen) != 0) {
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "This is HTTP/1.1 connection, "
                          << "but may be upgraded to HTTP/2.0 later.";
    }
    // Reset header length for later HTTP/2.0 upgrade
    handler->set_left_connhd_len(NGHTTP2_CLIENT_CONNECTION_HEADER_LEN);
    handler->set_bev_cb(upstream_readcb, upstream_writecb, upstream_eventcb);
    if(handler->on_read() != 0) {
      delete handler;
      return;
    }
    return;
  }
  if(evbuffer_drain(input, readlen) == -1) {
    delete handler;
    return;
  }
  leftlen -= readlen;
  handler->set_left_connhd_len(leftlen);
  if(leftlen == 0) {
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "direct HTTP/2.0 connection";
    }
    handler->direct_http2_upgrade();
    handler->set_bev_cb(upstream_readcb, upstream_writecb, upstream_eventcb);
    // Run on_read to process data left in buffer since they are not
    // notified further
    if(handler->on_read() != 0) {
      delete handler;
      return;
    }
  }
}
} // namespace

namespace {
void tls_raw_readcb(evbuffer *buffer, const evbuffer_cb_info *info, void *arg)
{
  auto handler = static_cast<ClientHandler*>(arg);
  if(handler->get_tls_renegotiation()) {
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, handler) << "Close connection due to TLS renegotiation";
    }
    delete handler;
  }
}
} // namespace

namespace {
void tls_raw_writecb(evbuffer *buffer, const evbuffer_cb_info *info, void *arg)
{
  auto handler = static_cast<ClientHandler*>(arg);
  // upstream_writecb() is called when external bufferevent
  // handler->bev's output buffer gets empty. But the underlying
  // bufferevent may have pending output buffer.
  upstream_writecb(handler->get_bev(), handler);
}
} // namespace

ClientHandler::ClientHandler(bufferevent *bev, int fd, SSL *ssl,
                             const char *ipaddr)
  : ipaddr_(ipaddr),
    bev_(bev),
    http2session_(nullptr),
    ssl_(ssl),
    left_connhd_len_(NGHTTP2_CLIENT_CONNECTION_HEADER_LEN),
    fd_(fd),
    should_close_after_write_(false),
    tls_handshake_(false),
    tls_renegotiation_(false)
{
  int rv;
  rv = bufferevent_set_rate_limit(bev_, get_config()->rate_limit_cfg);
  if(rv == -1) {
    CLOG(FATAL, this) << "bufferevent_set_rate_limit() failed";
  }
  bufferevent_enable(bev_, EV_READ | EV_WRITE);
  bufferevent_setwatermark(bev_, EV_READ, 0, SHRPX_READ_WARTER_MARK);
  set_upstream_timeouts(&get_config()->upstream_read_timeout,
                        &get_config()->upstream_write_timeout);
  if(ssl_) {
    SSL_set_app_data(ssl_, reinterpret_cast<char*>(this));
    set_bev_cb(nullptr, upstream_writecb, upstream_eventcb);
    auto input = bufferevent_get_input(bufferevent_get_underlying(bev_));
    evbuffer_add_cb(input, tls_raw_readcb, this);
    auto output = bufferevent_get_output(bufferevent_get_underlying(bev_));
    evbuffer_add_cb(output, tls_raw_writecb, this);
  } else {
    // For non-TLS version, first create HttpsUpstream. It may be
    // upgraded to HTTP/2.0 through HTTP Upgrade or direct HTTP/2.0
    // connection.
    upstream_ = util::make_unique<HttpsUpstream>(this);
    set_bev_cb(upstream_http1_connhd_readcb, nullptr, upstream_eventcb);
  }
}

ClientHandler::~ClientHandler()
{
  if(LOG_ENABLED(INFO)) {
    CLOG(INFO, this) << "Deleting";
  }
  if(ssl_) {
    SSL_set_app_data(ssl_, nullptr);
    SSL_set_shutdown(ssl_, SSL_RECEIVED_SHUTDOWN);
    SSL_shutdown(ssl_);
  }
  auto underlying = bufferevent_get_underlying(bev_);
  bufferevent_disable(bev_, EV_READ | EV_WRITE);
  bufferevent_free(bev_);
  if(ssl_) {
    SSL_free(ssl_);
  }
  if(underlying) {
    bufferevent_disable(underlying, EV_READ | EV_WRITE);
    bufferevent_free(underlying);
  }
  shutdown(fd_, SHUT_WR);
  close(fd_);
  for(auto dconn : dconn_pool_) {
    delete dconn;
  }
  if(LOG_ENABLED(INFO)) {
    CLOG(INFO, this) << "Deleted";
  }
}

Upstream* ClientHandler::get_upstream()
{
  return upstream_.get();
}

bufferevent* ClientHandler::get_bev() const
{
  return bev_;
}

event_base* ClientHandler::get_evbase() const
{
  return bufferevent_get_base(bev_);
}

void ClientHandler::set_bev_cb
(bufferevent_data_cb readcb, bufferevent_data_cb writecb,
 bufferevent_event_cb eventcb)
{
  bufferevent_setcb(bev_, readcb, writecb, eventcb, this);
}

void ClientHandler::set_upstream_timeouts(const timeval *read_timeout,
                                          const timeval *write_timeout)
{
  bufferevent_set_timeouts(bev_, read_timeout, write_timeout);
}

int ClientHandler::validate_next_proto()
{
  const unsigned char *next_proto = nullptr;
  unsigned int next_proto_len;
  // First set callback for catch all cases
  set_bev_cb(upstream_readcb, upstream_writecb, upstream_eventcb);
  SSL_get0_next_proto_negotiated(ssl_, &next_proto, &next_proto_len);
  for(int i = 0; i < 2; ++i) {
    if(next_proto) {
      if(LOG_ENABLED(INFO)) {
        std::string proto(next_proto, next_proto+next_proto_len);
        CLOG(INFO, this) << "The negotiated next protocol: " << proto;
      }
      if(!ssl::in_proto_list(get_config()->npn_list,
                             get_config()->npn_list_len,
                             next_proto, next_proto_len)) {
        break;
      }
      if(next_proto_len == NGHTTP2_PROTO_VERSION_ID_LEN &&
         memcmp(NGHTTP2_PROTO_VERSION_ID, next_proto,
                NGHTTP2_PROTO_VERSION_ID_LEN) == 0) {
        set_bev_cb(upstream_http2_connhd_readcb, upstream_writecb,
                   upstream_eventcb);
        upstream_ = util::make_unique<Http2Upstream>(this);
        return 0;
      } else {
#ifdef HAVE_SPDYLAY
        uint16_t version = spdylay_npn_get_version(next_proto, next_proto_len);
        if(version) {
          upstream_ = util::make_unique<SpdyUpstream>(version, this);
          return 0;
        }
#endif // HAVE_SPDYLAY
        if(next_proto_len == 8 && memcmp("http/1.1", next_proto, 8) == 0) {
          upstream_ = util::make_unique<HttpsUpstream>(this);
          return 0;
        }
      }
      break;
    }
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    SSL_get0_alpn_selected(ssl_, &next_proto, &next_proto_len);
#else // OPENSSL_VERSION_NUMBER < 0x10002000L
    break;
#endif // OPENSSL_VERSION_NUMBER < 0x10002000L
  }
  if(!next_proto) {
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, this) << "No protocol negotiated. Fallback to HTTP/1.1";
    }
    upstream_ = util::make_unique<HttpsUpstream>(this);
    return 0;
  }
  if(LOG_ENABLED(INFO)) {
    CLOG(INFO, this) << "The negotiated protocol is not supported";
  }
  return -1;
}

int ClientHandler::on_read()
{
  return upstream_->on_read();
}

int ClientHandler::on_event()
{
  return upstream_->on_event();
}

const std::string& ClientHandler::get_ipaddr() const
{
  return ipaddr_;
}

bool ClientHandler::get_should_close_after_write() const
{
  return should_close_after_write_;
}

void ClientHandler::set_should_close_after_write(bool f)
{
  should_close_after_write_ = f;
}

void ClientHandler::pool_downstream_connection(DownstreamConnection *dconn)
{
  if(LOG_ENABLED(INFO)) {
    CLOG(INFO, this) << "Pooling downstream connection DCONN:" << dconn;
  }
  dconn_pool_.insert(dconn);
}

void ClientHandler::remove_downstream_connection(DownstreamConnection *dconn)
{
  if(LOG_ENABLED(INFO)) {
    CLOG(INFO, this) << "Removing downstream connection DCONN:" << dconn
                     << " from pool";
  }
  dconn_pool_.erase(dconn);
}

DownstreamConnection* ClientHandler::get_downstream_connection()
{
  if(dconn_pool_.empty()) {
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, this) << "Downstream connection pool is empty."
                       << " Create new one";
    }
    if(http2session_) {
      return new Http2DownstreamConnection(this);
    } else {
      return new HttpDownstreamConnection(this);
    }
  } else {
    auto dconn = *std::begin(dconn_pool_);
    dconn_pool_.erase(dconn);
    if(LOG_ENABLED(INFO)) {
      CLOG(INFO, this) << "Reuse downstream connection DCONN:" << dconn
                       << " from pool";
    }
    return dconn;
  }
}

size_t ClientHandler::get_outbuf_length()
{
  auto underlying = bufferevent_get_underlying(bev_);
  auto len = evbuffer_get_length(bufferevent_get_output(bev_));
  if(underlying) {
    len += evbuffer_get_length(bufferevent_get_output(underlying));
  }
  return len;
}

SSL* ClientHandler::get_ssl() const
{
  return ssl_;
}

void ClientHandler::set_http2_session(Http2Session *http2session)
{
  http2session_ = http2session;
}

Http2Session* ClientHandler::get_http2_session() const
{
  return http2session_;
}

size_t ClientHandler::get_left_connhd_len() const
{
  return left_connhd_len_;
}

void ClientHandler::set_left_connhd_len(size_t left)
{
  left_connhd_len_ = left;
}

void ClientHandler::direct_http2_upgrade()
{
  upstream_= util::make_unique<Http2Upstream>(this);
  set_bev_cb(upstream_readcb, upstream_writecb, upstream_eventcb);
}

int ClientHandler::perform_http2_upgrade(HttpsUpstream *http)
{
  int rv;
  auto upstream = util::make_unique<Http2Upstream>(this);
  if(upstream->upgrade_upstream(http) != 0) {
    return -1;
  }
  // http pointer is now owned by upstream.
  upstream_.release();
  upstream_ = std::move(upstream);
  set_bev_cb(upstream_http2_connhd_readcb, upstream_writecb, upstream_eventcb);
  static char res[] = "HTTP/1.1 101 Switching Protocols\r\n"
    "Connection: Upgrade\r\n"
    "Upgrade: " NGHTTP2_PROTO_VERSION_ID "\r\n"
    "\r\n";
  rv = bufferevent_write(bev_, res, sizeof(res) - 1);
  if(rv != 0) {
    CLOG(FATAL, this) << "bufferevent_write() faild";
    return -1;
  }
  return 0;
}

bool ClientHandler::get_http2_upgrade_allowed() const
{
  return !ssl_;
}

std::string ClientHandler::get_upstream_scheme() const
{
  if(ssl_) {
    return "https";
  } else {
    return "http";
  }
}

void ClientHandler::set_tls_handshake(bool f)
{
  tls_handshake_ = f;
}

bool ClientHandler::get_tls_handshake() const
{
  return tls_handshake_;
}

void ClientHandler::set_tls_renegotiation(bool f)
{
  tls_renegotiation_ = f;
}

bool ClientHandler::get_tls_renegotiation() const
{
  return tls_renegotiation_;
}

} // namespace shrpx
