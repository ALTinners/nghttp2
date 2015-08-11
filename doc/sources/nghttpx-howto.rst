nghttpx - HOW-TO
================

nghttpx is a proxy translating protocols between HTTP/2 and other
protocols (e.g., HTTP/1, SPDY).  It operates in several modes and each
mode may require additional programs to work with.  This article
describes each operation mode and explains the intended use-cases.  It
also covers some useful options later.

Default mode
------------

If nghttpx is invoked without any ``-s``, ``-p`` and ``--client``, it
operates in default mode.  In this mode, nghttpx frontend listens for
HTTP/2 requests and translates them to HTTP/1 requests.  Thus it works
as reverse proxy (gateway) for HTTP/2 clients to HTTP/1 web server.
HTTP/1 requests are also supported in frontend as a fallback.  If
nghttpx is linked with spdylay library and frontend connection is
SSL/TLS, the frontend also supports SPDY protocol.

By default, this mode's frontend connection is encrypted using
SSL/TLS.  So server's private key and certificate must be supplied to
the command line (or through configuration file).  In this case, the
fontend protocol selection will is done via ALPN or NPN.

With ``--frontend-no-tls`` option, user can turn off SSL/TLS in
frontend connection.  In this case, SPDY protocol is not available
even if spdylay library is liked to nghttpx.  HTTP/2 and HTTP/1 are
available on the frontend and a HTTP/1 connection can be upgraded to
HTTP/2 using HTTP Upgrade.  Starting HTTP/2 connection by sending
HTTP/2 connection preface is also supported.

The backend is supposed to be HTTP/1 Web server.  For example, to make
nghttpx listen to encrypted HTTP/2 requests at port 8443, and a
backend HTTP/1 web server is configured to listen to HTTP/1 request at
port 8080 in the same host, run nghttpx command-line like this::

    $ nghttpx -f0.0.0.0,8443 -b127.0.0.1,8080 /path/to/server.key /path/to/server.crt

Then HTTP/2 enabled client can access to the nghttpx in HTTP/2.  For
example, you can send GET request to the server using nghttp::

    $ nghttp -nv https://localhost:8443/

HTTP/2 proxy mode
-----------------

If nghttpx is invoked with ``-s`` option, it operates in HTTP/2 proxy
mode.  The supported protocols in frontend and backend connections are
the same in `default mode`_.  The difference is that this mode acts like
forward proxy and assumes the backend is HTTP/1 proxy server (e.g.,
squid).  So HTTP/1 request must include absolute URI in request line.

By default, frontend connection is encrypted, this mode is also called
secure proxy.  If nghttpx is linked with spdylay, it supports SPDY
protocols and it works as so called SPDY proxy.

With ``--frontend-no-tls`` option, SSL/TLS is turned off in frontend
connection, so the connection gets insecure.

The backend must be HTTP/1 proxy server.  nghttpx only supports 1
backend server address.  It translates incoming requests to HTTP/1
request to backend server.  The backend server performs real proxy
work for each request, for example, dispatching requests to the origin
server and caching contents.

For example, to make nghttpx listen to encrypted HTTP/2 requests at
port 8443, and a backend HTTP/1 proxy server is configured to listen
to HTTP/1 request at port 3128 in the same host, run nghttpx
command-line like this::

    $ nghttpx -s -f0.0.0.0,8443 -b127.0.0.1,3128 /path/to/server.key /path/to/server.crt

At the time of this writing, there is no known HTTP/2 client which
supports HTTP/2 proxy in this fashion.  You can use Google Chrome to
use this as secure (SPDY) proxy to test it out, though it does not use
HTTP/2 at all.

The one way to configure Google Chrome to use secure proxy is create
proxy.pac script like this:

.. code-block:: javascript

    function FindProxyForURL(url, host) {
        return "HTTPS SERVERADDR:PORT";
    }

``SERVERADDR`` and ``PORT`` is the hostname/address and port of the
machine nghttpx is running.  Please note that Google Chrome requires
valid certificate for secure proxy.

Then run Google Chrome with the following arguments::

    $ google-chrome --proxy-pac-url=file:///path/to/proxy.pac --use-npn

Client mode
-----------

If nghttpx is invoked with ``--client`` option, it operates in client
mode.  In this mode, nghttpx listens for plain, unencrypted HTTP/2 and
HTTP/1 requests and translates them to encrypted HTTP/2 requests to
the backend.  User cannot enable SSL/TLS in frontend connection.

HTTP/1 frontend connection can be upgraded to HTTP/2 using HTTP
Upgrade.  To disable SSL/TLS in backend connection, use
``--backend-no-tls`` option.

The backend connection is created one per worker (thread).

The backend server is supporsed to be a HTTP/2 web server (e.g.,
nghttpd).  The one use-case of this mode is utilize existing HTTP/1
clients to test HTTP/2 deployment.  Suppose that HTTP/2 web server
listens to port 80 without encryption.  Then run nghttpx as client
mode to access to that web server::

    $ nghttpx --client -f127.0.0.1,8080 -b127.0.0.1,80 --backend-no-tls

.. note::

    You may need ``-k`` option if HTTP/2 server enables SSL/TLS and
    its certificate is self-signed. But please note that it is
    insecure.

Then you can use curl to access HTTP/2 server via nghttpx::

    $ curl http://localhost:8080/

Client proxy mode
-----------------

If nghttpx is invoked with ``-p`` option, it operates in client proxy
mode.  This mode behaves like `client mode`_, but it works like
forward proxy.  So HTTP/1 request must include absolute URI in request
line.

HTTP/1 frontend connection can be upgraded to HTTP/2 using HTTP
Upgrade.  To disable SSL/TLS in backend connection, use
``--backend-no-tls`` option.

The backend connection is created one per worker (thread).

The backend server must be a HTTP/2 proxy.  You can use nghttpx in
`HTTP/2 proxy mode`_ as backend server.  The one use-case of this mode
is utilize existing HTTP/1 clients to test HTTP/2 connections between
2 proxies. The another use-case is use this mode to aggregate local
HTTP/1 connections to one HTTP/2 backend encrypted connection.  This
makes HTTP/1 clients which does not support secure proxy can use
secure HTTP/2 proxy via nghttpx client mode.

Suppose that HTTP/2 proxy listens to port 8443, just like we saw in
`HTTP/2 proxy mode`_.  To run nghttpx in client proxy mode to access
that server, invoke nghttpx like this::

    $ nghttpx -p -f127.0.0.1,8080 -b127.0.0.1,8443

.. note::

    You may need ``-k`` option if HTTP/2 server'ss certificate is
    self-signed. But please note that it is insecure.

Then you can use curl to issue HTTP request via HTTP/2 proxy::

    $ curl --http-proxy=http://localhost:8080 http://www.google.com/

You can configure web browser to use localhost:8080 as forward
proxy.

HTTP/2 bridge mode
------------------

If nghttpx is invoked with ``--http2-bridge`` option, it operates in
HTTP/2 bridge mode.  The supported protocols in frontend and backend
connections are the same in `default mode`_.

With ``--frontend-no-tls`` option, SSL/TLS is turned off in frontend
connection, so the connection gets insecure.

The backend server is supporsed to be a HTTP/2 web server or HTTP/2
proxy.  Since HTTP/2 requests opaque between proxied and non-proxied
request, the backend server may be proxy or just web server depending
on the context of incoming requests.

The use-case of this mode is aggregate the incoming connections to one
HTTP/2 connection.  One backend HTTP/2 connection is created per
worker (thread).

Disable SSL/TLS
---------------

In `default mode`_, `HTTP/2 proxy mode`_ and `HTTP/2 bridge mode`_,
frontend connections are encrypted with SSL/TLS by default.  To turn
off SSL/TLS, use ``--frontend-no-tls`` option.  If this option is
used, the private key and certificate are not required to run nghttpx.

In `client mode`_, `client proxy mode`_ and `HTTP/2 bridge mode`_,
backend connections are encrypted with SSL/TLS by default.  To turn
off SSL/TLS, use ``--backend-no-tls`` option.

Specifying additional CA certificate
------------------------------------

By default, nghttpx tries to read CA certificate from system.  But
depending on the system you use, this may fail or is not supported.
To specify CA certificate manually, use ``--cacert`` option.  The
specified file must be PEM format and can contain multiple
certificates.

By default, nghttpx validates server's certificate.  If you want to
turn off this validation, knowing this is really insecure and what you
are doing, you can use ``-k`` option to disable certificate
validation.

Read/write rate limit
---------------------

nghttpx supports transfer rate limiting on frontend connections.  You
can do rate limit per connection or per worker (thread) for reading
and writeing individually.

To rate limit per connection for reading, use ``--read-rate`` and
``--read-burst`` options.  For writing, use ``--write-rate`` and
``--write-burst`` options.

To rate limit per worker (thread), use ``--worker-read-rate`` and
``--worker-read-burst`` options.  For writing, use
``--worker-write-rate`` and ``--worker-write-burst``.

If both per connection and per worker rate limit configurations are
specified, the lower rate is used.

Please note that rate limit is performed on top of TCP and nothing to
do with HTTP/2 flow control.
