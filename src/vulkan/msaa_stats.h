/*
 * libapplegfx-vulkan — multisample (MSAA) counters and the g7-gpu-msaa diagnostics line. No Vulkan types: included by protocol/state.h.
 * src/vulkan/msaa_stats.h
 *
 * SPDX-License-Identifier: MIT
 *
 * Every refusal / downgrade of the multisample path is counted here and printed: nothing is dropped, clamped or approximated silently.
 * Evidence level of the guest fields behind these counters is in docs/metal-vulkan-compatibility.md section 5 ("MSAA"): no guest capture of a multisampled pass exists yet.
 *
 * Policy (docs section 5): default "exact" mode uses the guest-requested sample count only when the device / format supports exactly that count, otherwise the multisampled texture
 * is refused (ms_exact_refused, ms_tex_refused) and a pass that names it skips the attachment (ms_pass_refused). Opt-in "compat" mode (LAGFX_MSAA_COMPAT=1, default OFF) permits a
 * clamp-down (ms_compat_clamped) and a 1x image with a resolve-by-copy (ms_compat_1x, resolves_by_copy): NOT exact MSAA. The line below always names the mode.
 */
#ifndef LAGFX_VULKAN_MSAA_STATS_H
#define LAGFX_VULKAN_MSAA_STATS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct lagfx_msaa_stats {
    /* textures (guest descriptor sample count > 1) */
    uint64_t ms_images_created;        /* device images made with more than one sample */
    uint64_t ms_compat_clamped;        /* COMPAT mode only: image granted with fewer samples than the guest asked for (largest supported lower power of two), still > 1: NOT exact */
    uint64_t ms_compat_1x;             /* COMPAT mode only: guest asked for > 1 sample, the image is single sample (no count > 1 usable for this use): NOT exact, "resolve" is a copy */
    uint64_t ms_exact_refused;         /* EXACT mode (default): the guest's sample count is not offered exactly (not a power of two <= 16, or the device / format / usage / extent lacks it): texture refused */
    uint64_t ms_tex_refused;           /* multisampled texture descriptors refused (exact count unavailable, no usable format, creation failed; bad shape is counted separately): the texture does not exist for the guest */
    uint64_t ms_tex_bad_shape;         /* descriptor shape the mirror does not model: sample count with a non-multisample type, multisample type with 1 sample, 2D multisample ARRAY, mips > 1, layers > 1 */
    /* passes */
    uint64_t ms_passes;                /* passes whose colour attachments are multisampled */
    uint64_t ms_attach;                /* multisampled colour attachments begun */
    uint64_t resolves;                 /* attachments resolved (dynamic-rendering resolve or the copy of a 1x fallback) */
    uint64_t resolves_by_copy;         /* ... of those, the 1x-fallback copy (a one-sample resolve is a copy) */
    uint64_t store_unknown;            /* store action not in the Metal table (4 Unknown after the deferred tags, 5, garbage): stored, not resolved */
    uint64_t resolve_no_target;        /* a resolve store action without a usable resolve texture (nil, not mirrored, shape / format / size mismatch): stored, not resolved */
    uint64_t resolve_on_1x;            /* a resolve store action on a single-sample attachment (invalid in Metal): treated as Store */
    uint64_t deferred_store_tags;      /* tags 0x66 (SetColorStoreAction) / 0x96 (SetColorResolveTexture) applied to the pass (payload layout from the x86-era table, NOT verified on ARM64) */
    uint64_t attach_mixed_ignored;     /* attachments of the pass with another sample count than the first colour attachment (Vulkan needs one count): ignored */
    uint64_t ds_mismatch_ignored;      /* a depth / stencil attachment whose sample count differs from the colour attachments: ignored */
    uint64_t ms_pass_refused;          /* a pass attachment skipped: it names a refused multisample texture, or (exact mode) an image whose host count differs from the guest's */
    uint64_t ds_resolve_unsupported;   /* UNSUPPORTED: a depth / stencil attachment with a resolve texture or a resolve store action: stored only, resolve not recorded */
    /* draws */
    uint64_t pipes_ms;                 /* pipelines built with rasterizationSamples > 1 (count taken from the pass: the pipeline descriptor field is not decoded) */
    uint64_t draws_ms;                 /* draws recorded into a multisampled pass */
    uint64_t sampled_ms_refused;       /* shader texture bindings of a multisampled texture refused (no AIR texture2d_ms read in the translator): draw skipped */
    uint64_t fbfetch_ms_variants, fbfetch_ms_reads;   /* multisampled framebuffer fetch: fragment modules rewritten (spv_msfetch.c) / OpImageReads changed */
    uint64_t fbfetch_ms_refused;       /* framebuffer-fetch draws in a multisampled pass refused (input attachment of a multisampled image needs a multisampled SubpassData type) */
    uint64_t tile_ms_refused;          /* tile-shader records in a multisampled pass refused (the pass would be suspended / resumed around a resolve) */
    uint64_t blit_ms_refused;          /* blit commands naming a multisampled texture refused */
    uint64_t capture_ms_skipped;       /* capture snapshots of a multisampled attachment skipped */
    /* translator (AIR) */
    uint64_t air_ms_ops;               /* AIR texture-multisample reads / sample-count queries seen in a translated module (NOT translated) */
    uint64_t air_sample_builtins;      /* AIR [[sample_id]] / [[sample_mask]] / sample position builtins seen in a translated module (NOT translated) */
    /* host-side machinery behind explicit UNWIRED entry points (src/vulkan/msaa_host.h) */
    uint64_t ms_fetch_views;           /* multisampled sampled-image views created for texel fetch (unwired entry point) */
    uint64_t ms_fetch_refused;         /* ... refused: 1x image, no SAMPLED usage, or vkCreateImageView failed */
    uint64_t a2c_pipes;                /* pipelines built with alphaToCoverageEnable (injected / future decoded flag) */
    uint64_t sample_shading_pipes;     /* pipelines built with sampleShadingEnable (injected / future decoded flag) */
    uint64_t fbfetch_ms_no_local_read_ms;   /* multisampled fetch refused: dynamicRenderingLocalReadMultisampledAttachments is false / unknown on the device */
    uint64_t sample_shading_refused;   /* sample shading asked for on a device without the sampleRateShading feature: pipeline refused */
    uint64_t ds_resolve_refused;       /* depth / stencil resolve modes refused by lagfx_msaa_dsres_choose (mode not offered, independent modes, unknown filter) */
    /* device info */
    uint64_t devinfo_clamped;          /* MSAASamples lowered to the largest count exact mode grants for the common colour + depth formats */
    uint64_t devinfo_gap;              /* device-info replies whose advertised range has a count the host lacks (e.g. 2x on 1|4): the max-count field cannot express it, exact mode refuses it */
} lagfx_msaa_stats_t;

static inline uint64_t lagfx_msaa_stats_any(const lagfx_msaa_stats_t *s) {
    const uint64_t *u = (const uint64_t *)s;
    uint64_t a = 0;
    for (size_t i = 0; i < sizeof *s / sizeof *u; i++) a |= u[i];
    return a;
}

/* Compat mode: exactly the string "1" in LAGFX_MSAA_COMPAT (read at every call; default exact). */
static inline int lagfx_msaa_compat_env(void) {
    const char *e = getenv("LAGFX_MSAA_COMPAT");
    return e && e[0] == '1' && e[1] == 0;
}

/* The one-line summary (<= 1000 chars). Returns the length written. The unsupported[...] group is everything that is NOT implemented: depth / stencil resolve, shader reads of a
 * multisampled texture (texture2d_ms), [[sample_id]] / [[sample_mask]] builtins, framebuffer fetch / tile shaders / blits / capture of multisampled attachments. */
static inline int lagfx_msaa_stats_line(const lagfx_msaa_stats_t *s, char *buf, size_t cap) {
#define LU(f) (unsigned long long)s->f
    return snprintf(buf, cap,
        "g7-gpu-msaa: mode=%s images[ms_created=%llu exact_refused=%llu compat_clamped=%llu compat_1x=%llu refused=%llu bad_shape=%llu] passes[ms=%llu attach=%llu resolves=%llu by_copy=%llu store_unknown=%llu "
        "resolve_no_target=%llu resolve_on_1x=%llu deferred_tags=%llu mixed_ignored=%llu ds_mismatch=%llu pass_refused=%llu] draws[pipes_ms=%llu draws_ms=%llu] "
        "unsupported[ds_resolve=%llu sampled_ms=%llu fbfetch_ms=%llu tile_ms=%llu blit_ms=%llu capture_ms=%llu air_ms_ops=%llu air_sample_id_mask=%llu] devinfo_clamped=%llu devinfo_gap=%llu host[a2c=%llu sample_shading=%llu sample_shading_refused=%llu msfetch_views=%llu msfetch_refused=%llu dsres_refused=%llu] fbfetch_ms[variants=%llu reads=%llu no_local_read_ms=%llu]",
        lagfx_msaa_compat_env() ? "compat" : "exact",
        LU(ms_images_created), LU(ms_exact_refused), LU(ms_compat_clamped), LU(ms_compat_1x), LU(ms_tex_refused), LU(ms_tex_bad_shape), LU(ms_passes), LU(ms_attach), LU(resolves), LU(resolves_by_copy),
        LU(store_unknown), LU(resolve_no_target), LU(resolve_on_1x), LU(deferred_store_tags), LU(attach_mixed_ignored), LU(ds_mismatch_ignored), LU(ms_pass_refused), LU(pipes_ms), LU(draws_ms),
        LU(ds_resolve_unsupported), LU(sampled_ms_refused), LU(fbfetch_ms_refused), LU(tile_ms_refused), LU(blit_ms_refused), LU(capture_ms_skipped), LU(air_ms_ops), LU(air_sample_builtins), LU(devinfo_clamped), LU(devinfo_gap), LU(a2c_pipes), LU(sample_shading_pipes), LU(sample_shading_refused), LU(ms_fetch_views), LU(ms_fetch_refused), LU(ds_resolve_refused), LU(fbfetch_ms_variants), LU(fbfetch_ms_reads), LU(fbfetch_ms_no_local_read_ms));
#undef LU
}

#endif
