/*
 *
 * Copyright 2015, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <grpc/grpc.h>
#include <grpc/grpc_security.h>

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
/* This is for _exit() below, which is temporary. */
#include <unistd.h>
#endif

#include <grpc/support/alloc.h>
#include <grpc/support/cmdline.h>
#include <grpc/support/host_port.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include "test/core/end2end/data/ssl_test_data.h"
#include "test/core/util/memory_counters.h"
#include "test/core/util/port.h"
#include "test/core/util/test_config.h"

static grpc_completion_queue *cq;
static grpc_server *server;
static grpc_metadata_array request_metadata_recv;
static grpc_metadata_array initial_metadata_send;
static grpc_op metadata_send_op[2];
static grpc_op receive_close_op;
static grpc_op send_status_op;
static int got_sigint = 0;
// static grpc_byte_buffer *terminal_buffer = NULL;

static void *tag(intptr_t t) { return (void *)t; }

typedef enum {
  FLING_SERVER_NEW_REQUEST = 1,
  FLING_SERVER_SEND_INIT_METADATA_FOR_UNARY,
  FLING_SERVER_RECEIVE_CLOSE,
  FLING_SERVER_SEND_STATUS,
} fling_server_tags;

typedef struct {
  fling_server_tags state;
  grpc_call *call;
  grpc_call_details call_details;
  grpc_byte_buffer *terminal_buffer;
} fling_call;

static fling_call calls[10001];

static void request_call_unary(int k) {
  if (k == (int)(sizeof(calls) / sizeof(fling_call))) {
    gpr_log(GPR_INFO, "Used all call slots (10000) on server. Server exit.");
    _exit(0);
  }
  grpc_metadata_array_init(&request_metadata_recv);
  grpc_server_request_call(server, &calls[k].call, &calls[k].call_details,
                           &request_metadata_recv, cq, cq, &calls[k]);
}

static void send_initial_metadata_unary(void *tag) {
  grpc_call_error error;
  grpc_metadata_array_init(&initial_metadata_send);
  metadata_send_op[0].op = GRPC_OP_SEND_INITIAL_METADATA;
  metadata_send_op[0].data.send_initial_metadata.count = 0;

  error = grpc_call_start_batch((*(fling_call *)tag).call, metadata_send_op, 1,
                                tag, NULL);

  GPR_ASSERT(GRPC_CALL_OK == error);
}

static void receive_close(void *tag) {
  grpc_call_error error;

  receive_close_op.op = GRPC_OP_RECV_MESSAGE;
  receive_close_op.data.recv_message = &(*(fling_call *)tag).terminal_buffer;

  error = grpc_call_start_batch((*(fling_call *)tag).call, &receive_close_op, 1,
                                tag, NULL);

  GPR_ASSERT(GRPC_CALL_OK == error);
}

static void send_status(void *tag) {
  grpc_call_error error;
  send_status_op.op = GRPC_OP_SEND_STATUS_FROM_SERVER;
  send_status_op.data.send_status_from_server.status = GRPC_STATUS_OK;
  send_status_op.data.send_status_from_server.trailing_metadata_count = 0;
  send_status_op.data.send_status_from_server.status_details = "";

  error = grpc_call_start_batch((*(fling_call *)tag).call, &send_status_op, 1,
                                tag, NULL);

  GPR_ASSERT(GRPC_CALL_OK == error);
}
/* We have some sort of deadlock, so let's not exit gracefully for now.
   When that is resolved, please remove the #include <unistd.h> above. */
static void sigint_handler(int x) { _exit(0); }

int main(int argc, char **argv) {
  grpc_event ev;
  char *addr_buf = NULL;
  gpr_cmdline *cl;
  int shutdown_started = 0;
  int shutdown_finished = 0;

  int secure = 0;
  char *addr = NULL;

  char *fake_argv[1];

  GPR_ASSERT(argc >= 1);
  fake_argv[0] = argv[0];
  grpc_test_init(1, fake_argv);

  grpc_init();
  srand((unsigned)clock());

  cl = gpr_cmdline_create("fling server");
  gpr_cmdline_add_string(cl, "bind", "Bind host:port", &addr);
  gpr_cmdline_add_flag(cl, "secure", "Run with security?", &secure);
  gpr_cmdline_parse(cl, argc, argv);
  gpr_cmdline_destroy(cl);

  if (addr == NULL) {
    gpr_join_host_port(&addr_buf, "::", grpc_pick_unused_port_or_die());
    addr = addr_buf;
  }
  gpr_log(GPR_INFO, "creating server on: %s", addr);

  cq = grpc_completion_queue_create(NULL);

  struct grpc_memory_counters before_server_create =
      grpc_memory_counters_snapshot();
  if (secure) {
    grpc_ssl_pem_key_cert_pair pem_key_cert_pair = {test_server1_key,
                                                    test_server1_cert};
    grpc_server_credentials *ssl_creds = grpc_ssl_server_credentials_create(
        NULL, &pem_key_cert_pair, 1, 0, NULL);
    server = grpc_server_create(NULL, NULL);
    GPR_ASSERT(grpc_server_add_secure_http2_port(server, addr, ssl_creds));
    grpc_server_credentials_release(ssl_creds);
  } else {
    server = grpc_server_create(NULL, NULL);
    GPR_ASSERT(grpc_server_add_insecure_http2_port(server, addr));
  }
  struct grpc_memory_counters after_server_create =
      grpc_memory_counters_snapshot();

  gpr_log(GPR_INFO, "grpc server create %zi bytes",
          after_server_create.total_size_relative -
              before_server_create.total_size_relative);
  grpc_server_register_completion_queue(server, cq, NULL);
  grpc_server_start(server);

  gpr_free(addr_buf);
  addr = addr_buf = NULL;

  // initiallize fling_call instances
  for (int i = 0; i < (int)(sizeof(calls) / sizeof(fling_call)); i++) {
    grpc_call_details_init(&calls[i].call_details);
    calls[i].state = FLING_SERVER_NEW_REQUEST;
  }

  int next_call_idx = 0;
  request_call_unary(next_call_idx);

  signal(SIGINT, sigint_handler);
  while (!shutdown_finished) {
    if (got_sigint && !shutdown_started) {
      gpr_log(GPR_INFO, "Shutting down due to SIGINT");
      grpc_server_shutdown_and_notify(server, cq, tag(1000));
      GPR_ASSERT(grpc_completion_queue_pluck(
                     cq, tag(1000), GRPC_TIMEOUT_SECONDS_TO_DEADLINE(5), NULL)
                     .type == GRPC_OP_COMPLETE);
      grpc_completion_queue_shutdown(cq);
      shutdown_started = 1;
    }
    ev = grpc_completion_queue_next(
        cq, gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                         gpr_time_from_micros(1000000, GPR_TIMESPAN)),
        NULL);
    fling_call *s = ev.tag;

    switch (ev.type) {
      case GRPC_OP_COMPLETE:
        switch (s->state) {
          case FLING_SERVER_NEW_REQUEST:
            request_call_unary(++next_call_idx);
            s->state = FLING_SERVER_SEND_INIT_METADATA_FOR_UNARY;
            send_initial_metadata_unary(s);
            break;
          case FLING_SERVER_SEND_INIT_METADATA_FOR_UNARY:
            s->state = FLING_SERVER_RECEIVE_CLOSE;
            receive_close(s);
            break;
          case FLING_SERVER_RECEIVE_CLOSE:
            s->state = FLING_SERVER_SEND_STATUS;
            send_status(s);
            break;
          case FLING_SERVER_SEND_STATUS:
            grpc_call_destroy(s->call);
            grpc_call_details_destroy(&s->call_details);
            break;
        }
        break;
      case GRPC_QUEUE_SHUTDOWN:
        GPR_ASSERT(shutdown_started);
        shutdown_finished = 1;
        break;
      case GRPC_QUEUE_TIMEOUT:
        break;
    }
  }

  grpc_server_destroy(server);
  grpc_completion_queue_destroy(cq);
  grpc_shutdown();
  return 0;
}
