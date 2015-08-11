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
#ifndef SHRPX_CONFIG_H
#define SHRPX_CONFIG_H

#include "shrpx.h"

#include <stdint.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <vector>
#include <memory>
#include <atomic>

#include <event.h>
#include <openssl/ssl.h>

#include <nghttp2/nghttp2.h>

namespace shrpx {

namespace ssl {

struct CertLookupTree;

} // namespace ssl

extern const char SHRPX_OPT_PRIVATE_KEY_FILE[];
extern const char SHRPX_OPT_PRIVATE_KEY_PASSWD_FILE[];
extern const char SHRPX_OPT_CERTIFICATE_FILE[];
extern const char SHRPX_OPT_DH_PARAM_FILE[];
extern const char SHRPX_OPT_SUBCERT[];
extern const char SHRPX_OPT_BACKEND[];
extern const char SHRPX_OPT_FRONTEND[];
extern const char SHRPX_OPT_WORKERS[];
extern const char SHRPX_OPT_HTTP2_MAX_CONCURRENT_STREAMS[];
extern const char SHRPX_OPT_LOG_LEVEL[];
extern const char SHRPX_OPT_DAEMON[];
extern const char SHRPX_OPT_HTTP2_PROXY[];
extern const char SHRPX_OPT_HTTP2_BRIDGE[];
extern const char SHRPX_OPT_CLIENT_PROXY[];
extern const char SHRPX_OPT_ADD_X_FORWARDED_FOR[];
extern const char SHRPX_OPT_NO_VIA[];
extern const char SHRPX_OPT_FRONTEND_HTTP2_READ_TIMEOUT[];
extern const char SHRPX_OPT_FRONTEND_READ_TIMEOUT[];
extern const char SHRPX_OPT_FRONTEND_WRITE_TIMEOUT[];
extern const char SHRPX_OPT_BACKEND_READ_TIMEOUT[];
extern const char SHRPX_OPT_BACKEND_WRITE_TIMEOUT[];
extern const char SHRPX_OPT_ACCESSLOG_FILE[];
extern const char SHRPX_OPT_ACCESSLOG_SYSLOG[];
extern const char SHRPX_OPT_ERRORLOG_FILE[];
extern const char SHRPX_OPT_ERRORLOG_SYSLOG[];
extern const char SHRPX_OPT_BACKEND_KEEP_ALIVE_TIMEOUT[];
extern const char SHRPX_OPT_FRONTEND_HTTP2_WINDOW_BITS[];
extern const char SHRPX_OPT_BACKEND_HTTP2_WINDOW_BITS[];
extern const char SHRPX_OPT_FRONTEND_HTTP2_CONNECTION_WINDOW_BITS[];
extern const char SHRPX_OPT_BACKEND_HTTP2_CONNECTION_WINDOW_BITS[];
extern const char SHRPX_OPT_FRONTEND_NO_TLS[];
extern const char SHRPX_OPT_BACKEND_NO_TLS[];
extern const char SHRPX_OPT_PID_FILE[];
extern const char SHRPX_OPT_USER[];
extern const char SHRPX_OPT_SYSLOG_FACILITY[];
extern const char SHRPX_OPT_BACKLOG[];
extern const char SHRPX_OPT_CIPHERS[];
extern const char SHRPX_OPT_CLIENT[];
extern const char SHRPX_OPT_INSECURE[];
extern const char SHRPX_OPT_CACERT[];
extern const char SHRPX_OPT_BACKEND_IPV4[];
extern const char SHRPX_OPT_BACKEND_IPV6[];
extern const char SHRPX_OPT_BACKEND_HTTP_PROXY_URI[];
extern const char SHRPX_OPT_BACKEND_TLS_SNI_FIELD[];
extern const char SHRPX_OPT_WORKER_READ_RATE[];
extern const char SHRPX_OPT_WORKER_READ_BURST[];
extern const char SHRPX_OPT_WORKER_WRITE_RATE[];
extern const char SHRPX_OPT_WORKER_WRITE_BURST[];
extern const char SHRPX_OPT_NPN_LIST[];
extern const char SHRPX_OPT_TLS_PROTO_LIST[];
extern const char SHRPX_OPT_VERIFY_CLIENT[];
extern const char SHRPX_OPT_VERIFY_CLIENT_CACERT[];
extern const char SHRPX_OPT_CLIENT_PRIVATE_KEY_FILE[];
extern const char SHRPX_OPT_CLIENT_CERT_FILE[];
extern const char SHRPX_OPT_FRONTEND_HTTP2_DUMP_REQUEST_HEADER[];
extern const char SHRPX_OPT_FRONTEND_HTTP2_DUMP_RESPONSE_HEADER[];
extern const char SHRPX_OPT_HTTP2_NO_COOKIE_CRUMBLING[];
extern const char SHRPX_OPT_FRONTEND_FRAME_DEBUG[];
extern const char SHRPX_OPT_PADDING[];
extern const char SHRPX_OPT_ALTSVC[];
extern const char SHRPX_OPT_ADD_RESPONSE_HEADER[];
extern const char SHRPX_OPT_WORKER_FRONTEND_CONNECTIONS[];

union sockaddr_union {
  sockaddr sa;
  sockaddr_storage storage;
  sockaddr_in6 in6;
  sockaddr_in in;
};

enum shrpx_proto {
  PROTO_HTTP2,
  PROTO_HTTP
};

struct AltSvc {
  AltSvc()
    : protocol_id(nullptr),
      host(nullptr),
      origin(nullptr),
      protocol_id_len(0),
      host_len(0),
      origin_len(0),
      port(0)
  {}

  char *protocol_id;
  char *host;
  char *origin;

  size_t protocol_id_len;
  size_t host_len;
  size_t origin_len;

  uint16_t port;
};

struct Config {
  // The list of (private key file, certificate file) pair
  std::vector<std::pair<std::string, std::string>> subcerts;
  std::vector<AltSvc> altsvcs;
  std::vector<std::pair<std::string, std::string>> add_response_headers;
  std::vector<unsigned char> alpn_prefs;
  std::shared_ptr<std::string> cached_time;
  sockaddr_union downstream_addr;
  // binary form of http proxy host and port
  sockaddr_union downstream_http_proxy_addr;
  timeval http2_upstream_read_timeout;
  timeval upstream_read_timeout;
  timeval upstream_write_timeout;
  timeval downstream_read_timeout;
  timeval downstream_write_timeout;
  timeval downstream_idle_read_timeout;
  std::unique_ptr<char[]> host;
  std::unique_ptr<char[]> private_key_file;
  std::unique_ptr<char[]> private_key_passwd;
  std::unique_ptr<char[]> cert_file;
  std::unique_ptr<char[]> dh_param_file;
  SSL_CTX *default_ssl_ctx;
  ssl::CertLookupTree *cert_tree;
  const char *server_name;
  std::unique_ptr<char[]> downstream_host;
  std::unique_ptr<char[]> downstream_hostport;
  std::unique_ptr<char[]> backend_tls_sni_name;
  std::unique_ptr<char[]> pid_file;
  std::unique_ptr<char[]> conf_path;
  std::unique_ptr<char[]> ciphers;
  std::unique_ptr<char[]> cacert;
  // userinfo in http proxy URI, not percent-encoded form
  std::unique_ptr<char[]> downstream_http_proxy_userinfo;
  // host in http proxy URI
  std::unique_ptr<char[]> downstream_http_proxy_host;
  // Rate limit configuration per worker (thread)
  ev_token_bucket_cfg *worker_rate_limit_cfg;
  // list of supported NPN/ALPN protocol strings in the order of
  // preference. The each element of this list is a NULL-terminated
  // string.
  std::vector<char*> npn_list;
  // list of supported SSL/TLS protocol strings. The each element of
  // this list is a NULL-terminated string.
  std::vector<char*> tls_proto_list;
  // Path to file containing CA certificate solely used for client
  // certificate validation
  std::unique_ptr<char[]> verify_client_cacert;
  std::unique_ptr<char[]> client_private_key_file;
  std::unique_ptr<char[]> client_cert_file;
  std::unique_ptr<char[]> accesslog_file;
  std::unique_ptr<char[]> errorlog_file;
  FILE *http2_upstream_dump_request_header;
  FILE *http2_upstream_dump_response_header;
  nghttp2_option *http2_option;
  size_t downstream_addrlen;
  size_t num_worker;
  size_t http2_max_concurrent_streams;
  size_t http2_upstream_window_bits;
  size_t http2_downstream_window_bits;
  size_t http2_upstream_connection_window_bits;
  size_t http2_downstream_connection_window_bits;
  // actual size of downstream_http_proxy_addr
  size_t downstream_http_proxy_addrlen;
  size_t read_rate;
  size_t read_burst;
  size_t write_rate;
  size_t write_burst;
  size_t worker_read_rate;
  size_t worker_read_burst;
  size_t worker_write_rate;
  size_t worker_write_burst;
  size_t padding;
  size_t worker_frontend_connections;
  // Bit mask to disable SSL/TLS protocol versions.  This will be
  // passed to SSL_CTX_set_options().
  long int tls_proto_mask;
  // downstream protocol; this will be determined by given options.
  shrpx_proto downstream_proto;
  int syslog_facility;
  int backlog;
  uid_t uid;
  gid_t gid;
  uint16_t port;
  uint16_t downstream_port;
  // port in http proxy URI
  uint16_t downstream_http_proxy_port;
  bool verbose;
  bool daemon;
  bool verify_client;
  bool http2_proxy;
  bool http2_bridge;
  bool client_proxy;
  bool add_x_forwarded_for;
  bool no_via;
  bool upstream_no_tls;
  bool downstream_no_tls;
  // Send accesslog to syslog, ignoring accesslog_file.
  bool accesslog_syslog;
  // Send errorlog to syslog, ignoring errorlog_file.
  bool errorlog_syslog;
  bool client;
  // true if --client or --client-proxy are enabled.
  bool client_mode;
  bool insecure;
  bool backend_ipv4;
  bool backend_ipv6;
  bool http2_no_cookie_crumbling;
  bool upstream_frame_debug;
};

const Config* get_config();
Config* mod_config();
void create_config();

// Parses option name |opt| and value |optarg|.  The results are
// stored into statically allocated Config object. This function
// returns 0 if it succeeds, or -1.
int parse_config(const char *opt, const char *optarg);

// Loads configurations from |filename| and stores them in statically
// allocated Config object. This function returns 0 if it succeeds, or
// -1.
int load_config(const char *filename);

// Read passwd from |filename|
std::string read_passwd_from_file(const char *filename);

// Parses comma delimited strings in |s| and returns the array of
// pointers, each element points to the each substring in |s|.  The
// |s| must be comma delimited list of strings.  The strings must be
// delimited by a single comma and any white spaces around it are
// treated as a part of protocol strings.  This function may modify
// |s| and the caller must leave it as is after this call.  This
// function copies |s| and first element in the return value points to
// it.  It is caller's responsibility to deallocate its memory.
std::vector<char*> parse_config_str_list(const char *s);

// Clears all elements of |list|, which is returned by
// parse_config_str_list().  If list is not empty, list[0] is freed by
// free(2).  After this call, list.empty() must be true.
void clear_config_str_list(std::vector<char*>& list);

// Parses header field in |optarg|.  We expect header field is formed
// like "NAME: VALUE".  We require that NAME is non empty string.  ":"
// is allowed at the start of the NAME, but NAME == ":" is not
// allowed.  This function returns pair of NAME and VALUE.
std::pair<std::string, std::string> parse_header(const char *optarg);

// Returns a copy of NULL-terminated string |val|.
std::unique_ptr<char[]> strcopy(const char *val);

// Returns a copy of val.c_str().
std::unique_ptr<char[]> strcopy(const std::string& val);

// Returns string for syslog |facility|.
const char* str_syslog_facility(int facility);

// Returns integer value of syslog |facility| string.
int int_syslog_facility(const char *strfacility);

} // namespace shrpx

#endif // SHRPX_CONFIG_H
