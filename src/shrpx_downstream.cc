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
#include "shrpx_downstream.h"

#include <cassert>

#include "http-parser/http_parser.h"

#include "shrpx_upstream.h"
#include "shrpx_client_handler.h"
#include "shrpx_config.h"
#include "shrpx_error.h"
#include "shrpx_downstream_connection.h"
#include "util.h"
#include "http2.h"

namespace shrpx {

Downstream::Downstream(Upstream *upstream, int32_t stream_id, int32_t priority)
  : request_bodylen_(0),
    response_bodylen_(0),
    upstream_(upstream),
    response_body_buf_(nullptr),
    upstream_rtimerev_(nullptr),
    upstream_wtimerev_(nullptr),
    downstream_rtimerev_(nullptr),
    downstream_wtimerev_(nullptr),
    request_headers_sum_(0),
    response_headers_sum_(0),
    request_datalen_(0),
    response_datalen_(0),
    stream_id_(stream_id),
    priority_(priority),
    downstream_stream_id_(-1),
    response_rst_stream_error_code_(NGHTTP2_NO_ERROR),
    request_state_(INITIAL),
    request_major_(1),
    request_minor_(1),
    response_state_(INITIAL),
    response_http_status_(0),
    response_major_(1),
    response_minor_(1),
    upgrade_request_(false),
    upgraded_(false),
    http2_upgrade_seen_(false),
    http2_settings_seen_(false),
    chunked_request_(false),
    request_connection_close_(false),
    request_header_key_prev_(false),
    request_http2_expect_body_(false),
    chunked_response_(false),
    response_connection_close_(false),
    response_header_key_prev_(false),
    expect_final_response_(false)
{}

Downstream::~Downstream()
{
  if(LOG_ENABLED(INFO)) {
    DLOG(INFO, this) << "Deleting";
  }
  if(response_body_buf_) {
    // Passing NULL to evbuffer_free() causes segmentation fault.
    evbuffer_free(response_body_buf_);
  }
  if(upstream_rtimerev_) {
    event_free(upstream_rtimerev_);
  }
  if(upstream_wtimerev_) {
    event_free(upstream_wtimerev_);
  }
  if(downstream_rtimerev_) {
    event_free(downstream_rtimerev_);
  }
  if(downstream_wtimerev_) {
    event_free(downstream_wtimerev_);
  }
  if(LOG_ENABLED(INFO)) {
    DLOG(INFO, this) << "Deleted";
  }
}

int Downstream::attach_downstream_connection
(std::unique_ptr<DownstreamConnection> dconn)
{
  if(dconn->attach_downstream(this) != 0) {
    return -1;
  }

  dconn_ = std::move(dconn);

  return 0;
}

void Downstream::detach_downstream_connection()
{
  if(!dconn_) {
    return;
  }

  dconn_->detach_downstream(this);

  auto handler = dconn_->get_client_handler();

  handler->pool_downstream_connection
    (std::unique_ptr<DownstreamConnection>(dconn_.release()));
}

void Downstream::release_downstream_connection()
{
  dconn_.release();
}

DownstreamConnection* Downstream::get_downstream_connection()
{
  return dconn_.get();
}

std::unique_ptr<DownstreamConnection> Downstream::pop_downstream_connection()
{
  return std::unique_ptr<DownstreamConnection>(dconn_.release());
}

void Downstream::pause_read(IOCtrlReason reason)
{
  if(dconn_) {
    dconn_->pause_read(reason);
  }
}

int Downstream::resume_read(IOCtrlReason reason, size_t consumed)
{
  if(dconn_) {
    return dconn_->resume_read(reason, consumed);
  }

  return 0;
}

void Downstream::force_resume_read()
{
  if(dconn_) {
    dconn_->force_resume_read();
  }
}

namespace {
Headers::const_iterator get_norm_header(const Headers& headers,
                                        const std::string& name)
{
  auto i = std::lower_bound(std::begin(headers), std::end(headers),
                            Header(name, ""), http2::name_less);
  if(i != std::end(headers) && (*i).name == name) {
    return i;
  }
  return std::end(headers);
}
} // namespace

namespace {
Headers::iterator get_norm_header(Headers& headers, const std::string& name)
{
  auto i = std::lower_bound(std::begin(headers), std::end(headers),
                            Header(name, ""), http2::name_less);
  if(i != std::end(headers) && (*i).name == name) {
    return i;
  }
  return std::end(headers);
}
} // namespace

const Headers& Downstream::get_request_headers() const
{
  return request_headers_;
}

void Downstream::assemble_request_cookie()
{
  std::string& cookie = assembled_request_cookie_;
  cookie = "";
  for(auto& kv : request_headers_) {
    if(util::strieq("cookie", kv.name.c_str())) {
      auto end = kv.value.find_last_not_of(" ;");
      if(end == std::string::npos) {
        cookie += kv.value;
      } else {
        cookie.append(std::begin(kv.value), std::begin(kv.value) + end + 1);
      }
      cookie += "; ";
    }
  }
  if(cookie.size() >= 2) {
    cookie.erase(cookie.size() - 2);
  }
}

void Downstream::crumble_request_cookie()
{
  Headers cookie_hdrs;
  for(auto& kv : request_headers_) {
    if(util::strieq("cookie", kv.name.c_str())) {
      size_t last = kv.value.size();
      size_t num = 0;
      std::string rep_cookie;

      for(size_t j = 0; j < last;) {
        j = kv.value.find_first_not_of("\t ;", j);
        if(j == std::string::npos) {
          break;
        }
        auto first = j;

        j = kv.value.find(';', j);
        if(j == std::string::npos) {
          j = last;
        }

        if(num == 0) {
          if(first == 0 && j == last) {
            break;
          }
          rep_cookie = kv.value.substr(first, j - first);
        } else {
          cookie_hdrs.push_back
            (Header("cookie", kv.value.substr(first, j - first),
                    kv.no_index));
        }
        ++num;
      }
      if(num > 0) {
        kv.value = std::move(rep_cookie);
      }
    }
  }
  request_headers_.insert(std::end(request_headers_),
                          std::make_move_iterator(std::begin(cookie_hdrs)),
                          std::make_move_iterator(std::end(cookie_hdrs)));
}

const std::string& Downstream::get_assembled_request_cookie() const
{
  return assembled_request_cookie_;
}

void Downstream::normalize_request_headers()
{
  http2::normalize_headers(request_headers_);
}

Headers::const_iterator Downstream::get_norm_request_header
(const std::string& name) const
{
  return get_norm_header(request_headers_, name);
}

void Downstream::add_request_header(std::string name, std::string value)
{
  request_header_key_prev_ = true;
  request_headers_sum_ += name.size() + value.size();
  request_headers_.emplace_back(std::move(name), std::move(value));
}

void Downstream::set_last_request_header_value(std::string value)
{
  request_header_key_prev_ = false;
  request_headers_sum_ += value.size();
  Headers::value_type &item = request_headers_.back();
  item.value = std::move(value);
}

void Downstream::split_add_request_header
(const uint8_t *name, size_t namelen,
 const uint8_t *value, size_t valuelen,
 bool no_index)
{
  request_headers_sum_ += namelen + valuelen;
  http2::add_header(request_headers_, name, namelen, value, valuelen,
                    no_index);
}

bool Downstream::get_request_header_key_prev() const
{
  return request_header_key_prev_;
}

void Downstream::append_last_request_header_key(const char *data, size_t len)
{
  assert(request_header_key_prev_);
  request_headers_sum_ += len;
  auto& item = request_headers_.back();
  item.name.append(data, len);
}

void Downstream::append_last_request_header_value(const char *data, size_t len)
{
  assert(!request_header_key_prev_);
  request_headers_sum_ += len;
  auto& item = request_headers_.back();
  item.value.append(data, len);
}

void Downstream::clear_request_headers()
{
  Headers().swap(request_headers_);
}

size_t Downstream::get_request_headers_sum() const
{
  return request_headers_sum_;
}

void Downstream::set_request_method(std::string method)
{
  request_method_ = std::move(method);
}

const std::string& Downstream::get_request_method() const
{
  return request_method_;
}

void Downstream::set_request_path(std::string path)
{
  request_path_ = std::move(path);
}

void Downstream::append_request_path(const char *data, size_t len)
{
  request_path_.append(data, len);
}

const std::string& Downstream::get_request_path() const
{
  return request_path_;
}

const std::string& Downstream::get_request_http2_scheme() const
{
  return request_http2_scheme_;
}

void Downstream::set_request_http2_scheme(std::string scheme)
{
  request_http2_scheme_ = std::move(scheme);
}

const std::string& Downstream::get_request_http2_authority() const
{
  return request_http2_authority_;
}

void Downstream::set_request_http2_authority(std::string authority)
{
  request_http2_authority_ = std::move(authority);
}

void Downstream::set_request_major(int major)
{
  request_major_ = major;
}

void Downstream::set_request_minor(int minor)
{
  request_minor_ = minor;
}

int Downstream::get_request_major() const
{
  return request_major_;
}

int Downstream::get_request_minor() const
{
  return request_minor_;
}

void Downstream::reset_upstream(Upstream* upstream)
{
  upstream_ = upstream;
  if(dconn_) {
    dconn_->on_upstream_change(upstream);
  }
}

Upstream* Downstream::get_upstream() const
{
  return upstream_;
}

void Downstream::set_stream_id(int32_t stream_id)
{
  stream_id_ = stream_id;
}

int32_t Downstream::get_stream_id() const
{
  return stream_id_;
}

void Downstream::set_request_state(int state)
{
  request_state_ = state;
}

int Downstream::get_request_state() const
{
  return request_state_;
}

bool Downstream::get_chunked_request() const
{
  return chunked_request_;
}

void Downstream::set_chunked_request(bool f)
{
  chunked_request_ = f;
}

bool Downstream::get_request_connection_close() const
{
  return request_connection_close_;
}

void Downstream::set_request_connection_close(bool f)
{
  request_connection_close_ = f;
}

void Downstream::set_request_user_agent(std::string user_agent)
{
  request_user_agent_ = std::move(user_agent);
}

const std::string& Downstream::get_request_user_agent() const
{
  return request_user_agent_;
}

bool Downstream::get_request_http2_expect_body() const
{
  return request_http2_expect_body_;
}

void Downstream::set_request_http2_expect_body(bool f)
{
  request_http2_expect_body_ = f;
}

bool Downstream::get_output_buffer_full()
{
  if(dconn_) {
    return dconn_->get_output_buffer_full();
  } else {
    return false;
  }
}

// Call this function after this object is attached to
// Downstream. Otherwise, the program will crash.
int Downstream::push_request_headers()
{
  if(!dconn_) {
    DLOG(INFO, this) << "dconn_ is NULL";
    return -1;
  }
  return dconn_->push_request_headers();
}

int Downstream::push_upload_data_chunk(const uint8_t *data, size_t datalen)
{
  // Assumes that request headers have already been pushed to output
  // buffer using push_request_headers().
  if(!dconn_) {
    DLOG(INFO, this) << "dconn_ is NULL";
    return -1;
  }
  request_bodylen_ += datalen;
  if(dconn_->push_upload_data_chunk(data, datalen) != 0) {
    return -1;
  }

  request_datalen_ += datalen;

  return 0;
}

int Downstream::end_upload_data()
{
  if(!dconn_) {
    DLOG(INFO, this) << "dconn_ is NULL";
    return -1;
  }
  return dconn_->end_upload_data();
}

const Headers& Downstream::get_response_headers() const
{
  return response_headers_;
}

void Downstream::normalize_response_headers()
{
  http2::normalize_headers(response_headers_);
}

Headers::const_iterator Downstream::get_norm_response_header
(const std::string& name) const
{
  return get_norm_header(response_headers_, name);
}

void Downstream::rewrite_norm_location_response_header
(const std::string& upstream_scheme,
 uint16_t upstream_port)
{
  auto hd = get_norm_header(response_headers_, "location");
  if(hd == std::end(response_headers_)) {
    return;
  }
  http_parser_url u;
  memset(&u, 0, sizeof(u));
  int rv = http_parser_parse_url((*hd).value.c_str(), (*hd).value.size(),
                                 0, &u);
  if(rv != 0) {
    return;
  }
  std::string new_uri;
  if(!request_http2_authority_.empty()) {
    new_uri = http2::rewrite_location_uri((*hd).value, u,
                                          request_http2_authority_,
                                          upstream_scheme, upstream_port);
  }
  if(new_uri.empty()) {
    auto host = get_norm_request_header("host");
    if(host == std::end(request_headers_)) {
      return;
    }
    new_uri = http2::rewrite_location_uri((*hd).value, u, (*host).value,
                                          upstream_scheme, upstream_port);
  }
  if(!new_uri.empty()) {
    (*hd).value = std::move(new_uri);
  }
}

void Downstream::add_response_header(std::string name, std::string value)
{
  response_header_key_prev_ = true;
  response_headers_sum_ += name.size() + value.size();
  response_headers_.emplace_back(std::move(name), std::move(value));
}

void Downstream::set_last_response_header_value(std::string value)
{
  response_header_key_prev_ = false;
  response_headers_sum_ += value.size();
  auto& item = response_headers_.back();
  item.value = std::move(value);
}

void Downstream::split_add_response_header
(const uint8_t *name, size_t namelen,
 const uint8_t *value, size_t valuelen,
 bool no_index)
{
  response_headers_sum_ += namelen + valuelen;
  http2::add_header(response_headers_, name, namelen, value, valuelen,
                    no_index);
}

bool Downstream::get_response_header_key_prev() const
{
  return response_header_key_prev_;
}

void Downstream::append_last_response_header_key(const char *data, size_t len)
{
  assert(response_header_key_prev_);
  response_headers_sum_ += len;
  auto& item = response_headers_.back();
  item.name.append(data, len);
}

void Downstream::append_last_response_header_value(const char *data,
                                                   size_t len)
{
  assert(!response_header_key_prev_);
  response_headers_sum_ += len;
  auto& item = response_headers_.back();
  item.value.append(data, len);
}

void Downstream::clear_response_headers()
{
  Headers().swap(response_headers_);
}

size_t Downstream::get_response_headers_sum() const
{
  return response_headers_sum_;
}

unsigned int Downstream::get_response_http_status() const
{
  return response_http_status_;
}

void Downstream::set_response_http_status(unsigned int status)
{
  response_http_status_ = status;
}

void Downstream::set_response_major(int major)
{
  response_major_ = major;
}

void Downstream::set_response_minor(int minor)
{
  response_minor_ = minor;
}

int Downstream::get_response_major() const
{
  return response_major_;
}

int Downstream::get_response_minor() const
{
  return response_minor_;
}

int Downstream::get_response_version() const
{
  return response_major_*100+response_minor_;
}

bool Downstream::get_chunked_response() const
{
  return chunked_response_;
}

void Downstream::set_chunked_response(bool f)
{
  chunked_response_ = f;
}

bool Downstream::get_response_connection_close() const
{
  return response_connection_close_;
}

void Downstream::set_response_connection_close(bool f)
{
  response_connection_close_ = f;
}

int Downstream::on_read()
{
  if(!dconn_) {
    DLOG(INFO, this) << "dconn_ is NULL";
    return -1;
  }
  return dconn_->on_read();
}

int Downstream::change_priority(int32_t pri)
{
  if(!dconn_) {
    DLOG(INFO, this) << "dconn_ is NULL";
    return -1;
  }
  return dconn_->on_priority_change(pri);
}

void Downstream::set_response_state(int state)
{
  response_state_ = state;
}

int Downstream::get_response_state() const
{
  return response_state_;
}

int Downstream::init_response_body_buf()
{
  if(!response_body_buf_) {
    response_body_buf_ = evbuffer_new();
    if(response_body_buf_ == nullptr) {
      DIE();
    }
  }
  return 0;
}

evbuffer* Downstream::get_response_body_buf()
{
  return response_body_buf_;
}

void Downstream::add_response_bodylen(size_t amount)
{
  response_bodylen_ += amount;
}

int64_t Downstream::get_response_bodylen() const
{
  return response_bodylen_;
}

void Downstream::set_priority(int32_t pri)
{
  priority_ = pri;
}

int32_t Downstream::get_priority() const
{
  return priority_;
}

void Downstream::check_upgrade_fulfilled()
{
  if(request_method_ == "CONNECT") {
    upgraded_ = 200 <= response_http_status_ && response_http_status_ < 300;

    return;
  }

  if(response_http_status_ == 101) {
    // TODO Do more strict checking for upgrade headers
    upgraded_ = upgrade_request_;

    return;
  }
}

void Downstream::inspect_http2_request()
{
  if(request_method_ == "CONNECT") {
    upgrade_request_ = true;
  }
}

void Downstream::inspect_http1_request()
{
  if(request_method_ == "CONNECT") {
    upgrade_request_ = true;
  }

  for(auto& hd : request_headers_) {
    if(!upgrade_request_ && util::strieq("upgrade", hd.name.c_str())) {
      // TODO Perform more strict checking for upgrade headers
      upgrade_request_ = true;

      if(util::streq(NGHTTP2_CLEARTEXT_PROTO_VERSION_ID,
                     hd.value.c_str(), hd.value.size())) {
        http2_upgrade_seen_ = true;
      }
    } else if(!http2_settings_seen_ &&
              util::strieq(hd.name.c_str(), "http2-settings")) {

      http2_settings_seen_ = true;
      http2_settings_ = hd.value;
    } else if(!chunked_request_ &&
              util::strieq(hd.name.c_str(), "transfer-encoding")) {
      if(util::strifind(hd.value.c_str(), "chunked")) {
        chunked_request_ = true;
      }
    }
  }
}

void Downstream::inspect_http1_response()
{
  for(auto& hd : response_headers_) {
    if(!chunked_response_ &&
       util::strieq(hd.name.c_str(), "transfer-encoding")) {
      if(util::strifind(hd.value.c_str(), "chunked")) {
        chunked_response_ = true;
      }
    }
  }
}

void Downstream::reset_response()
{
  response_http_status_ = 0;
  response_major_ = 1;
  response_minor_ = 1;
}

bool Downstream::get_non_final_response() const
{
  return response_http_status_ / 100 == 1;
}

bool Downstream::get_upgraded() const
{
  return upgraded_;
}

bool Downstream::get_upgrade_request() const
{
  return upgrade_request_;
}

bool Downstream::get_http2_upgrade_request() const
{
  return request_bodylen_ == 0 && http2_upgrade_seen_ && http2_settings_seen_;
}

const std::string& Downstream::get_http2_settings() const
{
  return http2_settings_;
}

void Downstream::set_downstream_stream_id(int32_t stream_id)
{
  downstream_stream_id_ = stream_id;
}

int32_t Downstream::get_downstream_stream_id() const
{
  return downstream_stream_id_;
}

uint32_t Downstream::get_response_rst_stream_error_code() const
{
  return response_rst_stream_error_code_;
}

void Downstream::set_response_rst_stream_error_code(uint32_t error_code)
{
  response_rst_stream_error_code_ = error_code;
}

void Downstream::set_expect_final_response(bool f)
{
  expect_final_response_ = f;
}

bool Downstream::get_expect_final_response() const
{
  return expect_final_response_;
}

size_t Downstream::get_request_datalen() const
{
  return request_datalen_;
}

void Downstream::dec_request_datalen(size_t len)
{
  assert(request_datalen_ >= len);
  request_datalen_ -= len;
}

void Downstream::reset_request_datalen()
{
  request_datalen_ = 0;
}

void Downstream::add_response_datalen(size_t len)
{
  response_datalen_ += len;
}

void Downstream::dec_response_datalen(size_t len)
{
  assert(response_datalen_ >= len);
  response_datalen_ -= len;
}

size_t Downstream::get_response_datalen() const
{
  return response_datalen_;
}

void Downstream::reset_response_datalen()
{
  response_datalen_ = 0;
}

bool Downstream::expect_response_body() const
{
  return request_method_ != "HEAD" &&
    response_http_status_ / 100 != 1 &&
    response_http_status_ != 304 &&
    response_http_status_ != 204;
}

namespace {
bool pseudo_header_allowed(const Headers& headers)
{
  if(headers.empty()) {
    return true;
  }

  return headers.back().name.c_str()[0] == ':';
}
} // namespace

bool Downstream::request_pseudo_header_allowed() const
{
  return pseudo_header_allowed(request_headers_);
}

bool Downstream::response_pseudo_header_allowed() const
{
  return pseudo_header_allowed(response_headers_);
}

namespace {
void upstream_timeoutcb(evutil_socket_t fd, short event, void *arg)
{
  auto downstream = static_cast<Downstream*>(arg);
  auto upstream = downstream->get_upstream();

  auto which = event == EV_READ ? "read" : "write";

  if(LOG_ENABLED(INFO)) {
    DLOG(INFO, downstream) << "upstream timeout stream_id="
                           << downstream->get_stream_id()
                           << " event=" << which;
  }

  downstream->disable_upstream_rtimer();
  downstream->disable_upstream_wtimer();

  upstream->on_timeout(downstream);
}
} // namespace

namespace {
void upstream_rtimeoutcb(evutil_socket_t fd, short event, void *arg)
{
  upstream_timeoutcb(fd, EV_READ, arg);
}
} // namespace

namespace {
void upstream_wtimeoutcb(evutil_socket_t fd, short event, void *arg)
{
  upstream_timeoutcb(fd, EV_WRITE, arg);
}
} // namespace

namespace {
event* init_timer(event_base *evbase, event_callback_fn cb, void *arg)
{
  auto timerev = evtimer_new(evbase, cb, arg);

  if(timerev == nullptr) {
    LOG(WARN) << "timer initialization failed";
    return nullptr;
  }

  return timerev;
}
} // namespace

void Downstream::init_upstream_timer()
{
  auto evbase = upstream_->get_client_handler()->get_evbase();

  if(get_config()->stream_read_timeout.tv_sec > 0) {
    upstream_rtimerev_ = init_timer(evbase, upstream_rtimeoutcb, this);
  }

  if(get_config()->stream_write_timeout.tv_sec > 0) {
    upstream_wtimerev_ = init_timer(evbase, upstream_wtimeoutcb, this);
  }
}

namespace {
void reset_timer(event *timer, const timeval *timeout)
{
  if(!timer) {
    return;
  }

  event_add(timer, timeout);
}
} // namespace

namespace {
void try_reset_timer(event *timer, const timeval *timeout)
{
  if(!timer) {
    return;
  }

  if(!evtimer_pending(timer, nullptr)) {
    return;
  }

  event_add(timer, timeout);
}
} // namespace

namespace {
void ensure_timer(event *timer, const timeval *timeout)
{
  if(!timer) {
    return;
  }

  if(evtimer_pending(timer, nullptr)) {
    return;
  }

  event_add(timer, timeout);
}
} // namespace

namespace {
void disable_timer(event *timer)
{
  if(!timer) {
    return;
  }

  event_del(timer);
}
} // namespace

void Downstream::reset_upstream_rtimer()
{
  reset_timer(upstream_rtimerev_, &get_config()->stream_read_timeout);
  try_reset_timer(upstream_wtimerev_, &get_config()->stream_write_timeout);
}

void Downstream::reset_upstream_wtimer()
{
  reset_timer(upstream_wtimerev_, &get_config()->stream_write_timeout);
  try_reset_timer(upstream_rtimerev_, &get_config()->stream_read_timeout);
}

void Downstream::ensure_upstream_wtimer()
{
  ensure_timer(upstream_wtimerev_, &get_config()->stream_write_timeout);
}

void Downstream::disable_upstream_rtimer()
{
  disable_timer(upstream_rtimerev_);
}

void Downstream::disable_upstream_wtimer()
{
  disable_timer(upstream_wtimerev_);
}

namespace {
void downstream_timeoutcb(evutil_socket_t fd, short event, void *arg)
{
  auto downstream = static_cast<Downstream*>(arg);

  auto which = event == EV_READ ? "read" : "write";

  if(LOG_ENABLED(INFO)) {
    DLOG(INFO, downstream) << "downstream timeout stream_id="
                           << downstream->get_downstream_stream_id()
                           << " event=" << which;
  }

  downstream->disable_downstream_rtimer();
  downstream->disable_downstream_wtimer();

  auto dconn = downstream->get_downstream_connection();

  if(dconn) {
    dconn->on_timeout();
  }
}
} // namespace

namespace {
void downstream_rtimeoutcb(evutil_socket_t fd, short event, void *arg)
{
  downstream_timeoutcb(fd, EV_READ, arg);
}
} // namespace

namespace {
void downstream_wtimeoutcb(evutil_socket_t fd, short event, void *arg)
{
  downstream_timeoutcb(fd, EV_WRITE, arg);
}
} // namespace

void Downstream::init_downstream_timer()
{
  auto evbase = upstream_->get_client_handler()->get_evbase();

  if(get_config()->stream_read_timeout.tv_sec > 0) {
    downstream_rtimerev_ = init_timer(evbase, downstream_rtimeoutcb, this);
  }

  if(get_config()->stream_write_timeout.tv_sec > 0) {
    downstream_wtimerev_ = init_timer(evbase, downstream_wtimeoutcb, this);
  }
}

void Downstream::reset_downstream_rtimer()
{
  reset_timer(downstream_rtimerev_, &get_config()->stream_read_timeout);
  try_reset_timer(downstream_wtimerev_, &get_config()->stream_write_timeout);
}

void Downstream::reset_downstream_wtimer()
{
  reset_timer(downstream_wtimerev_, &get_config()->stream_write_timeout);
  try_reset_timer(downstream_rtimerev_, &get_config()->stream_read_timeout);
}

void Downstream::ensure_downstream_wtimer()
{
  ensure_timer(downstream_wtimerev_, &get_config()->stream_write_timeout);
}

void Downstream::disable_downstream_rtimer()
{
  disable_timer(downstream_rtimerev_);
}

void Downstream::disable_downstream_wtimer()
{
  disable_timer(downstream_wtimerev_);
}

} // namespace shrpx
