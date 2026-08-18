#pragma once
/*
 * WS2812B signal capture — decode types and algorithm.
 *
 * Decode algorithm adapted from FastLED (MIT License).
 * Copyright (c) 2013 FastLED — https://github.com/FastLED/FastLED
 *
 * Adapted for WLED ARGB passthrough: stripped FastLED dependencies,
 * uses plain C types, operates on ESP-IDF rmt_symbol_word_t input.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 4-byte packed edge representation (from FastLED EdgeTime) */
typedef struct {
    uint32_t ns   : 31;  /* duration in nanoseconds (max ~2.1s) */
    uint32_t high : 1;   /* 1 = HIGH level, 0 = LOW level */
} argb_edge_t;

/* 4-phase timing windows for WS2812B decode with tolerance ranges */
typedef struct {
    uint32_t t0h_min_ns, t0h_max_ns;  /* bit-0 HIGH window */
    uint32_t t0l_min_ns, t0l_max_ns;  /* bit-0 LOW window */
    uint32_t t1h_min_ns, t1h_max_ns;  /* bit-1 HIGH window */
    uint32_t t1l_min_ns, t1l_max_ns;  /* bit-1 LOW window */
    uint32_t reset_min_us;            /* reset pulse threshold (microseconds) */
} argb_timing_t;

/* Decode result */
typedef struct {
    uint32_t bytes_written;  /* number of decoded bytes (3 per LED, GRB order) */
    uint32_t error_count;    /* edge-pair errors encountered */
    uint8_t  error;          /* 0=ok, 1=invalid_arg, 2=buffer_overflow, 3=high_error_rate */
} argb_decode_result_t;

#define ARGB_DECODE_OK              0
#define ARGB_DECODE_INVALID_ARG     1
#define ARGB_DECODE_BUFFER_OVERFLOW 2
#define ARGB_DECODE_HIGH_ERROR_RATE 3

/*
 * Default WS2812B timing with ±150ns tolerance.
 * Nominal: T0H=400ns T0L=850ns T1H=800ns T1L=450ns Reset≥50us
 */
static inline argb_timing_t argb_timing_ws2812b(void) {
    argb_timing_t t;
    t.t0h_min_ns = 250;   t.t0h_max_ns = 550;
    t.t0l_min_ns = 700;   t.t0l_max_ns = 1000;
    t.t1h_min_ns = 650;   t.t1h_max_ns = 950;
    t.t1l_min_ns = 300;   t.t1l_max_ns = 600;
    t.reset_min_us = 50;
    return t;
}

/*
 * Decode edge pairs into bytes using 4-phase timing windows.
 * Adapted from FastLED decodeWs2812Edges() — MIT licensed.
 *
 * edges:     array of edge timestamps (HIGH/LOW pairs)
 * edge_count: number of edges
 * out:       output byte buffer (GRB order, 3 bytes per LED)
 * out_size:  size of output buffer in bytes
 *
 * Returns decode result with bytes_written, error_count, error code.
 */
static inline argb_decode_result_t argb_decode_edges(
    const argb_timing_t *timing,
    const argb_edge_t *edges, size_t edge_count,
    uint8_t *out, size_t out_size)
{
    argb_decode_result_t r = {0, 0, ARGB_DECODE_OK};

    if (!edges || edge_count == 0 || !out || out_size == 0) {
        r.error = ARGB_DECODE_INVALID_ARG;
        return r;
    }

    uint8_t current_byte = 0;
    uint8_t bit_index = 0;
    size_t i = 0;

    while (i + 1 < edge_count) {
        const argb_edge_t high_edge = edges[i];
        const argb_edge_t low_edge  = edges[i + 1];

        /* Validate edge polarity: expect HIGH then LOW */
        if (!high_edge.high || low_edge.high) {
            r.error_count++;
            i++;  /* advance by 1 to re-sync (not 2) */
            continue;
        }

        uint32_t high_ns = high_edge.ns;
        uint32_t low_ns  = low_edge.ns;

        /* Reset pulse terminates frame */
        if (low_ns >= (uint32_t)timing->reset_min_us * 1000u) break;
        if (high_ns >= (uint32_t)timing->reset_min_us * 1000u) break;

        /* Match against 4-phase timing windows */
        bool is_bit1 = (high_ns >= timing->t1h_min_ns && high_ns <= timing->t1h_max_ns &&
                        low_ns  >= timing->t1l_min_ns && low_ns  <= timing->t1l_max_ns);
        bool is_bit0 = (high_ns >= timing->t0h_min_ns && high_ns <= timing->t0h_max_ns &&
                        low_ns  >= timing->t0l_min_ns && low_ns  <= timing->t0l_max_ns);

        if (!is_bit0 && !is_bit1) {
            r.error_count++;
            i += 2;
            continue;
        }

        /* Accumulate bits MSB-first */
        current_byte = (current_byte << 1) | (is_bit1 ? 1 : 0);
        if (++bit_index == 8) {
            if (r.bytes_written >= out_size) {
                r.error = ARGB_DECODE_BUFFER_OVERFLOW;
                return r;
            }
            out[r.bytes_written++] = current_byte;
            current_byte = 0;
            bit_index = 0;
        }
        i += 2;
    }

    /* Error rate gate: reject if >10% of decoded bits had errors */
    if (r.bytes_written > 0 && r.error_count * 10 > r.bytes_written * 8) {
        r.error = ARGB_DECODE_HIGH_ERROR_RATE;
    }

    return r;
}

/*
 * Convert ESP-IDF rmt_symbol_word_t array to argb_edge_t array.
 * Each RMT symbol has two phases (duration0/level0, duration1/level1).
 * resolution_hz is the RMT clock (e.g. 10000000 for 10MHz = 100ns/tick).
 *
 * out_edges must have space for sym_count * 2 edges.
 * Returns number of edges written.
 */
static inline size_t argb_rmt_to_edges(
    const void *symbols, size_t sym_count,
    uint32_t resolution_hz,
    argb_edge_t *out_edges, size_t max_edges)
{
    /* rmt_symbol_word_t layout: duration0:15, level0:1, duration1:15, level1:1 */
    typedef struct { uint32_t val; } rmt_sym_t;
    const rmt_sym_t *syms = (const rmt_sym_t *)symbols;

    uint32_t ns_per_tick = 1000000000u / resolution_hz;
    size_t ei = 0;

    for (size_t i = 0; i < sym_count && ei + 1 < max_edges; i++) {
        uint32_t v = syms[i].val;
        uint16_t dur0 = v & 0x7FFF;
        uint8_t  lvl0 = (v >> 15) & 1;
        uint16_t dur1 = (v >> 16) & 0x7FFF;
        uint8_t  lvl1 = (v >> 31) & 1;

        if (dur0 == 0 && dur1 == 0) break;  /* end marker */

        out_edges[ei].ns   = dur0 * ns_per_tick;
        out_edges[ei].high = lvl0;
        ei++;

        if (dur1 > 0) {
            out_edges[ei].ns   = dur1 * ns_per_tick;
            out_edges[ei].high = lvl1;
            ei++;
        }
    }
    return ei;
}

#ifdef __cplusplus
}
#endif
