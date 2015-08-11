/*
 * nghttp2 - HTTP/2 C Library
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
#include "shrpx_worker_config.h"
#include "http2.h"
#include "util.h"

using namespace nghttp2;

namespace shrpx {

Http2DownstreamConnection::Http2DownstreamConnection(
    DownstreamConnectionPool *dconn_pool, Http2Session *http2session)
    : DownstreamConnection(dconn_pool), http2session_(http2session),
      request_body_buf_(nullptr), sd_(nullptr) {}

Http2DownstreamConnection::~Http2DownstreamConnection() {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Deleting";
  }
  if (request_body_buf_) {
    evbuffer_free(request_body_buf_);
  }
  if (downstream_) {
    downstream_->disable_downstream_rtimer();
    downstream_->disable_downstream_wtimer();

    uint32_t error_code;
    if (downstream_->get_request_state() == Downstream::STREAM_CLOSED &&
        downstream_->get_upgraded()) {
      // For upgraded connection, send NO_ERROR.  Should we consider
      // request states other than Downstream::STREAM_CLOSED ?
      error_code = NGHTTP2_NO_ERROR;
    } else {
      error_code = NGHTTP2_INTERNAL_ERROR;
    }

    if (LOG_ENABLED(INFO)) {
      DCLOG(INFO, this) << "Submit RST_STREAM for DOWNSTREAM:" << downstream_
                        << ", stream_id="
                        << downstream_->get_downstream_stream_id()
                        << ", error_code=" << error_code;
    }

    if (submit_rst_stream(downstream_, error_code) == 0) {
      http2session_->notify();
    }

    if (downstream_->get_downstream_stream_id() != -1) {
      http2session_->consume(downstream_->get_downstream_stream_id(),
                             downstream_->get_response_datalen());

      downstream_->reset_response_datalen();

      http2session_->notify();
    }
  }
  http2session_->remove_downstream_connection(this);
  // Downstream and DownstreamConnection may be deleted
  // asynchronously.
  if (downstream_) {
    downstream_->release_downstream_connection();
  }
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Deleted";
  }
}

int Http2DownstreamConnection::init_request_body_buf() {
  int rv;
  if (request_body_buf_) {
    rv = evbuffer_drain(request_body_buf_,
                        evbuffer_get_length(request_body_buf_));
    if (rv != 0) {
      return -1;
    }
  } else {
    request_body_buf_ = evbuffer_new();
    if (request_body_buf_ == nullptr) {
      return -1;
    }
  }
  return 0;
}

int Http2DownstreamConnection::attach_downstream(Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Attaching to DOWNSTREAM:" << downstream;
  }
  if (init_request_body_buf() == -1) {
    return -1;
  }
  http2session_->add_downstream_connection(this);
  if (http2session_->get_state() == Http2Session::DISCONNECTED) {
    http2session_->notify();
  }

  downstream_ = downstream;

  downstream_->init_downstream_timer();

  return 0;
}

void Http2DownstreamConnection::detach_downstream(Downstream *downstream) {
  if (LOG_ENABLED(INFO)) {
    DCLOG(INFO, this) << "Detaching from DOWNSTREAM:" << downstream;
  }
  if (submit_rst_stream(downstream) == 0) {
    http2session_->notify();
  }

  if (downstream_->get_downstream_stream_id() != -1) {
    http2session_->consume(downstream_->get_downstream_stream_id(),
                           downstream_->get_response_datalen());

    downstream_->reset_response_datalen();

    http2session_->notify();
  }

  downstream->disable_downstream_rtimer();
  downstream->disable_downstream_wtimer();
  downstream_ = nullptr;
}

int Http2DownstreamConnection::submit_rst_stream(Downstream *downstream,
                                                 uint32_t error_code) {
  int rv = -1;
  if (http2session_->get_state() == Http2Session::CONNECTED &&
      downstream->get_downstream_stream_id() != -1) {
    switch (downstream->get_response_state()) {
    case Downstream::MSG_RESET:
    case Downstream::MSG_COMPLETE:
      break;
    default:
      if (LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "Submit RST_STREAM for DOWNSTREAM:" << downstream
                          << ", stream_id="
                          << downstream->get_downstream_stream_id();
      }
      rv = http2session_->submit_rst_stream(
          downstream->get_downstream_stream_id(), error_code);
    }
  }
  return rv;
}

namespace {
ssize_t http2_data_read_callback(nghttp2_session *session, int32_t stream_id,
                                 uint8_t *buf, size_t length,
                                 uint32_t *data_flags,
                                 nghttp2_data_source *source, void *user_data) {
  auto sd = static_cast<StreamData *>(
      nghttp2_session_get_stream_user_data(session, stream_id));
  if (!sd || !sd->dconn) {
    return NGHTTP2_ERR_DEFERRED;
  }
  auto dconn = static_cast<Http2DownstreamConnection *>(source->ptr);
  auto downstream = dconn->get_downstream();
  if (!downstream) {
    // In this case, RST_STREAM should have been issued. But depending
    // on the priority, DATA frame may come first.
    return NGHTTP2_ERR_DEFERRED;
  }
  auto body = dconn->get_request_body_buf();

  auto nread = evbuffer_remove(body, buf, length);
  if (nread == -1) {
    DCLOG(FATAL, dconn) << "evbuffer_remove() failed";
    return NGHTTP2_ERR_CALLBACK_FAILURE;
  }

  if (nread > 0) {
    // This is important because it will handle flow control
    // stuff.
    if (downstream->get_upstream()->resume_read(SHRPX_NO_BUFFER, downstream,
                                                nread) != 0) {
      // In this case, downstream may be deleted.
      return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    // Check dconn is still alive because Upstream::resume_read()
    // may delete downstream which will delete dconn.
    if (sd->dconn == nullptr) {
      return NGHTTP2_ERR_DEFERRED;
    }
  }

  if (evbuffer_get_length(body) == 0 &&
      downstream->get_request_state() == Downstream::MSG_COMPLETE &&
      // If connection is upgraded, don't set EOF flag, since HTTP/1
      // will set MSG_COMPLETE to request state after upgrade response
      // header is seen.
      (!downstream->get_upgrade_request() ||
       (downstream->get_response_state() == Downstream::HEADER_COMPLETE &&
        !downstream->get_upgraded()))) {

    *data_flags |= NGHTTP2_DATA_FLAG_EOF;
  }

  if (evbuffer_get_length(body) > 0) {
    downstream->reset_downstream_wtimer();
  } else {
    downstream->disable_downstream_wtimer();
  }

  if (nread == 0 && (*data_flags & NGHTTP2_DATA_FLAG_EOF) == 0) {
    downstream->disable_downstream_wtimer();

    return NGHTTP2_ERR_DEFERRED;
  }

  return nread;
}
} // namespace

int Http2DownstreamConnection::push_request_headers() {
  int rv;
  if (http2session_->get_state() != Http2Session::CONNECTED) {
    // The HTTP2 session to the backend has not been established.
    // This function will be called again just after it is
    // established.
    return 0;
  }
  if (!downstream_) {
    return 0;
  }
  size_t nheader = downstream_->get_request_headers().size();
  if (!get_config()->http2_no_cookie_crumbling) {
    downstream_->crumble_request_cookie();
  }

  assert(downstream_->get_request_headers_normalized());

  auto end_headers = std::end(downstream_->get_request_headers());

  // 7 means:
  // 1. :method
  // 2. :scheme
  // 3. :path
  // 4. :authority (optional)
  // 5. via (optional)
  // 6. x-forwarded-for (optional)
  // 7. x-forwarded-proto (optional)
  auto nva = std::vector<nghttp2_nv>();
  nva.reserve(nheader + 7);
  std::string via_value;
  std::string xff_value;
  std::string scheme, authority, path, query;
  // To reconstruct HTTP/1 status line and headers, proxy should
  // preserve host header field. See draft-09 section 8.1.3.1.
  if (downstream_->get_request_method() == "CONNECT") {
    // The upstream may be HTTP/2 or HTTP/1
    if (!downstream_->get_request_http2_authority().empty()) {
      nva.push_back(http2::make_nv_ls(
          ":authority", downstream_->get_request_http2_authority()));
    } else {
      nva.push_back(
          http2::make_nv_ls(":authority", downstream_->get_request_path()));
    }
  } else if (!downstream_->get_request_http2_scheme().empty()) {
    // Here the upstream is HTTP/2
    nva.push_back(
        http2::make_nv_ls(":scheme", downstream_->get_request_http2_scheme()));
    nva.push_back(http2::make_nv_ls(":path", downstream_->get_request_path()));
    if (!downstream_->get_request_http2_authority().empty()) {
      nva.push_back(http2::make_nv_ls(
          ":authority", downstream_->get_request_http2_authority()));
    } else if (downstream_->get_norm_request_header("host") == end_headers) {
      if (LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "host header field missing";
      }
      return -1;
    }
  } else {
    // The upstream is HTTP/1
    http_parser_url u;
    const char *url = downstream_->get_request_path().c_str();
    memset(&u, 0, sizeof(u));
    rv = http_parser_parse_url(url, downstream_->get_request_path().size(), 0,
                               &u);
    if (rv == 0) {
      http2::copy_url_component(scheme, &u, UF_SCHEMA, url);
      http2::copy_url_component(authority, &u, UF_HOST, url);
      http2::copy_url_component(path, &u, UF_PATH, url);
      http2::copy_url_component(query, &u, UF_QUERY, url);
      if (path.empty()) {
        if (!authority.empty() &&
            downstream_->get_request_method() == "OPTIONS") {
          path = "*";
        } else {
          path = "/";
        }
      }
      if (!query.empty()) {
        path += "?";
        path += query;
      }
    }
    if (scheme.empty()) {
      if (client_handler_->get_ssl()) {
        nva.push_back(http2::make_nv_ll(":scheme", "https"));
      } else {
        nva.push_back(http2::make_nv_ll(":scheme", "http"));
      }
    } else {
      nva.push_back(http2::make_nv_ls(":scheme", scheme));
    }
    if (path.empty()) {
      nva.push_back(
          http2::make_nv_ls(":path", downstream_->get_request_path()));
    } else {
      nva.push_back(http2::make_nv_ls(":path", path));
    }
    if (!authority.empty()) {
      // TODO properly check IPv6 numeric address
      if (authority.find(":") != std::string::npos) {
        authority = "[" + authority;
        authority += "]";
      }
      if (u.field_set & (1 << UF_PORT)) {
        authority += ":";
        authority += util::utos(u.port);
      }
      nva.push_back(http2::make_nv_ls(":authority", authority));
    } else if (downstream_->get_norm_request_header("host") == end_headers) {
      if (LOG_ENABLED(INFO)) {
        DCLOG(INFO, this) << "host header field missing";
      }
      return -1;
    }
  }

  nva.push_back(
      http2::make_nv_ls(":method", downstream_->get_request_method()));

  http2::copy_norm_headers_to_nva(nva, downstream_->get_request_headers());

  bool chunked_encoding = false;
  auto transfer_encoding =
      downstream_->get_norm_request_header("transfer-encoding");
  if (transfer_encoding != end_headers &&
      util::strieq((*transfer_encoding).value.c_str(), "chunked")) {
    chunked_encoding = true;
  }

  auto xff = downstream_->get_norm_request_header("x-forwarded-for");
  if (get_config()->add_x_forwarded_for) {
    if (xff != end_headers && !get_config()->strip_incoming_x_forwarded_for) {
      xff_value = (*xff).value;
      xff_value += ", ";
    }
    xff_value +=
        downstream_->get_upstream()->get_client_handler()->get_ipaddr();
    nva.push_back(http2::make_nv_ls("x-forwarded-for", xff_value));
  } else if (xff != end_headers &&
             !get_config()->strip_incoming_x_forwarded_for) {
    nva.push_back(http2::make_nv_ls("x-forwarded-for", (*xff).value));
  }

  if (!get_config()->http2_proxy && !get_config()->client_proxy &&
      downstream_->get_request_method() != "CONNECT") {
    // We use same protocol with :scheme header field
    if (scheme.empty()) {
      if (client_handler_->get_ssl()) {
        nva.push_back(http2::make_nv_ll("x-forwarded-proto", "https"));
      } else {
        nva.push_back(http2::make_nv_ll("x-forwarded-proto", "http"));
      }
    } else {
      nva.push_back(http2::make_nv_ls("x-forwarded-proto", scheme));
    }
  }

  auto via = downstream_->get_norm_request_header("via");
  if (get_config()->no_via) {
    if (via != end_headers) {
      nva.push_back(http2::make_nv_ls("via", (*via).value));
    }
  } else {
    if (via != end_headers) {
      via_value = (*via).value;
      via_value += ", ";
    }
    via_value += http::create_via_header_value(
        downstream_->get_request_major(), downstream_->get_request_minor());
    nva.push_back(http2::make_nv_ls("via", via_value));
  }

  if (LOG_ENABLED(INFO)) {
    std::stringstream ss;
    for (auto &nv : nva) {
      ss << TTY_HTTP_HD;
      ss.write(reinterpret_cast<const char *>(nv.name), nv.namelen);
      ss << TTY_RST << ": ";
      ss.write(reinterpret_cast<const char *>(nv.value), nv.valuelen);
      ss << "\n";
    }
    DCLOG(INFO, this) << "HTTP request headers\n" << ss.str();
  }

  auto content_length =
      downstream_->get_norm_request_header("content-length") != end_headers;

  if (downstream_->get_request_method() == "CONNECT" || chunked_encoding ||
      content_length || downstream_->get_request_http2_expect_body()) {
    // Request-body is expected.
    nghttp2_data_provider data_prd;
    data_prd.source.ptr = this;
    data_prd.read_callback = http2_data_read_callback;
    rv = http2session_->submit_request(this, downstream_->get_priority(),
                                       nva.data(), nva.size(), &data_prd);
  } else {
    rv = http2session_->submit_request(this, downstream_->get_priority(),
                                       nva.data(), nva.size(), nullptr);
  }
  if (rv != 0) {
    DCLOG(FATAL, this) << "nghttp2_submit_request() failed";
    return -1;
  }

  downstream_->reset_downstream_wtimer();

  http2session_->notify();
  return 0;
}

int Http2DownstreamConnection::push_upload_data_chunk(const uint8_t *data,
                                                      size_t datalen) {
  int rv = evbuffer_add(request_body_buf_, data, datalen);
  if (rv != 0) {
    DCLOG(FATAL, this) << "evbuffer_add() failed";
    return -1;
  }
  if (downstream_->get_downstream_stream_id() != -1) {
    rv = http2session_->resume_data(this);
    if (rv != 0) {
      return -1;
    }

    downstream_->ensure_downstream_wtimer();

    http2session_->notify();
  }
  return 0;
}

int Http2DownstreamConnection::end_upload_data() {
  int rv;
  if (downstream_->get_downstream_stream_id() != -1) {
    rv = http2session_->resume_data(this);
    if (rv != 0) {
      return -1;
    }

    downstream_->ensure_downstream_wtimer();

    http2session_->notify();
  }
  return 0;
}

int Http2DownstreamConnection::resume_read(IOCtrlReason reason,
                                           size_t consumed) {
  int rv;

  if (http2session_->get_state() != Http2Session::CONNECTED ||
      !http2session_->get_flow_control()) {
    return 0;
  }

  if (!downstream_ || downstream_->get_downstream_stream_id() == -1) {
    return 0;
  }

  if (consumed > 0) {
    assert(downstream_->get_response_datalen() >= consumed);

    rv = http2session_->consume(downstream_->get_downstream_stream_id(),
                                consumed);

    if (rv != 0) {
      return -1;
    }

    downstream_->dec_response_datalen(consumed);

    http2session_->notify();
  }

  return 0;
}

int Http2DownstreamConnection::on_read() { return 0; }

int Http2DownstreamConnection::on_write() { return 0; }

evbuffer *Http2DownstreamConnection::get_request_body_buf() const {
  return request_body_buf_;
}

void Http2DownstreamConnection::attach_stream_data(StreamData *sd) {
  // It is possible sd->dconn is not NULL. sd is detached when
  // on_stream_close_callback. Before that, after MSG_COMPLETE is set
  // to Downstream::set_response_state(), upstream's readcb is called
  // and execution path eventually could reach here. Since the
  // response was already handled, we just detach sd.
  detach_stream_data();
  sd_ = sd;
  sd_->dconn = this;
}

StreamData *Http2DownstreamConnection::detach_stream_data() {
  if (sd_) {
    auto sd = sd_;
    sd_ = nullptr;
    sd->dconn = nullptr;
    return sd;
  } else {
    return nullptr;
  }
}

bool Http2DownstreamConnection::get_output_buffer_full() {
  if (request_body_buf_) {
    return evbuffer_get_length(request_body_buf_) >=
           Http2Session::OUTBUF_MAX_THRES;
  } else {
    return false;
  }
}

int Http2DownstreamConnection::on_priority_change(int32_t pri) {
  int rv;
  if (downstream_->get_priority() == pri) {
    return 0;
  }
  downstream_->set_priority(pri);
  if (http2session_->get_state() != Http2Session::CONNECTED) {
    return 0;
  }
  rv = http2session_->submit_priority(this, pri);
  if (rv != 0) {
    DLOG(FATAL, this) << "nghttp2_submit_priority() failed";
    return -1;
  }
  http2session_->notify();
  return 0;
}

int Http2DownstreamConnection::on_timeout() {
  if (!downstream_) {
    return 0;
  }

  return submit_rst_stream(downstream_, NGHTTP2_NO_ERROR);
}

} // namespace shrpx
