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
#include "nghttp2_npn.h"

#include <string.h>

int nghttp2_select_next_protocol(unsigned char **out, unsigned char *outlen,
                                 const unsigned char *in, unsigned int inlen)
{
  int http_selected = 0;
  unsigned int i = 0;
  for(; i < inlen; i += in[i]+1) {
    if(in[i] == NGHTTP2_PROTO_VERSION_ID_LEN &&
       i + 1 + in[i] <= inlen &&
       memcmp(&in[i+1], NGHTTP2_PROTO_VERSION_ID, in[i]) == 0) {
      *out = (unsigned char*)&in[i+1];
      *outlen = in[i];
      return 1;
    }
    if(in[i] == 8 && i + 1 + in[i] <= inlen &&
       memcmp(&in[i+1], "http/1.1", in[i]) == 0) {
      http_selected = 1;
      *out = (unsigned char*)&in[i+1];
      *outlen = in[i];
      /* Go through to the next iteration, because "HTTP/2" may be
         there */
    }
  }
  if(http_selected) {
    return 0;
  } else {
    return -1;
  }
}
