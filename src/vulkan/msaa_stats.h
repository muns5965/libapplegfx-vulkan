/*
 * libapplegfx-vulkan — multisample (MSAA) counters and the g7-gpu-msaa diagnostics line. No Vulkan types: included by protocol/state.h.
 * src/vulkan/msaa_stats.h
 *
 * SPDX-License-Identifier: MIT
 *
 * Every refusal / downgrade of the multisample path is counted here and printed: nothing is dropped, clamped or approximated silently.
 * Evidence level of the guest fields behind these counters is in docs/metal-vulkan-compatibility.md section 5 ("MSAA"): no guest capture of a multisampled pass exists yet.
 */
#ifndef LAGFX_VULKAN_MSAA_STATS_H
#define LAGFX_VULKAN_MSAA_STATS_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct lagfx_msaa_stats {
    /* textures (guest descriptor sample count > 1) */
    uint64_t ms_images_created;        /* device images made with more than one sample */
    uint64_t ms_images_clamped;        /* ... of those, with fewer samples than the guest asked for (device or format limit), still > 1 */
    uint64_t ms_fallback_1x;           /* guest asked for > 1 sample, the image is single sample (device / format cannot do any count > 1 for this use): explicit downgrade */
    uint64_t ms_tex_refused;           /* multisampled texture descriptors refused (no usable format, bad shape, creation failed): the texture does not exist for the guest */
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
    uint64_t ds_resolve_unsupported;   /* a depth / stencil attachment with a resolve texture or a resolve store action: stored only, resolve not recorded */
    /* draws */
    uint64_t pipes_ms;                 /* pipelines built with rasterizationSamples > 1 (count taken from the pass: the pipeline descriptor field is not decoded) */
    uint64_t draws_ms;                 /* draws recorded into a multisampled pass */
    uint64_t sampled_ms_refused;       /* shader texture bindings of a multisampled texture refused (no AIR texture2d_ms read in the translator): draw skipped */
    uint64_t fbfetch_ms_refused;       /* framebuffer-fetch draws in a multisampled pass refused (input attachment of a multisampled image needs a multisampled SubpassData type) */
    uint64_t tile_ms_refused;          /* tile-shader records in a multisampled pass refused (the pass would be suspended / resumed around a resolve) */
    uint64_t blit_ms_refused;          /* blit commands naming a multisampled texture refused */
    uint64_t capture_ms_skipped;       /* capture snapshots of a multisampled attachment skipped */
    /* translator (AIR) */
    uint64_t air_ms_ops;               /* AIR texture-multisample reads / sample-count queries seen in a translated module (NOT translated) */
    uint64_t air_sample_builtins;      /* AIR [[sample_id]] / [[sample_mask]] / sample position builtins seen in a translated module (NOT translated) */
    /* device info */
    uint64_t devinfo_clamped;          /* MSAASamples lowered to the host maximum */
} lagfx_msaa_stats_t;

static inline uint64_t lagfx_msaa_stats_any(const lagfx_msaa_stats_t *s) {
    const uint64_t *u = (const uint64_t *)s;
    uint64_t a = 0;
    for (size_t i = 0; i < sizeof *s / sizeof *u; i++) a |= u[i];
    return a;
}

/* The one-line summary (<= 1000 chars). Returns the length written. */
static inline int lagfx_msaa_stats_line(const lagfx_msaa_stats_t *s, char *buf, size_t cap) {
#define LU(f) (unsigned long long)s->f
    return snprintf(buf, cap,
        "g7-gpu-msaa: images[ms_created=%llu clamped=%llu fallback_1x=%llu refused=%llu bad_shape=%llu] passes[ms=%llu attach=%llu resolves=%llu by_copy=%llu store_unknown=%llu resolve_no_target=%llu resolve_on_1x=%llu "
        "deferred_tags=%llu mixed_ignored=%llu ds_mismatch=%llu ds_resolve_unsupported=%llu] draws[pipes_ms=%llu draws_ms=%llu] unsupported[sampled_ms=%llu fbfetch_ms=%llu tile_ms=%llu blit_ms=%llu capture_ms=%llu "
        "air_ms_ops=%llu air_sample_builtins=%llu] devinfo_clamped=%llu",
        LU(ms_images_created), LU(ms_images_clamped), LU(ms_fallback_1x), LU(ms_tex_refused), LU(ms_tex_bad_shape), LU(ms_passes), LU(ms_attach), LU(resolves), LU(resolves_by_copy), LU(store_unknown),
        LU(resolve_no_target), LU(resolve_on_1x), LU(deferred_store_tags), LU(attach_mixed_ignored), LU(ds_mismatch_ignored), LU(ds_resolve_unsupported), LU(pipes_ms), LU(draws_ms), LU(sampled_ms_refused),
        LU(fbfetch_ms_refused), LU(tile_ms_refused), LU(blit_ms_refused), LU(capture_ms_skipped), LU(air_ms_ops), LU(air_sample_builtins), LU(devinfo_clamped));
#undef LU
}

#endif
