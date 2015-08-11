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
#ifndef HTTP2_HANDLER_H
#define HTTP2_HANDLER_H

#include "nghttp2_config.h"

#include <map>
#include <vector>
#include <functional>
#include <string>
#include <boost/array.hpp>
#include <boost/asio.hpp>

#include <nghttp2/nghttp2.h>

#include <nghttp2/asio_http2.h>

namespace nghttp2 {
namespace asio_http2 {

class channel_impl {
public:
  channel_impl();
  void post(void_cb cb);
  void strand(boost::asio::io_service::strand *strand);
private:
  boost::asio::io_service::strand *strand_;
};

namespace server {

class http2_handler;
class http2_stream;

class request_impl {
public:
  request_impl();

  const std::vector<header>& headers() const;
  const std::string& method() const;
  const std::string& scheme() const;
  const std::string& authority() const;
  const std::string& host() const;
  const std::string& path() const;

  bool push(std::string method, std::string path,
            std::vector<header> headers = {});

  bool pushed() const;
  bool closed() const;

  void on_data(data_cb cb);
  void on_end(void_cb cb);

  bool run_task(thread_cb start);

  void set_header(std::vector<header> headers);
  void add_header(std::string name, std::string value);
  void method(std::string method);
  void scheme(std::string scheme);
  void authority(std::string authority);
  void host(std::string host);
  void path(std::string path);
  void pushed(bool f);
  void handler(std::weak_ptr<http2_handler> h);
  void stream(std::weak_ptr<http2_stream> s);
  void call_on_data(const uint8_t *data, std::size_t len);
  void call_on_end();
private:
  std::vector<header> headers_;
  std::string method_;
  std::string scheme_;
  std::string authority_;
  std::string host_;
  std::string path_;
  data_cb on_data_cb_;
  void_cb on_end_cb_;
  std::weak_ptr<http2_handler> handler_;
  std::weak_ptr<http2_stream> stream_;
  bool pushed_;
};

class response_impl {
public:
  response_impl();
  void write_head(unsigned int status_code, std::vector<header> headers = {});
  void end(std::string data = "");
  void end(read_cb cb);
  void resume();
  bool closed() const;

  unsigned int status_code() const;
  const std::vector<header>& headers() const;
  bool started() const;
  void handler(std::weak_ptr<http2_handler> h);
  void stream(std::weak_ptr<http2_stream> s);
  read_cb::result_type call_read(uint8_t *data, std::size_t len);
private:
  std::vector<header> headers_;
  read_cb read_cb_;
  std::weak_ptr<http2_handler> handler_;
  std::weak_ptr<http2_stream> stream_;
  unsigned int status_code_;
  bool started_;
};

class http2_stream {
public:
  http2_stream(int32_t stream_id);

  int32_t get_stream_id() const;
  const std::shared_ptr<request>& get_request();
  const std::shared_ptr<response>& get_response();
private:
  std::shared_ptr<request> request_;
  std::shared_ptr<response> response_;
  int32_t stream_id_;
};

struct callback_guard {
  callback_guard(http2_handler& h);
  ~callback_guard();
  http2_handler& handler;
};

typedef std::function<void(void)> connection_write;

class http2_handler : public std::enable_shared_from_this<http2_handler> {
public:
  http2_handler(boost::asio::io_service& io_service,
                boost::asio::io_service& task_io_service,
                connection_write writefun,
                request_cb cb);

  ~http2_handler();

  int start();

  std::shared_ptr<http2_stream> create_stream(int32_t stream_id);
  void close_stream(int32_t stream_id);
  std::shared_ptr<http2_stream> find_stream(int32_t stream_id);

  void call_on_request(http2_stream& stream);

  bool should_stop() const;

  int start_response(http2_stream& stream);

  void stream_error(int32_t stream_id, uint32_t error_code);

  void initiate_write();

  void enter_callback();
  void leave_callback();
  bool inside_callback() const;

  void resume(http2_stream& stream);

  int push_promise(http2_stream& stream, std::string method,
                   std::string path,
                   std::vector<header> headers);

  bool run_task(thread_cb start);

  boost::asio::io_service& io_service();

  template<size_t N>
  int on_read(const boost::array<uint8_t, N>& buffer, std::size_t len)
  {
    callback_guard cg(*this);

    int rv;

    rv = nghttp2_session_mem_recv(session_, buffer.data(), len);

    if(rv < 0) {
      return -1;
    }

    return 0;
  }

  template<size_t N>
  int on_write(boost::array<uint8_t, N>& buffer, std::size_t& len)
  {
    callback_guard cg(*this);

    len = 0;

    if(buf_) {
      std::copy(buf_, buf_ + buflen_, std::begin(buffer));

      len += buflen_;

      buf_ = nullptr;
      buflen_ = 0;
    }

    for(;;) {
      const uint8_t *data;
      auto nread = nghttp2_session_mem_send(session_, &data);
      if(nread < 0) {
        return -1;
      }

      if(nread == 0) {
        break;
      }

      if(len + nread > buffer.size()) {
        buf_ = data;
        buflen_ = nread;

        break;
      }

      std::copy(data, data + nread, std::begin(buffer) + len);

      len += nread;
    }

    return 0;
  }

private:
  std::map<int32_t, std::shared_ptr<http2_stream>> streams_;
  connection_write writefun_;
  request_cb request_cb_;
  boost::asio::io_service& io_service_;
  boost::asio::io_service& task_io_service_;
  std::shared_ptr<boost::asio::io_service::strand> strand_;
  nghttp2_session *session_;
  const uint8_t *buf_;
  std::size_t buflen_;
  bool inside_callback_;
};

} // namespace server
} // namespace asio_http2
} // namespace nghttp

#endif // HTTP2_HANDLER_H
