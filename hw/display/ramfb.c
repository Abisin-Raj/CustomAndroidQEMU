/*
 * early boot framebuffer in guest ram
 * configured using fw_cfg
 *
 * Copyright Red Hat, Inc. 2017
 *
 * Author:
 *     Gerd Hoffmann <kraxel@redhat.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/loader.h"
#include "hw/display/ramfb.h"
#include "hw/display/bochs-vbe.h" /* for limits */
#include "ui/console.h"
#include "sysemu/reset.h"

struct QEMU_PACKED RAMFBCfg {
    uint64_t addr;
    uint32_t fourcc;
    uint32_t flags;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
};

typedef struct RAMFBCfg RAMFBCfg;

struct RAMFBState {
    DisplaySurface *ds;
    uint32_t width, height;
    struct RAMFBCfg cfg;
    bool diag_initialized;
    bool export_initialized;
    uint32_t *export_buf;
    size_t export_buf_size;
    uint64_t export_count;
};

static uint32_t ramfb_calc_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xffffffff;
    size_t i;
    for (i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xedb88320U & (uint32_t)(-(int32_t)(crc & 1)));
        }
    }
    return ~crc;
}

static void ramfb_generate_diag_pattern(uint32_t *buf, uint32_t width, uint32_t height, uint32_t stride)
{
    static const uint32_t cols1[8] = {
        0x00FFFFFF, 0x00FFFF00, 0x0000FFFF, 0x0000FF00,
        0x00FF00FF, 0x00FF0000, 0x000000FF, 0x00000000
    };
    static const uint32_t cols2[8] = {
        0x000000FF, 0x00000000, 0x00FF00FF, 0x00000000,
        0x0000FFFF, 0x00000000, 0x00FFFFFF, 0x00000000
    };
    uint32_t stride_words = stride / 4;
    uint32_t y, x;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            uint32_t b_idx = (x * 8) / width;
            uint32_t color;
            if (y < (height * 3) / 4) {
                color = cols1[b_idx];
            } else if (y < (height * 7) / 8) {
                color = cols2[b_idx];
            } else {
                uint32_t g = (b_idx * 255) / 7;
                color = (g << 16) | (g << 8) | g;
            }
            buf[y * stride_words + x] = color;
        }
    }
}

static void ramfb_unmap_display_surface(pixman_image_t *image, void *unused)
{
    void *data = pixman_image_get_data(image);
    uint32_t size = pixman_image_get_stride(image) *
        pixman_image_get_height(image);
    cpu_physical_memory_unmap(data, size, 0, 0);
}

static DisplaySurface *ramfb_create_display_surface(int width, int height,
                                                    pixman_format_code_t format,
                                                    hwaddr stride, hwaddr addr)
{
    DisplaySurface *surface;
    hwaddr size, mapsize, linesize;
    void *data;

    if (width < 16 || width > VBE_DISPI_MAX_XRES ||
        height < 16 || height > VBE_DISPI_MAX_YRES ||
        format == 0 /* unknown format */)
        return NULL;

    linesize = width * PIXMAN_FORMAT_BPP(format) / 8;
    if (stride == 0) {
        stride = linesize;
    }

    mapsize = size = stride * (height - 1) + linesize;
    data = cpu_physical_memory_map(addr, &mapsize, false);
    if (size != mapsize) {
        cpu_physical_memory_unmap(data, mapsize, 0, 0);
        return NULL;
    }

    surface = qemu_create_displaysurface_from(width, height,
                                              format, stride, data);
    pixman_image_set_destroy_function(surface->image,
                                      ramfb_unmap_display_surface, NULL);

    return surface;
}

static void ramfb_fw_cfg_write(void *dev, off_t offset, size_t len)
{
    RAMFBState *s = dev;
    DisplaySurface *surface;
    uint32_t fourcc, format, width, height;
    hwaddr stride, addr;

    width  = be32_to_cpu(s->cfg.width);
    height = be32_to_cpu(s->cfg.height);
    stride = be32_to_cpu(s->cfg.stride);
    fourcc = be32_to_cpu(s->cfg.fourcc);
    addr   = be64_to_cpu(s->cfg.addr);
    format = qemu_drm_format_to_pixman(fourcc);

    surface = ramfb_create_display_surface(width, height,
                                           format, stride, addr);
    if (!surface) {
        return;
    }

    s->width = width;
    s->height = height;
    qemu_free_displaysurface(s->ds);
    s->ds = surface;
}

void ramfb_diag_update(QemuConsole *con, RAMFBState *s, hwaddr addr,
                       uint32_t width, uint32_t height)
{
    if (!s->diag_initialized) {
        FILE *flog = fopen("C:\\qemu_work\\phase18_diag.log", "a");
        hwaddr stride = width * 4;
        hwaddr total_size = stride * height;
        hwaddr map_size = total_size;
        uint32_t *pattern;
        void *guest_ptr;
        void *verify_ptr;

        if (flog) {
            fprintf(flog, "=== Phase 18.2 Diagnostic Pattern Generator ===\n");
            fprintf(flog, "Target GPA: 0x%" PRIx64 ", Dimensions: %ux%u, Stride: %u, Total: %u bytes\n",
                    (uint64_t)addr, width, height, (uint32_t)stride, (uint32_t)total_size);
        }

        /* 1. Bounds check against reserved framebuffer (0x7f000000 - 0x7f800000) */
        if (addr < 0x7f000000ULL || (addr + total_size) > 0x7f800000ULL) {
            if (flog) {
                fprintf(flog, "ERROR: Target address 0x%" PRIx64 " out of reserved range [0x7f000000, 0x7f800000)!\n",
                        (uint64_t)addr);
                fclose(flog);
            }
            return;
        }

        pattern = g_malloc(total_size);
        ramfb_generate_diag_pattern(pattern, width, height, stride);

        /* 2. Map guest RAM with write permission */
        guest_ptr = cpu_physical_memory_map(addr, &map_size, true);
        if (!guest_ptr || map_size != total_size) {
            if (flog) {
                fprintf(flog, "ERROR: cpu_physical_memory_map failed or truncated: ptr=%p map_size=%" PRIu64 "\n",
                        guest_ptr, (uint64_t)map_size);
                fclose(flog);
            }
            if (guest_ptr) {
                cpu_physical_memory_unmap(guest_ptr, map_size, false, 0);
            }
            g_free(pattern);
            return;
        }

        /* 3. Write pattern into guest RAM */
        memcpy(guest_ptr, pattern, total_size);
        cpu_physical_memory_unmap(guest_ptr, map_size, true, total_size);

        if (flog) {
            fprintf(flog, "Host-to-Guest write successful: %u bytes written to GPA 0x%" PRIx64 "\n",
                    (uint32_t)total_size, (uint64_t)addr);
        }

        /* 4. Independently read back from guest RAM and verify */
        map_size = total_size;
        verify_ptr = cpu_physical_memory_map(addr, &map_size, false);
        if (verify_ptr && map_size == total_size) {
            int diff = memcmp(verify_ptr, pattern, total_size);
            uint32_t crc = ramfb_calc_crc32((const uint8_t *)verify_ptr, total_size);
            if (flog) {
                fprintf(flog, "Readback verification: %s (diff=%d)\n",
                        (diff == 0) ? "PASS (Exact Match)" : "FAIL (Mismatch)", diff);
                fprintf(flog, "Readback buffer CRC32: 0x%08x (Expected: 0xc95f0043)\n", crc);
                fprintf(flog, "First 8 sample pixels (row 0, bars 0..7):\n");
                for (int b = 0; b < 8; b++) {
                    uint32_t pval = ((uint32_t *)verify_ptr)[b * (width / 8)];
                    fprintf(flog, "  Bar %d (x=%d): 0x%08x\n", b, b * (width / 8), pval);
                }
            }
            cpu_physical_memory_unmap(verify_ptr, map_size, false, 0);
        } else {
            if (flog) {
                fprintf(flog, "ERROR: Readback mapping failed!\n");
            }
            if (verify_ptr) {
                cpu_physical_memory_unmap(verify_ptr, map_size, false, 0);
            }
        }

        g_free(pattern);

        /* 5. Create DisplaySurface directly from guest RAM */
        s->ds = ramfb_create_display_surface(width, height, PIXMAN_x8r8g8b8, stride, addr);
        if (!s->ds) {
            if (flog) {
                fprintf(flog, "ERROR: ramfb_create_display_surface failed!\n");
                fclose(flog);
            }
            return;
        }

        s->width = width;
        s->height = height;
        s->diag_initialized = true;

        if (flog) {
            fprintf(flog, "DisplaySurface created successfully: %ux%u format=PIXMAN_x8r8g8b8\n",
                    width, height);
            fclose(flog);
        }
    }

    if (s->ds) {
        dpy_gfx_replace_surface(con, s->ds);
        s->ds = NULL;
    }

    /* simple full screen update */
    dpy_gfx_update_full(con);
}

static void ramfb_read_guest_fb(uint8_t *dest, size_t total_size, hwaddr addr)
{
    if (addr == 0xe7e00000ULL) {
        /* Standard Android FB 30 scatterlist chunks */
        static const struct { hwaddr gpa; uint32_t len; } sg_chunks[] = {
            { 0xe7e00000ULL, 0x200000 },
            { 0xe7800000ULL, 0x200000 },
            { 0xe7a00000ULL, 0x200000 },
            { 0xe8200000ULL, 0x200000 },
            { 0xe8030000ULL, 0x010000 }, { 0xe8040000ULL, 0x010000 },
            { 0xe8050000ULL, 0x010000 }, { 0xe8060000ULL, 0x010000 },
            { 0xe8070000ULL, 0x010000 }, { 0xe8080000ULL, 0x010000 },
            { 0xe8090000ULL, 0x010000 }, { 0xe80a0000ULL, 0x010000 },
            { 0xe80b0000ULL, 0x010000 }, { 0xe80c0000ULL, 0x010000 },
            { 0xe80d0000ULL, 0x010000 }, { 0xe80e0000ULL, 0x010000 },
            { 0xe80f0000ULL, 0x010000 }, { 0xe8100000ULL, 0x010000 },
            { 0xe8110000ULL, 0x010000 }, { 0xe8120000ULL, 0x010000 },
            { 0xe8130000ULL, 0x010000 }, { 0xe8140000ULL, 0x010000 },
            { 0xe8150000ULL, 0x010000 }, { 0xe8160000ULL, 0x010000 },
            { 0xe8170000ULL, 0x010000 }, { 0xe8180000ULL, 0x010000 },
            { 0xe8190000ULL, 0x010000 }, { 0xe81a0000ULL, 0x010000 },
            { 0xe81b0000ULL, 0x010000 }, { 0xe81c0000ULL, 0x010000 },
            { 0xe81d0000ULL, 0x010000 }, { 0xe81e0000ULL, 0x010000 },
            { 0xe81f0000ULL, 0x010000 }, { 0xe8200000ULL, 0x010000 },
            { 0xe8210000ULL, 0x010000 }, { 0xe8220000ULL, 0x010000 },
            { 0xe8230000ULL, 0x010000 }, { 0xe8240000ULL, 0x010000 },
            { 0xeb870000ULL, 0x001000 }
        };
        size_t off = 0;
        for (size_t i = 0; i < ARRAY_SIZE(sg_chunks) && off < total_size; i++) {
            uint32_t chunk_len = sg_chunks[i].len;
            if (off + chunk_len > total_size) {
                chunk_len = total_size - off;
            }
            cpu_physical_memory_read(sg_chunks[i].gpa, dest + off, chunk_len);
            off += chunk_len;
        }
        if (off < total_size) {
            memset(dest + off, 0, total_size - off);
        }
    } else {
        cpu_physical_memory_read(addr, dest, total_size);
    }
}

static void ramfb_decode_ubwc_rgba(const uint8_t *src_buf, uint32_t *linear_out,
                                   uint32_t buf_width, uint32_t buf_height,
                                   uint32_t vis_width, uint32_t vis_height)
{
    const uint32_t tile_w = 16;
    const uint32_t tile_h = 4;
    const uint32_t tiles_x = buf_width / tile_w;
    const uint32_t tiles_y = buf_height / tile_h;
    const size_t meta_plane_size = 77824;
    const uint8_t *payload = src_buf + meta_plane_size;

    for (uint32_t ty = 0; ty < tiles_y; ty++) {
        for (uint32_t tx = 0; tx < tiles_x; tx++) {
            uint32_t tile_idx = ty * tiles_x + tx;
            const uint32_t *tile_pixels = (const uint32_t *)(payload + tile_idx * 256);

            for (uint32_t py = 0; py < tile_h; py++) {
                uint32_t y = ty * tile_h + py;
                if (y >= vis_height) {
                    continue;
                }
                for (uint32_t px = 0; px < tile_w; px++) {
                    uint32_t x = tx * tile_w + px;
                    if (x >= vis_width) {
                        continue;
                    }
                    uint32_t p = tile_pixels[py * tile_w + px];
                    uint8_t r = p & 0xff;
                    uint8_t g = (p >> 8) & 0xff;
                    uint8_t b = (p >> 16) & 0xff;
                    linear_out[y * vis_width + x] = (0xff000000U) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
                }
            }
        }
    }
}

static void ramfb_decode_linear_rgba(const uint8_t *src_buf, uint32_t *linear_out,
                                    uint32_t buf_width, uint32_t buf_height,
                                    uint32_t vis_width, uint32_t vis_height)
{
    for (uint32_t y = 0; y < vis_height && y < buf_height; y++) {
        const uint32_t *src_row = (const uint32_t *)(src_buf + y * buf_width * 4);
        uint32_t *dst_row = linear_out + y * vis_width;
        for (uint32_t x = 0; x < vis_width && x < buf_width; x++) {
            uint32_t p = src_row[x];
            uint8_t r = p & 0xff;
            uint8_t g = (p >> 8) & 0xff;
            uint8_t b = (p >> 16) & 0xff;
            dst_row[x] = (0xff000000U) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }
}

void ramfb_export_update(QemuConsole *con, RAMFBState *s,
                         hwaddr addr, uint32_t buf_width, uint32_t buf_height,
                         uint32_t vis_width, uint32_t vis_height, bool is_ubwc)
{
    size_t vis_size = (size_t)vis_width * vis_height * 4;
    size_t buf_bytes = (size_t)buf_width * buf_height * 4;
    size_t meta_size = is_ubwc ? 77824 : 0;
    size_t total_raw_size = buf_bytes + meta_size;

    if (!s->export_buf || s->export_buf_size != vis_size) {
        g_free(s->export_buf);
        s->export_buf = g_malloc0(vis_size);
        s->export_buf_size = vis_size;
    }

    uint8_t *raw_buf = g_malloc0(total_raw_size);
    ramfb_read_guest_fb(raw_buf, total_raw_size, addr);

    /* Inspect raw guest RAM */
    size_t nonzero_raw_words = 0;
    const uint32_t *raw32 = (const uint32_t *)raw_buf;
    size_t total_words = total_raw_size / 4;
    for (size_t wi = 0; wi < total_words; wi++) {
        if (raw32[wi] != 0) {
            nonzero_raw_words++;
        }
    }
    uint32_t raw_crc = ramfb_calc_crc32(raw_buf, total_raw_size);

    if (is_ubwc) {
        ramfb_decode_ubwc_rgba(raw_buf, s->export_buf, buf_width, buf_height, vis_width, vis_height);
    } else {
        ramfb_decode_linear_rgba(raw_buf, s->export_buf, buf_width, buf_height, vis_width, vis_height);
    }
    g_free(raw_buf);

    s->export_count++;

    if (!s->export_initialized || s->width != vis_width || s->height != vis_height) {
        DisplaySurface *ds = qemu_create_displaysurface_from(vis_width, vis_height,
                                                            PIXMAN_x8r8g8b8, vis_width * 4,
                                                            (void *)s->export_buf);
        s->width = vis_width;
        s->height = vis_height;
        s->export_initialized = true;
        dpy_gfx_replace_surface(con, ds);
    }

    FILE *flog = fopen("C:\\qemu_work\\phase18_export.log", "a");
    if (flog) {
        int64_t now_us = g_get_real_time();
        uint32_t dec_crc = ramfb_calc_crc32((const uint8_t *)s->export_buf, vis_size);
        fprintf(flog, "[%lld us] FB EXPORT #%" PRIu64 ": addr=0x%" PRIx64 " size=%zu vis=%ux%u ubwc=%d | raw_crc=0x%08x dec_crc=0x%08x | nonzero_words=%zu/%zu\n",
                (long long)now_us, s->export_count, (uint64_t)addr, total_raw_size,
                vis_width, vis_height, is_ubwc ? 1 : 0, raw_crc, dec_crc, nonzero_raw_words, total_words);
        fclose(flog);
    }

    dpy_gfx_update_full(con);
}

void ramfb_display_update(QemuConsole *con, RAMFBState *s)
{
    if (!s->width || !s->height) {
        return;
    }

    if (s->ds) {
        dpy_gfx_replace_surface(con, s->ds);
        s->ds = NULL;
    }

    /* simple full screen update */
    dpy_gfx_update_full(con);
}

static int ramfb_post_load(void *opaque, int version_id)
{
    ramfb_fw_cfg_write(opaque, 0, 0);
    return 0;
}

const VMStateDescription ramfb_vmstate = {
    .name = "ramfb",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ramfb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_UNSAFE(cfg, RAMFBState, 0, sizeof(RAMFBCfg)),
        VMSTATE_END_OF_LIST()
    }
};

RAMFBState *ramfb_setup(Error **errp)
{
    FWCfgState *fw_cfg = fw_cfg_find();
    RAMFBState *s;

    if (!fw_cfg || !fw_cfg->dma_enabled) {
        error_setg(errp, "ramfb device requires fw_cfg with DMA");
        return NULL;
    }

    s = g_new0(RAMFBState, 1);

    rom_add_vga("vgabios-ramfb.bin");
    fw_cfg_add_file_callback(fw_cfg, "etc/ramfb",
                             NULL, ramfb_fw_cfg_write, s,
                             &s->cfg, sizeof(s->cfg), false);
    return s;
}
