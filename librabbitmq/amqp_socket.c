/*
 * ***** BEGIN LICENSE BLOCK *****
 * Version: MIT
 *
 * Portions created by VMware are Copyright (c) 2007-2012 VMware, Inc.
 * All Rights Reserved.
 *
 * Portions created by Tony Garnock-Jones are Copyright (c) 2009-2010
 * VMware, Inc. and Tony Garnock-Jones. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 * ***** END LICENSE BLOCK *****
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "amqp_private.h"

#include "socket.h"

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <openssl/hmac.h>
#include <openssl/sha.h>

void evoz_signature(char* signature, const char* udid, const char* home, const char* serial, const char* key, long long now, char* expiryString);

int amqp_open_socket(char const *hostname,
		     int portnumber)
{
  int sockfd, res;
  struct sockaddr_in addr;
  struct hostent *he;
  int one = 1; /* used as a buffer by setsockopt below */

  res = amqp_socket_init();
  if (res)
    return res;

  he = gethostbyname(hostname);
  if (he == NULL)
    return -ERROR_GETHOSTBYNAME_FAILED;

  addr.sin_family = AF_INET;
  addr.sin_port = htons(portnumber);
  addr.sin_addr.s_addr = * (uint32_t *) he->h_addr_list[0];

  sockfd = amqp_socket_socket(PF_INET, SOCK_STREAM, 0);
  if (sockfd == -1)
    return -amqp_socket_error(NULL);

  if (amqp_socket_setsockopt(sockfd, IPPROTO_TCP, TCP_NODELAY, &one,
			     sizeof(one)) < 0
      || connect(sockfd, (struct sockaddr *) &addr, sizeof(addr)) < 0)
  {
    res = -amqp_socket_error(NULL);
    amqp_socket_close(sockfd, NULL);
    return res;
  }

  return sockfd;
}

int amqp_send_header(amqp_connection_state_t state) {
  static const uint8_t header[8] = { 'A', 'M', 'Q', 'P', 0,
				     AMQP_PROTOCOL_VERSION_MAJOR,
				     AMQP_PROTOCOL_VERSION_MINOR,
				     AMQP_PROTOCOL_VERSION_REVISION };
  return state->send(state->sockfd, (void *)header, 8, 0, state->user_data);
}

static amqp_bytes_t sasl_method_name(amqp_sasl_method_enum method) {
  amqp_bytes_t res;

  switch (method) {
  case AMQP_SASL_METHOD_PLAIN:
    res.bytes = "PLAIN";
    res.len = 5;
    break;
  
  case AMQP_SASL_METHOD_EVOZ:
    res.bytes = "EVOZ";
    res.len = 4;
    break;

  default:
    amqp_abort("Invalid SASL method: %d", (int) method);
  }

  return res;
}

static amqp_bytes_t sasl_response(amqp_pool_t *pool,
				  amqp_sasl_method_enum method,
				  va_list args)
{
  amqp_bytes_t response;

  switch (method) {
    case AMQP_SASL_METHOD_PLAIN: {
      char *username = va_arg(args, char *);
      size_t username_len = strlen(username);
      char *password = va_arg(args, char *);
      size_t password_len = strlen(password);
      char *response_buf;

      amqp_pool_alloc_bytes(pool, strlen(username) + strlen(password) + 2, &response);
      if (response.bytes == NULL)
	/* We never request a zero-length block, because of the +2
	   above, so a NULL here really is ENOMEM. */
	return response;

      response_buf = response.bytes;
      response_buf[0] = 0;
      memcpy(response_buf + 1, username, username_len);
      response_buf[username_len + 1] = 0;
      memcpy(response_buf + username_len + 2, password, password_len);
      break;
    }
    case AMQP_SASL_METHOD_EVOZ: {
      char *udid = va_arg(args, char *);
      size_t udid_len = strlen(udid);
      char *key = va_arg(args, char *);
      char* serial = va_arg(args, char*);
      char* home = va_arg(args, char*);
      size_t home_len = strlen(home);
      char* extra = va_arg(args, char*);
      size_t extra_len = strlen(extra);
      long long now = va_arg(args, long long);

      char hmac64[100];
      char expiry[23];
      evoz_signature(hmac64, udid, home, serial, key, now, expiry);
      size_t hmac64_len = strlen(hmac64);

      size_t expiry_len = strlen(expiry);

      char *response_buf;
      amqp_pool_alloc_bytes(pool, udid_len + home_len + hmac64_len + expiry_len + extra_len + 5, &response);
      if (response.bytes == NULL)
          /* We never request a zero-length block, because of the +2
             +           above, so a NULL here really is ENOMEM. */
              return response;

      //printf("udid: '%s'\nhome: '%s'\nserial: '%s'\nkey: '%s'\nexpiry: %s\nsignature: '%s'\n", udid, home, serial, key, expiry, hmac64);

      response_buf = response.bytes;
      response_buf[0] = 0;
      memcpy(response_buf + 1, udid, udid_len);
      response_buf[udid_len + 1] = 0;
      memcpy(response_buf + udid_len + 2, home, home_len);
      response_buf[udid_len + home_len + 2] = 0;
      memcpy(response_buf + udid_len + home_len + 3, hmac64, hmac64_len);
      response_buf[udid_len + home_len + hmac64_len + 3] = 0;
      memcpy(response_buf + udid_len + home_len + hmac64_len + 4, expiry, expiry_len);
      response_buf[udid_len + home_len + hmac64_len + expiry_len + 4] = 0;
      memcpy(response_buf + udid_len + home_len + hmac64_len + expiry_len + 5, extra, extra_len);
      break;
    }

    default:
      amqp_abort("Invalid SASL method: %d", (int) method);
  }

  return response;
}

amqp_boolean_t amqp_frames_enqueued(amqp_connection_state_t state) {
  return (state->first_queued_frame != NULL);
}

/*
 * Check to see if we have data in our buffer. If this returns 1, we
 * will avoid an immediate blocking read in amqp_simple_wait_frame.
 */
amqp_boolean_t amqp_data_in_buffer(amqp_connection_state_t state) {
  return (state->sock_inbound_offset < state->sock_inbound_limit);
}

static int wait_frame_inner(amqp_connection_state_t state,
			    amqp_frame_t *decoded_frame)
{
  while (1) {
    int res;

    while (amqp_data_in_buffer(state)) {
      amqp_bytes_t buffer;
      buffer.len = state->sock_inbound_limit - state->sock_inbound_offset;
      buffer.bytes = ((char *) state->sock_inbound_buffer.bytes) + state->sock_inbound_offset;

      res = amqp_handle_input(state, buffer, decoded_frame);
      if (res < 0)
	return res;

      state->sock_inbound_offset += res;

      if (decoded_frame->frame_type != 0)
	/* Complete frame was read. Return it. */
	return 0;

      /* Incomplete or ignored frame. Keep processing input. */
      assert(res != 0);
    }

    res = state->recv(state->sockfd, state->sock_inbound_buffer.bytes,
		      state->sock_inbound_buffer.len, 0, state->user_data);
    if (res <= 0) {
      if (res == 0)
	return -ERROR_CONNECTION_CLOSED;
      else
	return -state->error(state->user_data);
    }

    state->sock_inbound_limit = res;
    state->sock_inbound_offset = 0;
  }
}

int amqp_simple_wait_frame(amqp_connection_state_t state,
			   amqp_frame_t *decoded_frame)
{
  if (state->first_queued_frame != NULL) {
    amqp_frame_t *f = (amqp_frame_t *) state->first_queued_frame->data;
    state->first_queued_frame = state->first_queued_frame->next;
    if (state->first_queued_frame == NULL) {
      state->last_queued_frame = NULL;
    }
    *decoded_frame = *f;
    return 0;
  } else {
    return wait_frame_inner(state, decoded_frame);
  }
}

int amqp_simple_wait_method(amqp_connection_state_t state,
			    amqp_channel_t expected_channel,
			    amqp_method_number_t expected_method,
			    amqp_method_t *output)
{
  amqp_frame_t frame;
  int res = amqp_simple_wait_frame(state, &frame);
  if (res < 0)
    return res;

  if (frame.channel != expected_channel)
    amqp_abort("Expected 0x%08X method frame on channel %d, got frame on channel %d",
	       expected_method,
	       expected_channel,
	       frame.channel);
  if (frame.frame_type != AMQP_FRAME_METHOD)
    amqp_abort("Expected 0x%08X method frame on channel %d, got frame type %d",
	       expected_method,
	       expected_channel,
	       frame.frame_type);
  if (frame.payload.method.id != expected_method)
    amqp_abort("Expected method ID 0x%08X on channel %d, got ID 0x%08X",
	       expected_method,
	       expected_channel,
	       frame.payload.method.id);
  *output = frame.payload.method;
  return 0;
}

int amqp_send_method(amqp_connection_state_t state,
		     amqp_channel_t channel,
		     amqp_method_number_t id,
		     void *decoded)
{
  amqp_frame_t frame;

  frame.frame_type = AMQP_FRAME_METHOD;
  frame.channel = channel;
  frame.payload.method.id = id;
  frame.payload.method.decoded = decoded;
  return amqp_send_frame(state, &frame);
}

static int amqp_id_in_reply_list( amqp_method_number_t expected, amqp_method_number_t *list )
{
  while ( *list != 0 ) {
    if ( *list == expected ) return 1;
    list++;
  }
  return 0;
}

amqp_rpc_reply_t amqp_simple_rpc(amqp_connection_state_t state,
				 amqp_channel_t channel,
				 amqp_method_number_t request_id,
				 amqp_method_number_t *expected_reply_ids,
				 void *decoded_request_method)
{
  int status;
  amqp_rpc_reply_t result;

  memset(&result, 0, sizeof(result));

  status = amqp_send_method(state, channel, request_id, decoded_request_method);
  if (status < 0) {
    result.reply_type = AMQP_RESPONSE_LIBRARY_EXCEPTION;
    result.library_error = -status;
    return result;
  }

  {
    amqp_frame_t frame;

  retry:
    status = wait_frame_inner(state, &frame);
    if (status < 0) {
      result.reply_type = AMQP_RESPONSE_LIBRARY_EXCEPTION;
      result.library_error = -status;
      return result;
    }

    /*
     * We store the frame for later processing unless it's something
     * that directly affects us here, namely a method frame that is
     * either
     *  - on the channel we want, and of the expected type, or
     *  - on the channel we want, and a channel.close frame, or
     *  - on channel zero, and a connection.close frame.
     */
    if (!( (frame.frame_type == AMQP_FRAME_METHOD) &&
	   (   ((frame.channel == channel) &&
		((amqp_id_in_reply_list(frame.payload.method.id, expected_reply_ids)) ||
		 (frame.payload.method.id == AMQP_CHANNEL_CLOSE_METHOD)))
	    ||
	       ((frame.channel == 0) &&
		(frame.payload.method.id == AMQP_CONNECTION_CLOSE_METHOD))   ) ))
    {
      amqp_frame_t *frame_copy = amqp_pool_alloc(&state->decoding_pool, sizeof(amqp_frame_t));
      amqp_link_t *link = amqp_pool_alloc(&state->decoding_pool, sizeof(amqp_link_t));

      if (frame_copy == NULL || link == NULL) {
	result.reply_type = AMQP_RESPONSE_LIBRARY_EXCEPTION;
	result.library_error = ERROR_NO_MEMORY;
	return result;
      }

      *frame_copy = frame;

      link->next = NULL;
      link->data = frame_copy;

      if (state->last_queued_frame == NULL) {
	state->first_queued_frame = link;
      } else {
	state->last_queued_frame->next = link;
      }
      state->last_queued_frame = link;

      goto retry;
    }

    result.reply_type = (amqp_id_in_reply_list(frame.payload.method.id, expected_reply_ids))
      ? AMQP_RESPONSE_NORMAL
      : AMQP_RESPONSE_SERVER_EXCEPTION;

    result.reply = frame.payload.method;
    return result;
  }
}

void *amqp_simple_rpc_decoded(amqp_connection_state_t state,
			      amqp_channel_t channel,
			      amqp_method_number_t request_id,
			      amqp_method_number_t reply_id,
			      void *decoded_request_method)
{
  amqp_method_number_t replies[2];

  replies[0] = reply_id;
  replies[1] = 0;

  state->most_recent_api_result = amqp_simple_rpc(state, channel,
						  request_id, replies,
						  decoded_request_method);
  if (state->most_recent_api_result.reply_type == AMQP_RESPONSE_NORMAL)
    return state->most_recent_api_result.reply.decoded;
  else
    return NULL;
}

amqp_rpc_reply_t amqp_get_rpc_reply(amqp_connection_state_t state)
{
  return state->most_recent_api_result;
}


int evo_get_version(char *buffer, int bufsize) {
  FILE *pf = popen("/usr/libexec/evo/evo-hub --version", "r");
  if (pf == NULL) {
    return -1;
  }
  fgets(buffer, bufsize, pf);
  buffer[(strlen(buffer) -1)] = '\0';

  pclose(pf);
  return 0;
}


static int amqp_login_inner(amqp_connection_state_t state,
			    int channel_max,
			    int frame_max,
			    int heartbeat,
			    amqp_sasl_method_enum sasl_method,
			    va_list vl)
{
  int res;
  amqp_method_t method;
  int server_frame_max;
  uint16_t server_channel_max;
  uint16_t server_heartbeat;

  amqp_send_header(state);

  res = amqp_simple_wait_method(state, 0, AMQP_CONNECTION_START_METHOD,
				&method);
  if (res < 0)
    return res;

  {
    amqp_connection_start_t *s = (amqp_connection_start_t *) method.decoded;
    if ((s->version_major != AMQP_PROTOCOL_VERSION_MAJOR) ||
	(s->version_minor != AMQP_PROTOCOL_VERSION_MINOR)) {
      return -ERROR_INCOMPATIBLE_AMQP_VERSION;
    }

    /* TODO: check that our chosen SASL mechanism is in the list of
       acceptable mechanisms. Or even let the application choose from
       the list! */
  }

  {
    char buffer_evo_version[BUFSIZ];
    int evo_ver_res = -1;

    amqp_table_entry_t properties[3];
    amqp_connection_start_ok_t s;
    amqp_bytes_t response_bytes = sasl_response(&state->decoding_pool,
						sasl_method, vl);

    if (response_bytes.bytes == NULL)
      return -ERROR_NO_MEMORY;

    properties[0].key = amqp_cstring_bytes("product");
    properties[0].value.kind = AMQP_FIELD_KIND_UTF8;
    properties[0].value.value.bytes
      = amqp_cstring_bytes("rabbitmq-c");

    properties[1].key = amqp_cstring_bytes("information");
    properties[1].value.kind = AMQP_FIELD_KIND_UTF8;
    properties[1].value.value.bytes
      = amqp_cstring_bytes("See http://hg.rabbitmq.com/rabbitmq-c/");

    memset(buffer_evo_version, 0, BUFSIZ);
    evo_ver_res = evo_get_version(buffer_evo_version, BUFSIZ);
    properties[2].key = amqp_cstring_bytes("evo_version");
    properties[2].value.kind = AMQP_FIELD_KIND_UTF8;
    if(evo_ver_res < 0){
      properties[2].value.value.bytes
        = amqp_cstring_bytes("0.0.0");
    }
    else {
      properties[2].value.value.bytes
        = amqp_cstring_bytes(buffer_evo_version);
    }

    s.client_properties.num_entries = 3;
    s.client_properties.entries = properties;
    s.mechanism = sasl_method_name(sasl_method);
    s.response = response_bytes;
    s.locale.bytes = "en_US";
    s.locale.len = 5;

    res = amqp_send_method(state, 0, AMQP_CONNECTION_START_OK_METHOD, &s);
    if (res < 0)
      return res;
  }

  amqp_release_buffers(state);

  res = amqp_simple_wait_method(state, 0, AMQP_CONNECTION_TUNE_METHOD,
				&method);
  if (res < 0)
    return res;

  {
    amqp_connection_tune_t *s = (amqp_connection_tune_t *) method.decoded;
    server_channel_max = s->channel_max;
    server_frame_max = s->frame_max;
    server_heartbeat = s->heartbeat;
  }

  if (server_channel_max != 0 && server_channel_max < channel_max)
    channel_max = server_channel_max;

  if (server_frame_max != 0 && server_frame_max < frame_max)
    frame_max = server_frame_max;

  if (server_heartbeat != 0 && server_heartbeat < heartbeat)
    heartbeat = server_heartbeat;

  res = amqp_tune_connection(state, channel_max, frame_max, heartbeat);
  if (res < 0)
    return res;

  {
    amqp_connection_tune_ok_t s;
    s.frame_max = frame_max;
    s.channel_max = channel_max;
    s.heartbeat = heartbeat;

    res = amqp_send_method(state, 0, AMQP_CONNECTION_TUNE_OK_METHOD, &s);
    if (res < 0)
      return res;
  }

  amqp_release_buffers(state);

  return 0;
}

amqp_rpc_reply_t amqp_login(amqp_connection_state_t state,
			    char const *vhost,
			    int channel_max,
			    int frame_max,
			    int heartbeat,
			    amqp_sasl_method_enum sasl_method,
			    ...)
{
  va_list vl;
  amqp_rpc_reply_t result;
  int status;

  va_start(vl, sasl_method);

  status = amqp_login_inner(state, channel_max, frame_max, heartbeat, sasl_method, vl);
  if (status < 0) {
    result.reply_type = AMQP_RESPONSE_LIBRARY_EXCEPTION;
    result.reply.id = 0;
    result.reply.decoded = NULL;
    result.library_error = -status;
    return result;
  }

  {
    amqp_method_number_t replies[] = { AMQP_CONNECTION_OPEN_OK_METHOD, 0 };
    amqp_connection_open_t s;
    s.virtual_host = amqp_cstring_bytes(vhost);
    s.capabilities.len = 0;
    s.capabilities.bytes = NULL;
    s.insist = 1;

    result = amqp_simple_rpc(state,
			     0,
			     AMQP_CONNECTION_OPEN_METHOD,
			     (amqp_method_number_t *) &replies,
			     &s);
    if (result.reply_type != AMQP_RESPONSE_NORMAL)
      return result;
  }
  amqp_maybe_release_buffers(state);

  va_end(vl);

  result.reply_type = AMQP_RESPONSE_NORMAL;
  result.reply.id = 0;
  result.reply.decoded = NULL;
  result.library_error = 0;
  return result;
}

char b64string[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

long base64_encode (to, from, len)
char *to, *from;
unsigned int len;
{
  char *fromp = from;
  char *top = to;
  unsigned char cbyte;
  unsigned char obyte;
  char end[3];

  for (; len >= 3; len -= 3) {
    cbyte = *fromp++;
    *top++ = b64string[(int)(cbyte >> 2)];
    obyte = (cbyte << 4) & 0x30;            /* 0011 0000 */

    cbyte = *fromp++;
    obyte |= (cbyte >> 4);                  /* 0000 1111 */
    *top++ = b64string[(int)obyte];
    obyte = (cbyte << 2) & 0x3C;            /* 0011 1100 */

    cbyte = *fromp++;
    obyte |= (cbyte >> 6);                  /* 0000 0011 */
    *top++ = b64string[(int)obyte];
    *top++ = b64string[(int)(cbyte & 0x3F)];/* 0011 1111 */
  }

  if (len) {
    end[0] = *fromp++;
    if (--len) end[1] = *fromp++; else end[1] = 0;
    end[2] = 0;

    cbyte = end[0];
    *top++ = b64string[(int)(cbyte >> 2)];
    obyte = (cbyte << 4) & 0x30;            /* 0011 0000 */

    cbyte = end[1];
    obyte |= (cbyte >> 4);
    *top++ = b64string[(int)obyte];
    obyte = (cbyte << 2) & 0x3C;            /* 0011 1100 */

    if (len) *top++ = b64string[(int)obyte];
    else *top++ = '=';
    *top++ = '=';
  }
  *top = 0;
  return top - to;
}

void evoz_signature(char* signature, const char* udid, const char* home, const char* serial, const char* key, long long now, char* expiryString) {
  uint64_t expiry = now + 300;
  sprintf(expiryString, "%qd", expiry);

  char res[SHA_DIGEST_LENGTH];
  unsigned int len = sizeof(res);

  char* stringToSign = malloc(strlen(udid) + strlen(home) + strlen(serial) + 24);
  sprintf(stringToSign, "%s\n%s\n%s\n%s", udid, home, serial, expiryString);
  HMAC(EVP_sha1(), (unsigned char*) key, (int) strlen(key), (unsigned char*) stringToSign, strlen(stringToSign), (unsigned char*) res, &len);
  free(stringToSign);
  base64_encode(signature, res, len);
}
