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
#include "shrpx_http2_downstream_connection.h"

#include <unistd.h>

#include <openssl/err.h>

#include <event2/bufferevent_ssl.h>

#include "http-parser/http_parser.h"

#include "shrpx_client_handler.h"
#include "shrpx_upstream.h"
#include "shrpx_downstream.h"
#include "shrpx_config.h"
#include "shrpx_error.h"
#include "shrpx_http.h"
#include "shrpx_http2_session.h"
#include "http2.h"
#include "util.h"

using namespace nghttp2;

namespace shrpx {

Http2DownstreamConnection::Http2DownstreamConnection
(ClientHandler *client_handler)
  : DownstreamConnection(client_handler),
    http2session_(client_handler->get_http2_session()),
    request_body_buf_(nullptr),
    sd_(nullptr)
{}

Http2DownstreamConnection::~Http2DownstreamConnection()
{
  if(LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Deleting";
  }
  if(request_body_buf_) {
    evbuffer_free(request_body_buf_);
  }
  if(downstream_) {
    if(submit_rst_stream(downstream_) == 0) {
      http2session_->notify();
    }
  }
  http2session_->remove_downstream_connection(this);
  // Downstream and DownstreamConnection may be deleted
  // asynchronously.
  if(downstream_) {
    downstream_->set_downstream_connection(nullptr);
  }
  if(LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Deleted";
  }
}

int Http2DownstreamConnection::init_request_body_buf()
{
  int rv;
  if(request_body_buf_) {
    rv = evbuffer_drain(request_body_buf_,
                        evbuffer_get_length(request_body_buf_));
    if(rv != 0) {
      return -1;
    }
  } else {
    request_body_buf_ = evbuffer_new();
    if(request_body_buf_ == nullptr) {
      return -1;
    }
  }
  return 0;
}

int Http2DownstreamConnection::attach_downstream(Downstream *downstream)
{
  if(LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Attaching to DOWNSTREAM:" << downstream;
  }
  if(init_request_body_buf() == -1) {
    return -1;
  }
  http2session_->add_downstream_connection(this);
  if(http2session_->get_state() == Http2Session::DISCONNECTED) {
    http2session_->notify();
  }
  downstream->set_downstream_connection(this);
  downstream_ = downstream;
  return 0;
}

void Http2DownstreamConnection::detach_downstream(Downstream *downstream)
{
  if(LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Detaching from DOWNSTREAM:" << downstream;
  }
  if(submit_rst_stream(downstream) == 0) {
    http2session_->notify();
  }
  downstream->set_downstream_connection(nullptr);
  downstream_ = nullptr;

  client_handler_->pool_downstream_connection(this);
}

int Http2DownstreamConnection::submit_rst_stream(Downstream *downstream)
{
  int rv = -1;
  if(http2session_->get_state() == Http2Session::CONNECTED &&
     downstream->get_downstream_stream_id() != -1) {
    switch(downstream->get_response_state()) {
    case Downstream::MSG_RESET:
    case Downstream::MSG_COMPLETE:
      break;
    default:
      if(LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "Submit RST_STREAM for DOWNSTREAM:"
                          << downstream << ", stream_id="
                          << downstream->get_downstream_stream_id();
      }
      rv = http2session_->submit_rst_stream
        (downstream->get_downstream_stream_id(),
         NGHTTP2_INTERNAL_ERROR);
    }
  }
  return rv;
}

namespace {
ssize_t http2_data_read_callback(nghttp2_session *session,
                                 int32_t stream_id,
                                 uint8_t *buf, size_t length,
                                 int *eof,
                                 nghttp2_data_source *source,
                                 void *user_data)
{
  auto sd = static_cast<StreamData*>
    (nghttp2_session_get_stream_user_data(session, stream_id));
  if(!sd || !sd->dconn) {
    return NGHTTP2_ERR_DEFERRED;
  }
  auto dconn = static_cast<Http2DownstreamConnection*>(source->ptr);
  auto downstream = dconn->get_downstream();
  if(!downstream) {
    // In this case, RST_STREAM should have been issued. But depending
    // on the priority, DATA frame may come first.
    return NGHTTP2_ERR_DEFERRED;
  }
  auto body = dconn->get_request_body_buf();
  int nread = 0;
  for(;;) {
    nread = evbuffer_remove(body, buf, length);
    if(nread == -1) {
      DCLOG(FATAL, dconn) << "evbuffer_remove() failed";
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if(nread == 0) {
      if(downstream->get_request_state() == Downstream::MSG_COMPLETE) {
        if(!downstream->get_upgrade_request() ||
           (downstream->get_response_state() == Downstream::HEADER_COMPLETE &&
            !downstream->get_upgraded())) {
          *eof = 1;
        } else {
          return NGHTTP2_ERR_DEFERRED;
        }
        break;
      } else {
        // This is important because it will handle flow control
        // stuff.
        if(downstream->get_upstream()->resume_read(SHRPX_NO_BUFFER,
                                                   downstream) != 0) {
          // In this case, downstream may be deleted.
          return NGHTTP2_ERR_CALLBACK_FAILURE;
        }
        // Check dconn is still alive because Upstream::resume_read()
        // may delete downstream which will delete dconn.
        if(sd->dconn == nullptr) {
          return NGHTTP2_ERR_DEFERRED;
        }
        if(evbuffer_get_length(body) == 0) {
          // Check get_request_state() == MSG_COMPLETE just in case
          if(downstream->get_request_state() == Downstream::MSG_COMPLETE) {
            *eof = 1;
            break;
          }
          return NGHTTP2_ERR_DEFERRED;
        }
      }
    } else {
      // Send WINDOW_UPDATE before buffer is empty to avoid delay
      // because of RTT.
      if(!downstream->get_output_buffer_full() &&
         downstream->get_upstream()->resume_read(SHRPX_NO_BUFFER,
                                                 downstream) == -1) {
        // In this case, downstream may be deleted.
        return NGHTTP2_ERR_DEFERRED;
      }
      break;
    }
  }
  return nread;
}
} // namespace

int Http2DownstreamConnection::push_request_headers()
{
  int rv;
  if(http2session_->get_state() != Http2Session::CONNECTED) {
    // The HTTP2 session to the backend has not been established.
    // This function will be called again just after it is
    // established.
    return 0;
  }
  if(!downstream_) {
    return 0;
  }
  size_t nheader = downstream_->get_request_headers().size();
  if(!get_config()->http2_no_cookie_crumbling) {
    downstream_->crumble_request_cookie();
  }
  downstream_->normalize_request_headers();
  downstream_->concat_norm_request_headers();
  auto end_headers = std::end(downstream_->get_request_headers());

  // 6 means:
  // 1. :method
  // 2. :scheme
  // 3. :path
  // 4. :authority (optional)
  // 5. via (optional)
  // 6. x-forwarded-for (optional)
  auto nva = std::vector<nghttp2_nv>();
  nva.reserve(nheader + 6);
  std::string via_value;
  std::string xff_value;
  std::string scheme, authority, path, query;
  // To reconstruct HTTP/1 status line and headers, proxy should
  // preserve host header field. See draft-09 section 8.1.3.1.
  if(downstream_->get_request_method() == "CONNECT") {
    // The upstream may be HTTP/2 or HTTP/1
    if(!downstream_->get_request_http2_authority().empty()) {
      nva.push_back(http2::make_nv_ls
                    (":authority",
                     downstream_->get_request_http2_authority()));
    } else {
      nva.push_back(http2::make_nv_ls(":authority",
                                      downstream_->get_request_path()));
    }
  } else if(!downstream_->get_request_http2_scheme().empty()) {
    // Here the upstream is HTTP/2
    nva.push_back(http2::make_nv_ls(":scheme",
                                    downstream_->get_request_http2_scheme()));
    nva.push_back(http2::make_nv_ls(":path",
                                    downstream_->get_request_path()));
    if(!downstream_->get_request_http2_authority().empty()) {
      nva.push_back(http2::make_nv_ls
                    (":authority",
                     downstream_->get_request_http2_authority()));
    } else if(downstream_->get_norm_request_header("host") == end_headers) {
      if(LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "host header field missing";
      }
      return -1;
    }
  } else {
    // The upstream is HTTP/1
    http_parser_url u;
    const char *url = downstream_->get_request_path().c_str();
    memset(&u, 0, sizeof(u));
    rv = http_parser_parse_url(url,
                               downstream_->get_request_path().size(),
                               0, &u);
    if(rv == 0) {
      http2::copy_url_component(scheme, &u, UF_SCHEMA, url);
      http2::copy_url_component(authority, &u, UF_HOST, url);
      http2::copy_url_component(path, &u, UF_PATH, url);
      http2::copy_url_component(query, &u, UF_QUERY, url);
      if(path.empty()) {
        path = "/";
      }
      if(!query.empty()) {
        path += "?";
        path += query;
      }
    }
    if(scheme.empty()) {
      // The default scheme is http. For HTTP2 upstream, the path must
      // be absolute URI, so scheme should be provided.
      nva.push_back(http2::make_nv_ll(":scheme", "http"));
    } else {
      nva.push_back(http2::make_nv_ls(":scheme", scheme));
    }
    if(path.empty()) {
      nva.push_back(http2::make_nv_ls(":path",
                                      downstream_->get_request_path()));
    } else {
      nva.push_back(http2::make_nv_ls(":path", path));
    }
    if(!authority.empty()) {
      // TODO properly check IPv6 numeric address
      if(authority.find(":") != std::string::npos) {
        authority = "[" + authority;
        authority += "]";
      }
      if(u.field_set & (1 << UF_PORT)) {
        authority += ":";
        authority += util::utos(u.port);
      }
      nva.push_back(http2::make_nv_ls(":authority", authority));
    } else if(downstream_->get_norm_request_header("host") == end_headers) {
      if(LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "host header field missing";
      }
      return -1;
    }
  }

  nva.push_back(http2::make_nv_ls(":method",
                                  downstream_->get_request_method()));

  http2::copy_norm_headers_to_nva(nva, downstream_->get_request_headers());

  bool content_length = false;
  if(downstream_->get_norm_request_header("content-length") != end_headers) {
    content_length = true;
  }

  bool chunked_encoding = false;
  auto transfer_encoding =
    downstream_->get_norm_request_header("transfer-encoding");
  if(transfer_encoding != end_headers &&
     util::strieq((*transfer_encoding).second.c_str(), "chunked")) {
    chunked_encoding = true;
  }

  auto xff = downstream_->get_norm_request_header("x-forwarded-for");
  if(get_config()->add_x_forwarded_for) {
    if(xff != end_headers) {
      xff_value = (*xff).second;
      xff_value += ", ";
    }
    xff_value += downstream_->get_upstream()->get_client_handler()->
      get_ipaddr();
    nva.push_back(http2::make_nv_ls("x-forwarded-for", xff_value));
  } else if(xff != end_headers) {
    nva.push_back(http2::make_nv_ls("x-forwarded-for", (*xff).second));
  }

  auto via = downstream_->get_norm_request_header("via");
  if(get_config()->no_via) {
    if(via != end_headers) {
      nva.push_back(http2::make_nv_ls("via", (*via).second));
    }
  } else {
    if(via != end_headers) {
      via_value = (*via).second;
      via_value += ", ";
    }
    via_value += http::create_via_header_value
      (downstream_->get_request_major(), downstream_->get_request_minor());
    nva.push_back(http2::make_nv_ls("via", via_value));
  }

  if(LOG_ENABLED(INFO)) {
    std::stringstream ss;
    for(auto& nv : nva) {
      ss << TTY_HTTP_HD;
      ss.write(reinterpret_cast<const char*>(nv.name), nv.namelen);
      ss << TTY_RST << ": ";
      ss.write(reinterpret_cast<const char*>(nv.value), nv.valuelen);
      ss << "\n";
    }
    DCLOG(INFO, this) << "HTTP request headers\n" << ss.str();
  }

  if(downstream_->get_request_method() == "CONNECT" ||
     chunked_encoding || content_length) {
    // Request-body is expected.
    nghttp2_data_provider data_prd;
    data_prd.source.ptr = this;
    data_prd.read_callback = http2_data_read_callback;
    rv = http2session_->submit_request(this, downstream_->get_priorty(),
                                       nva.data(), nva.size(), &data_prd);
  } else {
    rv = http2session_->submit_request(this, downstream_->get_priorty(),
                                       nva.data(), nva.size(), nullptr);
  }
  if(rv != 0) {
    DCLOG(FATAL, this) << "nghttp2_submit_request() failed";
    return -1;
  }
  http2session_->notify();
  return 0;
}

int Http2DownstreamConnection::push_upload_data_chunk(const uint8_t *data,
                                                     size_t datalen)
{
  int rv = evbuffer_add(request_body_buf_, data, datalen);
  if(rv != 0) {
    DCLOG(FATAL, this) << "evbuffer_add() failed";
    return -1;
  }
  if(downstream_->get_downstream_stream_id() != -1) {
    rv = http2session_->resume_data(this);
    if(rv != 0) {
      return -1;
    }
    http2session_->notify();
  }
  return 0;
}

int Http2DownstreamConnection::end_upload_data()
{
  int rv;
  if(downstream_->get_downstream_stream_id() != -1) {
    rv = http2session_->resume_data(this);
    if(rv != 0) {
      return -1;
    }
    http2session_->notify();
  }
  return 0;
}

int Http2DownstreamConnection::resume_read(IOCtrlReason reason)
{
  int rv1 = 0, rv2 = 0;
  if(http2session_->get_state() == Http2Session::CONNECTED &&
     http2session_->get_flow_control()) {
    int32_t window_size_increment;
    window_size_increment = http2::determine_window_update_transmission
      (http2session_->get_session(), 0);
    if(window_size_increment != -1) {
      rv1 = http2session_->submit_window_update(nullptr, window_size_increment);
      if(rv1 == 0) {
        http2session_->notify();
      }
    }
    if(downstream_ && downstream_->get_downstream_stream_id() != -1) {
      window_size_increment = http2::determine_window_update_transmission
        (http2session_->get_session(), downstream_->get_downstream_stream_id());
      if(window_size_increment != -1) {
        rv2 = http2session_->submit_window_update(this, window_size_increment);
        if(rv2 == 0) {
          http2session_->notify();
        }
      }
    }
  }
  if(rv1 == 0 && rv2 == 0) {
    return 0;
  }
  DLOG(WARNING, this) << "Sending WINDOW_UPDATE failed";
  return -1;
}

int Http2DownstreamConnection::on_read()
{
  return 0;
}

int Http2DownstreamConnection::on_write()
{
  return 0;
}

evbuffer* Http2DownstreamConnection::get_request_body_buf() const
{
  return request_body_buf_;
}

void Http2DownstreamConnection::attach_stream_data(StreamData *sd)
{
  // It is possible sd->dconn is not NULL. sd is detached when
  // on_stream_close_callback. Before that, after MSG_COMPLETE is set
  // to Downstream::set_response_state(), upstream's readcb is called
  // and execution path eventually could reach here. Since the
  // response was already handled, we just detach sd.
  detach_stream_data();
  sd_ = sd;
  sd_->dconn = this;
}

StreamData* Http2DownstreamConnection::detach_stream_data()
{
  if(sd_) {
    auto sd = sd_;
    sd_ = nullptr;
    sd->dconn = nullptr;
    return sd;
  } else {
    return nullptr;
  }
}

bool Http2DownstreamConnection::get_output_buffer_full()
{
  if(request_body_buf_) {
    return http2session_->get_outbuf_length() +
      evbuffer_get_length(request_body_buf_) >= Http2Session::OUTBUF_MAX_THRES;
  } else {
    return false;
  }
}

int Http2DownstreamConnection::on_priority_change(int32_t pri)
{
  int rv;
  if(downstream_->get_priorty() == pri) {
    return 0;
  }
  downstream_->set_priority(pri);
  if(http2session_->get_state() != Http2Session::CONNECTED) {
    return 0;
  }
  rv = http2session_->submit_priority(this, pri);
  if(rv != 0) {
    DLOG(FATAL, this) << "nghttp2_submit_priority() failed";
    return -1;
  }
  http2session_->notify();
  return 0;
}

} // namespace shrpx
