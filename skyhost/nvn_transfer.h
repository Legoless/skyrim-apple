#pragma once

#include <stdint.h>

typedef struct sky_mtl_texture sky_mtl_texture_t;

enum {
    SKY_MTL_TRANSFER_BUFFER_TO_TEXTURE,
    SKY_MTL_TRANSFER_TEXTURE_TO_BUFFER,
    SKY_MTL_TRANSFER_TEXTURE_TO_TEXTURE,
};

typedef struct {
    uint32_t kind, flags;
    sky_mtl_texture_t *source, *destination;
    int32_t source_format, destination_format; /* NVN format, for compressed block geometry. */
    uint32_t source_level, source_layer, destination_level, destination_layer;
    /* x, y, z, width, height, depth. For non-3D textures z selects array slices;
     * source_layer/destination_layer are added to z. Levels/layers are relative
     * to the supplied Metal texture view. Depth 0 has NVN's one-slice meaning. */
    int32_t source_region[6], destination_region[6];
    uint64_t buffer_address;
    int64_t row_stride, image_stride; /* 0 = tightly packed; buffer copies only. */
} sky_mtl_transfer_t;

/* Encode into the caller's current command buffer, in command order. Returns
 * zero for invalid bounds, unresolved GPU addresses or unsupported copy forms.
 * The borrowed native Metal command buffer is not committed or waited here. */
int sky_mtl_transfer_encode(void *metal_command_buffer, const sky_mtl_transfer_t *transfer);

/* R8 video planes alias NVN CPU pool bytes directly where supported. Simulator
 * uses a private mirror; its backing must be synchronized before GPU reads and
 * flushed after GPU writes using the functions below. The caller supplies the
 * real NVN row pitch. Invalid formats, dimensions and ranges return NULL. */
sky_mtl_texture_t *sky_mtl_texture_create_linear_from_buffer(uint64_t gpu_address,
    uint32_t pixel_format, uint64_t width, uint64_t height, uint64_t row_stride);

/* Explicit mirror constructor also permits native GPU validation on hardware
 * that supports direct shared-buffer aliases. Source storage is retained by
 * the texture and inherited by its native views. */
sky_mtl_texture_t *sky_mtl_texture_create_linear_copy_from_buffer(uint64_t gpu_address,
    uint32_t pixel_format, uint64_t width, uint64_t height, uint64_t row_stride);

/* Native MTLTexture handles, including views. Nonmirrored textures are no-ops.
 * Encode/end a compute copy in the supplied command buffer; the caller must
 * end any active encoder first. These functions do not commit or wait.
 * Sync reads current CPU backing, flush writes visible texels back while
 * preserving row padding. NULL command/texture handles return zero. */
int sky_mtl_linear_texture_has_mirrors(void);
int sky_mtl_linear_texture_needs_sync(void *native_texture);
int sky_mtl_linear_texture_encode_sync(void *metal_command_buffer, void *native_texture);
int sky_mtl_linear_texture_encode_flush(void *metal_command_buffer, void *native_texture);
