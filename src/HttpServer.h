/*
 * nghttp2 - HTTP/2 C Library
 *
 * Copyright (c) 2013 Tatsuhiro Tsujikawa
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
#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "nghttp2_config.h"

#include <stdint.h>
#include <sys/types.h>

#include <cstdlib>

#include <string>
#include <vector>
#include <map>
#include <memory>

#include <openssl/ssl.h>

#include <event2/event.h>
#include <event2/bufferevent.h>

#include <nghttp2/nghttp2.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "nghttp2_buf.h"

#ifdef __cplusplus
}
#endif

#include "http2.h"

namespace nghttp2 {

struct Config {
  std::map<std::string, std::vector<std::string>> push;
  std::string htdocs;
  std::string host;
  std::string private_key_file;
  std::string cert_file;
  std::string dh_param_file;
  timeval stream_read_timeout;
  timeval stream_write_timeout;
  nghttp2_option *session_option;
  void *data_ptr;
  size_t padding;
  size_t num_worker;
  ssize_t header_table_size;
  uint16_t port;
  bool verbose;
  bool daemon;
  bool verify_client;
  bool no_tls;
  bool error_gzip;
  bool early_response;
  Config();
  ~Config();
};

class Http2Handler;

struct Stream {
  Headers headers;
  std::pair<std::string, size_t> response_body;
  Http2Handler *handler;
  event *rtimer;
  event *wtimer;
  int32_t stream_id;
  int file;
  Stream(Http2Handler *handler, int32_t stream_id);
  ~Stream();
};

class Sessions;

class Http2Handler {
public:
  Http2Handler(Sessions *sessions, int fd, SSL *ssl, int64_t session_id);
  ~Http2Handler();

  void remove_self();
  int setup_bev();
  int send();
  int on_read();
  int on_write();
  int on_connect();
  int verify_npn_result();

  int submit_file_response(const std::string &status, Stream *stream,
                           time_t last_modified, off_t file_length,
                           nghttp2_data_provider *data_prd);

  int submit_response(const std::string &status, int32_t stream_id,
                      nghttp2_data_provider *data_prd);

  int submit_response(const std::string &status, int32_t stream_id,
                      const Headers &headers, nghttp2_data_provider *data_prd);

  int submit_non_final_response(const std::string &status, int32_t stream_id);

  int submit_push_promise(Stream *stream, const std::string &push_path);

  int submit_rst_stream(Stream *stream, uint32_t error_code);

  void add_stream(int32_t stream_id, std::unique_ptr<Stream> stream);
  void remove_stream(int32_t stream_id);
  Stream *get_stream(int32_t stream_id);
  int64_t session_id() const;
  Sessions *get_sessions() const;
  const Config *get_config() const;
  void remove_settings_timer();
  void terminate_session(uint32_t error_code);

private:
  std::map<int32_t, std::unique_ptr<Stream>> id2stream_;
  int64_t session_id_;
  nghttp2_session *session_;
  Sessions *sessions_;
  SSL *ssl_;
  bufferevent *bev_;
  event *settings_timerev_;
  int fd_;
};

class HttpServer {
public:
  HttpServer(const Config *config);
  int listen();
  int run();
  const Config *get_config() const;

private:
  const Config *config_;
};

ssize_t file_read_callback(nghttp2_session *session, int32_t stream_id,
                           uint8_t *buf, size_t length, int *eof,
                           nghttp2_data_source *source, void *user_data);

} // namespace nghttp2

#endif // HTTP_SERVER_H
