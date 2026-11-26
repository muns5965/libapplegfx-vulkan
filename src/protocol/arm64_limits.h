/* Binding limits shared by the pipeline resolver, the exec descriptor writer and tools/tests. SPDX-License-Identifier: MIT */
#pragma once
#define LAGFX_A64_MAX_BINDINGS 48u   /* bindings of one pipeline, both stages together (descriptor writes are stack arrays of this size) */
#define LAGFX_A64_MAX_REFLECT 32u    /* bindings reflected from ONE stage module */
#define LAGFX_A64_MAX_ARGS 64u       /* Metal argument records (buffer / texture / sampler) read from ONE function's metadata; a real fragment function has 16+ (dump pso 30 / 46: 23) */
