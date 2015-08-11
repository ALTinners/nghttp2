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
#include "h2load_http2_session.h"

#include <cassert>

#include "h2load.h"
#include "util.h"

using namespace nghttp2;

namespace h2load {

Http2Session::Http2Session(Client *client)
  : client_(client),
    session_(nullptr)
{}

Http2Session::~Http2Session()
{
  nghttp2_session_del(session_);
}

namespace {
int on_header_callback(nghttp2_session *session,
                       const nghttp2_frame *frame,
                       const uint8_t *name, size_t namelen,
                       const uint8_t *value, size_t valuelen,
                       uint8_t flags,
                       void *user_data)
{
  auto client = static_cast<Client*>(user_data);
  if(frame->hd.type != NGHTTP2_HEADERS ||
     frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return 0;
  }
  client->on_header(frame->hd.stream_id, name, namelen, value, valuelen);
  return 0;
}
} // namespace

namespace {
int on_frame_recv_callback(nghttp2_session *session,
                           const nghttp2_frame *frame,
                           void *user_data)
{
  auto client = static_cast<Client*>(user_data);
  if(frame->hd.type != NGHTTP2_HEADERS ||
     frame->headers.cat != NGHTTP2_HCAT_RESPONSE) {
    return 0;
  }
  client->worker->stats.bytes_head += frame->hd.length;
  return 0;
}
} // namespace

namespace {
int on_data_chunk_recv_callback
(nghttp2_session *session, uint8_t flags, int32_t stream_id,
 const uint8_t *data, size_t len, void *user_data)
{
  auto client = static_cast<Client*>(user_data);
  client->worker->stats.bytes_body += len;
  return 0;
}
} // namespace

namespace {
int on_stream_close_callback
(nghttp2_session *session, int32_t stream_id, nghttp2_error_code error_code,
 void *user_data)
{
  auto client = static_cast<Client*>(user_data);
  client->on_stream_close(stream_id, error_code == NGHTTP2_NO_ERROR);
  return 0;
}
} // namespace

void Http2Session::on_connect()
{
  int rv;

  nghttp2_session_callbacks callbacks = {0};
  callbacks.on_frame_recv_callback = on_frame_recv_callback;
  callbacks.on_data_chunk_recv_callback = on_data_chunk_recv_callback;
  callbacks.on_stream_close_callback = on_stream_close_callback;
  callbacks.on_header_callback = on_header_callback;

  nghttp2_session_client_new(&session_, &callbacks, client_);

  nghttp2_settings_entry iv[2];
  iv[0].settings_id = NGHTTP2_SETTINGS_ENABLE_PUSH;
  iv[0].value = 0;
  iv[1].settings_id = NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE;
  iv[1].value = (1 << client_->worker->config->window_bits) - 1;

  rv = nghttp2_submit_settings(session_, NGHTTP2_FLAG_NONE, iv,
                               sizeof(iv) / sizeof(iv[0]));

  assert(rv == 0);

  auto extra_connection_window =
    (1 << client_->worker->config->connection_window_bits) - 1
    - NGHTTP2_INITIAL_CONNECTION_WINDOW_SIZE;
  if(extra_connection_window != 0) {
    nghttp2_submit_window_update(session_, NGHTTP2_FLAG_NONE, 0,
                                 extra_connection_window);
  }

  bufferevent_write(client_->bev, NGHTTP2_CLIENT_CONNECTION_PREFACE,
                    NGHTTP2_CLIENT_CONNECTION_PREFACE_LEN);
}

void Http2Session::submit_request()
{
  auto config = client_->worker->config;
  auto& nva = config->nva[client_->reqidx++];

  if(client_->reqidx == config->nva.size()) {
    client_->reqidx = 0;
  }

  auto stream_id = nghttp2_submit_request(session_, nullptr,
                                          nva.data(), nva.size(),
                                          nullptr, nullptr);
  assert(stream_id > 0);

  client_->on_request(stream_id);
}

ssize_t Http2Session::on_read()
{
  int rv;
  size_t nread = 0;

  auto input = bufferevent_get_input(client_->bev);

  for(;;) {
    auto inputlen = evbuffer_get_contiguous_space(input);

    if(inputlen == 0) {
      assert(evbuffer_get_length(input) == 0);

      return nread;
    }

    auto mem = evbuffer_pullup(input, inputlen);

    rv = nghttp2_session_mem_recv(session_, mem, inputlen);

    if(rv < 0) {
      return -1;
    }

    nread += rv;

    if(evbuffer_drain(input, rv) != 0) {
      return -1;
    }
  }
}

int Http2Session::on_write()
{
  int rv;
  uint8_t buf[16384];
  auto output = bufferevent_get_output(client_->bev);
  util::EvbufferBuffer evbbuf(output, buf, sizeof(buf));
  for(;;) {
    const uint8_t *data;
    auto datalen = nghttp2_session_mem_send(session_, &data);

    if(datalen < 0) {
      return -1;
    }
    if(datalen == 0) {
      break;
    }
    rv = evbbuf.add(data, datalen);
    if(rv != 0) {
      return -1;
    }
  }
  rv = evbbuf.flush();
  if(rv != 0) {
    return -1;
  }
  if(nghttp2_session_want_read(session_) == 0 &&
     nghttp2_session_want_write(session_) == 0 &&
     evbuffer_get_length(output) == 0) {
    return -1;
  }
  return 0;
}

void Http2Session::terminate()
{
  nghttp2_session_terminate_session(session_, NGHTTP2_NO_ERROR);
}

} // namespace h2load
