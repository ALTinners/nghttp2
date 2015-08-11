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
#include "nghttp2_outbound_item.h"

#include <assert.h>

void nghttp2_outbound_item_free(nghttp2_outbound_item *item)
{
  if(item == NULL) {
    return;
  }
  if(item->frame_cat == NGHTTP2_CAT_CTRL) {
    nghttp2_frame *frame;
    frame = nghttp2_outbound_item_get_ctrl_frame(item);
    switch(frame->hd.type) {
    case NGHTTP2_HEADERS:
      nghttp2_frame_headers_free(&frame->headers);
      if(item->aux_data) {
        free(((nghttp2_headers_aux_data*)item->aux_data)->data_prd);
      }
      break;
    case NGHTTP2_PRIORITY:
      nghttp2_frame_priority_free(&frame->priority);
      break;
    case NGHTTP2_RST_STREAM:
      nghttp2_frame_rst_stream_free(&frame->rst_stream);
      break;
    case NGHTTP2_SETTINGS:
      nghttp2_frame_settings_free(&frame->settings);
      break;
    case NGHTTP2_PUSH_PROMISE:
      nghttp2_frame_push_promise_free(&frame->push_promise);
      break;
    case NGHTTP2_PING:
      nghttp2_frame_ping_free(&frame->ping);
      break;
    case NGHTTP2_GOAWAY:
      nghttp2_frame_goaway_free(&frame->goaway);
      break;
    case NGHTTP2_WINDOW_UPDATE:
      nghttp2_frame_window_update_free(&frame->window_update);
      break;
    case NGHTTP2_EXT_ALTSVC:
      nghttp2_frame_altsvc_free(&frame->ext);
      free(frame->ext.payload);
      break;
    }
  } else if(item->frame_cat == NGHTTP2_CAT_DATA) {
    nghttp2_private_data *data_frame;
    data_frame = nghttp2_outbound_item_get_data_frame(item);
    nghttp2_frame_private_data_free(data_frame);
  } else {
    /* Unreachable */
    assert(0);
  }
  free(item->frame);
  free(item->aux_data);
}
