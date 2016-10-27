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

#include <grpc/support/cmdline.h>
#include <grpc/support/log.h>
#include <grpc/support/time.h>
#include <grpc/support/useful.h>
#include "test/core/util/memory_counters.h"
#include "test/core/util/test_config.h"

static grpc_channel *channel;
static grpc_completion_queue *cq;
static grpc_call *calls[4000];
static grpc_op metadata_ops[2];
static grpc_op status_ops[2];
static grpc_op close_ops[2];
static grpc_metadata_array initial_metadata_recv;
static grpc_op *op;
static grpc_status_code status;
static char *details = NULL;
static size_t details_capacity = 0;
static grpc_metadata_array trailing_metadata_recv;

static void *tag(intptr_t t) { return (void *)t; }

static void init_ping_pong_request(int call_idx) {
  grpc_metadata_array_init(&initial_metadata_recv);

  memset(metadata_ops, 0, sizeof(metadata_ops));
  op = metadata_ops;

  op->op = GRPC_OP_SEND_INITIAL_METADATA;
  op->data.send_initial_metadata.count = 0;
  op++;
  op->op = GRPC_OP_RECV_INITIAL_METADATA;
  op->data.recv_initial_metadata = &initial_metadata_recv;
  op++;
  calls[call_idx] = grpc_channel_create_call(
      channel, NULL, GRPC_PROPAGATE_DEFAULTS, cq, "/Reflector/reflectUnary",
      "localhost", gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(calls[call_idx],
                                                   metadata_ops, 2, tag(call_idx),
                                                   NULL));
  grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
}

static void step_ping_pong_request(int call_idx) {
  memset(close_ops, 0, sizeof(close_ops));
  op = close_ops;

  op->op = GRPC_OP_SEND_CLOSE_FROM_CLIENT;
  op++;

  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(calls[call_idx],
                                                   close_ops, 1, tag(call_idx),
                                                   NULL));
  grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
}

static void finish_ping_pong_request(int call_idx) {
  memset(status_ops, 0, sizeof(status_ops));
  op = status_ops;

  op->op = GRPC_OP_RECV_STATUS_ON_CLIENT;
  op->data.recv_status_on_client.trailing_metadata = &trailing_metadata_recv;
  op->data.recv_status_on_client.status = &status;
  op->data.recv_status_on_client.status_details = &details;
  op->data.recv_status_on_client.status_details_capacity = &details_capacity;
  op++;

  GPR_ASSERT(GRPC_CALL_OK == grpc_call_start_batch(calls[call_idx],
                                                   status_ops, 1, tag(call_idx),
                                                   NULL));
  grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME), NULL);
  grpc_call_destroy(calls[call_idx]);
  calls[call_idx] = NULL;
}

typedef struct {
  const char *name;
  void (*init)();
  void (*do_one_step)();
  void (*finish_one_step)();
} scenario;

static const scenario scenarios[] = {
    {"ping-pong-request", init_ping_pong_request, step_ping_pong_request, finish_ping_pong_request},
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
  scenario sc = {NULL, NULL, NULL, NULL};

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
  cq = grpc_completion_queue_create(NULL);

  struct grpc_memory_counters channel_start = grpc_memory_counters_snapshot();
  channel = grpc_insecure_channel_create(target, NULL, NULL);

  int call_idx;
  // warmup period
  for (call_idx = 0; call_idx < warmup_iterations; ++call_idx) {
    sc.init(call_idx);
  }

  struct grpc_memory_counters calls_start = grpc_memory_counters_snapshot();
  // benchmark period
  for (; call_idx < warmup_iterations + benchmark_iterations; ++call_idx) {
    sc.init(call_idx);
  }

  gpr_log(GPR_INFO, "memory usage: %f bytes per call",
          (double)(grpc_memory_counters_snapshot().total_size_relative -
                   calls_start.total_size_relative) /
              benchmark_iterations);

  // destroy calls
  for (call_idx = 0; call_idx < warmup_iterations + benchmark_iterations;
       ++call_idx) {
    sc.do_one_step(call_idx);
  }

  for (call_idx = 0; call_idx < warmup_iterations + benchmark_iterations;
       ++call_idx) {
    sc.finish_one_step(call_idx);
  }

  struct grpc_memory_counters channel_end = grpc_memory_counters_snapshot();
  gpr_log(
      GPR_INFO, "channel mem %zi bytes %zi.",
      channel_end.total_size_relative - channel_start.total_size_relative,
      channel_end.total_allocs_relative - channel_start.total_allocs_relative);

  grpc_channel_destroy(channel);

  grpc_completion_queue_shutdown(cq);

  do {
    event = grpc_completion_queue_next(cq, gpr_inf_future(GPR_CLOCK_REALTIME),
                                       NULL);
  } while (event.type != GRPC_QUEUE_SHUTDOWN);

  grpc_completion_queue_destroy(cq);
  gpr_slice_unref(slice);
  grpc_shutdown();

  gpr_log(GPR_INFO, "The end: relative %zi, %zi",
          grpc_memory_counters_snapshot().total_allocs_relative,
          grpc_memory_counters_snapshot().total_size_relative);
  grpc_memory_counters_destroy();
  return 0;
}
