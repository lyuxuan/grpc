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

#include <stdio.h>
#include <string.h>

#include <grpc/byte_buffer.h>
#include <grpc/byte_buffer_reader.h>
#include <grpc/support/alloc.h>
#include <grpc/support/cmdline.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>
#include "src/core/lib/support/string.h"
#include "test/core/util/memory_counters.h"
#include "test/core/util/test_config.h"
static grpc_channel *channel;
static grpc_completion_queue *cq;
static grpc_op metadata_ops[2];
static grpc_op status_ops[2];
static grpc_op snapshot_ops[6];
static grpc_op *op;

typedef struct {
  grpc_call *call;
  grpc_metadata_array initial_metadata_recv;
  grpc_status_code status;
  char *details;
  size_t details_capacity;
  grpc_metadata_array trailing_metadata_recv;
} fling_call;

static fling_call calls[10001];

static void *tag(intptr_t t) { return (void *)t; }

static void init_ping_pong_request(int call_idx) {
  grpc_metadata_array_init(&calls[call_idx].initial_metadata_recv);

  memset(metadata_ops, 0, sizeof(metadata_ops));
  op = metadata_ops;

  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op++;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata = &calls[call_idx].initial_metadata_recv;
  op++;
  calls[call_idx].call = grpc_channel_create_call(
      channel, NULL, GRPC_PROPAGATE_DEFAULTS, cq, "/Reflector/reflectUnary",
      "localhost", gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(calls[call_idx].call,
                                                   metadata_ops, 2,
                                                   tag(call_idx), NULL));
  grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
}

static void finish_ping_pong_request(int call_idx) {
  memset(status_ops, 0, sizeof(status_ops));
  op = status_ops;

  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata =
      &calls[call_idx].trailing_metadata_recv;
  op->data.recv_status_on_client.status = &calls[call_idx].status;
  op->data.recv_status_on_client.status_details = &calls[call_idx].details;
  op->data.recv_status_on_client.status_details_capacity =
      &calls[call_idx].details_capacity;
  op++;

  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(calls[call_idx].call,
                                                   status_ops, 1, tag(call_idx),
                                                   NULL));
  grpc_event ev =
      grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
  GPR_ASSERT(ev.type == GRPC_OP_COMPLETE);
  GPR_ASSERT(ev.tag == tag(call_idx));
  GPR_ASSERT(ev.success);
  gpr_free(calls[call_idx].details);
  grpc_call_destroy(calls[call_idx].call);
  calls[call_idx].call = NULL;
}

static struct grpc_memory_counters send_snapshot_request(
    int call_idx, const char *call_type) {
  grpc_metadata_array_init(&calls[call_idx].initial_metadata_recv);
  grpc_metadata_array_init(&calls[call_idx].trailing_metadata_recv);
  // gpr_slice request_payload_slice = gpr_slice_from_copied_string("X");
  // grpc_byte_buffer *request_payload =
  // grpc_raw_byte_buffer_create(&request_payload_slice, 1);
  grpc_byte_buffer *response_payload_recv = NULL;

  memset(snapshot_ops, 0, sizeof(snapshot_ops));
  op = snapshot_ops;

  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  // op++;
  // op->op = GRPC_OP_SEND_MESSAGE;
  // op->data.send_message = request_payload;
  op++;
  op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  op++;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata = &calls[call_idx].initial_metadata_recv;
  op++;
  op->op = GRPC_OP_RECV_MESSAGE;
  op->data.recv_message = &response_payload_recv;
  op++;
  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata =
      &calls[call_idx].trailing_metadata_recv;
  op->data.recv_status_on_client.status = &calls[call_idx].status;
  op->data.recv_status_on_client.status_details = &calls[call_idx].details;
  op->data.recv_status_on_client.status_details_capacity =
      &calls[call_idx].details_capacity;
  op++;

  calls[call_idx].call = grpc_channel_create_call(
      channel, NULL, GRPC_PROPAGATE_DEFAULTS, cq, call_type, "localhost",
      gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(
                                 calls[call_idx].call, snapshot_ops,
                                 (size_t)(op - snapshot_ops), (void *)1, NULL));
  grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);

  grpc_byte_buffer_reader reader;
  GPR_ASSERT(grpc_byte_buffer_reader_init(&reader, response_payload_recv));
  gpr_slice response = grpc_byte_buffer_reader_readall(&reader);

  struct grpc_memory_counters snapshot;
  snapshot.total_size_absolute =
      ((struct grpc_memory_counters *)GPR_SLICE_START_PTR(response))
          ->total_size_absolute;
  snapshot.total_allocs_absolute =
      ((struct grpc_memory_counters *)GPR_SLICE_START_PTR(response))
          ->total_allocs_absolute;
  snapshot.total_size_relative =
      ((struct grpc_memory_counters *)GPR_SLICE_START_PTR(response))
          ->total_size_relative;
  snapshot.total_allocs_relative =
      ((struct grpc_memory_counters *)GPR_SLICE_START_PTR(response))
          ->total_allocs_relative;

  GPR_ASSERT(snapshot.total_size_absolute > 0);
  GPR_ASSERT(snapshot.total_allocs_absolute > 0);
  GPR_ASSERT(snapshot.total_size_relative > 0);
  GPR_ASSERT(snapshot.total_allocs_relative > 0);
  // gpr_log(GPR_INFO, "abosolute %zi %zi", snapshot.total_size_absolute,
  // snapshot.total_allocs_absolute);
  // gpr_log(GPR_INFO, "relative %zi %zi", snapshot.total_size_relative,
  // snapshot.total_allocs_relative);

  // gpr_slice_unref(request_payload_slice);
  gpr_slice_unref(response);
  grpc_byte_buffer_reader_destroy(&reader);
  gpr_free(calls[call_idx].details);
  calls[call_idx].details = NULL;
  calls[call_idx].details_capacity = 0;
  grpc_call_destroy(calls[call_idx].call);
  // grpc_byte_buffer_destroy(request_payload);
  grpc_byte_buffer_destroy(response_payload_recv);
  calls[call_idx].call = NULL;

  return snapshot;
}

typedef struct {
  const char *name;
  void (*init)();
  void (*finish_one_step)();
} scenario;

static const scenario scenarios[] = {
    {"ping-pong-request", init_ping_pong_request, finish_ping_pong_request},
};

int main(int argc, char **argv) {
  grpc_memory_counters_init();
  gpr_slice slice = gpr_slice_from_copied_string("x");
  unsigned i;

  char *fake_argv[1];

  int secure = 0;
  int payload_size;
  char *target = "localhost:443";
  gpr_cmdline *cl;
  grpc_event event;
  char *scenario_name = "ping-pong-request";
  scenario sc = {NULL, NULL, NULL};

  grpc_init();

  GPR_ASSERT(argc >= 1);
  fake_argv[0] = argv[0];
  grpc_test_init(1, fake_argv);

  int warmup_iterations = 100;
  int benchmark_iterations = 1000;

  cl = gpr_cmdline_create("fling client");
  gpr_cmdline_add_int(cl, "payload_size", "Size of the payload to send",
                      &payload_size);
  gpr_cmdline_add_string(cl, "target", "Target host:port", &target);
  gpr_cmdline_add_flag(cl, "secure", "Run with security?", &secure);
  gpr_cmdline_add_string(cl, "scenario", "Scenario", &scenario_name);
  gpr_cmdline_add_int(cl, "warmup", "Warmup iterations", &warmup_iterations);
  gpr_cmdline_add_int(cl, "benchmark", "Benchmark iterations",
                      &benchmark_iterations);
  gpr_cmdline_parse(cl, argc, argv);
  gpr_cmdline_destroy(cl);

  for (i = 0; i < GPR_ARRAY_SIZE(scenarios); i++) {
    if (0 == strcmp(scenarios[i].name, scenario_name)) {
      sc = scenarios[i];
    }
  }
  if (!sc.name) {
    fprintf(stderr, "unsupported scenario '%s'. Valid are:", scenario_name);
    for (i = 0; i < GPR_ARRAY_SIZE(scenarios); i++) {
      fprintf(stderr, " %s", scenarios[i].name);
    }
    return 1;
  }

  for (int k = 0; k < (int)(sizeof(calls) / sizeof(fling_call)); k++) {
    calls[k].details = NULL;
    calls[k].details_capacity = 0;
  }

  cq = grpc_completion_queue_create(NULL);

  struct grpc_memory_counters channel_start = grpc_memory_counters_snapshot();
  channel = grpc_insecure_channel_create(target, NULL, NULL);

  int call_idx = 0;

  struct grpc_memory_counters s = grpc_memory_counters_snapshot();
  struct grpc_memory_counters before_server_create =
      send_snapshot_request(0, "Reflector/GetBeforeSvrCreation");
  struct grpc_memory_counters after_server_create =
      send_snapshot_request(0, "Reflector/GetAfterSvrCreation");
  struct grpc_memory_counters before_calls_start =
      send_snapshot_request(0, "Reflector/BeforeCallsStart");
  struct grpc_memory_counters e = grpc_memory_counters_snapshot();
  gpr_log(GPR_INFO, "Reflector/CreateServer snaphot call cost %zi %zi",
          e.total_size_relative - s.total_size_relative,
          e.total_allocs_relative - s.total_allocs_relative);
  gpr_log(GPR_INFO, "create server %zi",
          after_server_create.total_size_relative -
              before_server_create.total_size_relative);

  // warmup period
  for (call_idx = 0; call_idx < warmup_iterations; ++call_idx) {
    sc.init(call_idx + 1);
  }

  s = grpc_memory_counters_snapshot();
  struct grpc_memory_counters server_calls_start =
      send_snapshot_request(0, "Reflector/SimpleSnapshot");
  e = grpc_memory_counters_snapshot();
  gpr_log(GPR_INFO, "Reflector/SimpleSnapshot snaphot call cost %zi %zi",
          e.total_size_relative - s.total_size_relative,
          e.total_allocs_relative - s.total_allocs_relative);

  struct grpc_memory_counters client_calls_start =
      grpc_memory_counters_snapshot();
  // benchmark period
  for (; call_idx < warmup_iterations + benchmark_iterations; ++call_idx) {
    // gpr_log(GPR_INFO, "-----%d", call_idx);
    sc.init(call_idx + 1);
  }

  gpr_log(GPR_INFO, "memory usage: %f bytes per call",
          (double)(grpc_memory_counters_snapshot().total_size_relative -
                   client_calls_start.total_size_relative) /
              benchmark_iterations);

  // send snapshot request

  s = grpc_memory_counters_snapshot();
  struct grpc_memory_counters server_calls_inflight =
      send_snapshot_request(0, "Reflector/DestroyCalls");
  e = grpc_memory_counters_snapshot();

  gpr_log(GPR_INFO, "%zu", server_calls_inflight.total_size_relative);
  gpr_log(GPR_INFO, "%zu", server_calls_inflight.total_size_relative);
  gpr_log(GPR_INFO, "%zu", server_calls_inflight.total_allocs_absolute);
  gpr_log(GPR_INFO, "%zu", server_calls_inflight.total_allocs_relative);

  gpr_log(GPR_INFO, "Reflector/DestroyCalls snaphot call cost %zi %zi %zi",
          e.total_size_relative - s.total_size_relative,
          e.total_allocs_relative - s.total_allocs_relative,
          e.total_allocs_absolute - s.total_allocs_absolute);

  grpc_event ev;
  do {
    ev = grpc_completion_queue_next(
        cq, gpr_time_add(gpr_now(GPR_CLOCK_REALTIME),
                         gpr_time_from_micros(1000000, GPR_TIMESPAN)),
        NULL);
    gpr_log(GPR_INFO, "event here %d %p", ev.type, ev.tag);
  } while (ev.type != GRPC_QUEUE_TIMEOUT);

  // s = grpc_memory_counters_snapshot();
  // struct grpc_memory_counters server_xx =
  // send_snapshot_request(0, "Reflector/SimpleSnapshot");
  // e = grpc_memory_counters_snapshot();
  // gpr_log(GPR_INFO, "Reflector/SimpleSnapshot snaphot call cost %zi %zi %zi",
  //         e.total_size_relative - s.total_size_relative,
  //         e.total_allocs_relative - s.total_allocs_relative,
  //         e.total_allocs_absolute - s.total_allocs_absolute);
  // gpr_log(GPR_INFO, "%zi", server_xx.total_size_relative);

  // destroy calls
  for (call_idx = 0; call_idx < warmup_iterations + benchmark_iterations;
       ++call_idx) {
    sc.finish_one_step(call_idx + 1);
  }

  s = grpc_memory_counters_snapshot();
  struct grpc_memory_counters server_calls_end =
      send_snapshot_request(0, "Reflector/SimpleSnapshot");
  e = grpc_memory_counters_snapshot();
  gpr_log(GPR_INFO, "Reflector/SimpleSnapshot snaphot call cost %zi %zi",
          e.total_size_relative - s.total_size_relative,
          e.total_allocs_relative - s.total_allocs_relative);

  struct grpc_memory_counters channel_end = grpc_memory_counters_snapshot();

  // size_t server_calls_mem = server_calls_end.total_size_relative -
  // server_calls_inflight.total_size_relative;
  // gpr_log(GPR_INFO, "server call: %zi", server_calls_mem);
  gpr_log(GPR_INFO, "%zi", client_calls_start.total_size_relative);
  gpr_log(GPR_INFO, "%zi", server_calls_start.total_size_relative);
  gpr_log(GPR_INFO, "%zi", server_calls_inflight.total_size_relative);
  gpr_log(GPR_INFO, "%zi", server_calls_end.total_size_relative);

  gpr_log(
      GPR_INFO, "channel mem %zi bytes %zi.",
      channel_end.total_size_relative - channel_start.total_size_relative,
      channel_end.total_allocs_relative - channel_start.total_allocs_relative);
  printf("----------------------------------------\n");
  grpc_channel_destroy(channel);

  grpc_completion_queue_shutdown(cq);

  do {
    event = grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME),
                                       NULL);
  } while (event.type != GRPC_QUEUE_SHUTDOWN);

  grpc_completion_queue_destroy(cq);
  gpr_slice_unref(slice);
  grpc_shutdown();

  gpr_log(GPR_INFO, "before server create %zi",
          before_server_create.total_size_relative);
  gpr_log(GPR_INFO, "after server create: %zi",
          after_server_create.total_size_relative);
  gpr_log(GPR_INFO, "after initial wamup %zi",
          server_calls_start.total_size_relative);
  gpr_log(GPR_INFO, "after benchmark calls initiated %zi",
          server_calls_inflight.total_size_relative);
  gpr_log(GPR_INFO, "server calls end %zi",
          server_calls_end.total_size_relative);
  gpr_log(GPR_INFO, "---------server stats--------");
  gpr_log(GPR_INFO, "server create: %zi bytes",
          after_server_create.total_size_relative -
              before_server_create.total_size_relative);
  gpr_log(GPR_INFO, "server per call cost: %d bytes",
          (int)(server_calls_inflight.total_size_relative -
                server_calls_start.total_size_relative) /
              benchmark_iterations);
  gpr_log(GPR_INFO, "server channel: %zi bytes",
          server_calls_end.total_size_relative -
              before_calls_start.total_size_relative);
  gpr_log(GPR_INFO, "before calls start: %zi bytes",
          before_calls_start.total_size_relative);
  gpr_log(GPR_INFO, "The end: relative %zi, %zi",
          grpc_memory_counters_snapshot().total_allocs_relative,
          grpc_memory_counters_snapshot().total_size_relative);
  grpc_memory_counters_destroy();
  return 0;
}
