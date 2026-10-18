#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host.h"
#include "addons.h"

#define STB_IMAGE_IMPLEMENTATION
#include "third_party/stb_image.h"

#define CBMP_PAGE_SIZE 131072
#define MAX_CBMP_PIECES 1024

typedef struct Replacement {
    int x, y, w, h;
    u32 hash;
    char png_path[1024];
    int decoded;
    u16 *pixels_555;
    u8 *alpha_mask;
} Replacement;

typedef struct ChunkPiece {
    u32 file_offset;
    u32 len;
} ChunkPiece;

static Replacement *replacements = NULL;
static int num_replacements = 0;
static int cap_replacements = 0;
static int addons_scanned = 0;

int addons_enabled(void)
{
    const char *env = getenv("FCOP_ADDONS");
    if (env && env[0] != '\0') {
        return atoi(env) != 0;
    }
    u32 val = 1;
    if (reg_host_get_dword("HKEY_LOCAL_MACHINE\\Software\\Electronic Arts\\Future Cop\\Settings", "UseAddons", &val)) {
        return val != 0;
    }
    return 1;
}

void addons_set_enabled(int on)
{
    reg_host_set_dword("HKEY_LOCAL_MACHINE\\Software\\Electronic Arts\\Future Cop\\Settings", "UseAddons", on ? 1 : 0);
}

static int get_addons_dir(char *buf, size_t sz)
{
    const char *env = getenv("FCOP_ADDONS_DIR");
    struct stat st;
    if (env && env[0] && stat(env, &st) == 0 && S_ISDIR(st.st_mode)) {
        strncpy(buf, env, sz - 1);
        buf[sz - 1] = '\0';
        return 1;
    }
    if (stat("addons", &st) == 0 && S_ISDIR(st.st_mode)) {
        strncpy(buf, "addons", sz - 1);
        buf[sz - 1] = '\0';
        return 1;
    }
    snprintf(buf, sz, "%s/../../addons", game_dir);
    if (stat(buf, &st) == 0 && S_ISDIR(st.st_mode)) {
        return 1;
    }
    return 0;
}

static int parse_texture_filename(const char *name, int *out_x, int *out_y, int *out_w, int *out_h, u32 *out_hash)
{
    int x, y, w, h;
    char hstr[32] = {0};
    char ext[16] = {0};
    if (sscanf(name, "%d_%d_%d_%d_%31[^.].%15s", &x, &y, &w, &h, hstr, ext) != 6)
        return 0;
    if (strcasecmp(ext, "png") != 0)
        return 0;
    if (strlen(hstr) != 8)
        return 0;
    for (int i = 0; i < 8; i++) {
        if (!isxdigit((unsigned char)hstr[i])) return 0;
    }
    if (x < 0 || y < 0 || w <= 0 || h <= 0) return 0;
    if (x + w > 256 || y + h > 256) return 0;

    *out_x = x;
    *out_y = y;
    *out_w = w;
    *out_h = h;
    *out_hash = (u32)strtoul(hstr, NULL, 16);
    return 1;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

static void add_replacement(int x, int y, int w, int h, u32 hash, const char *path)
{
    for (int i = 0; i < num_replacements; i++) {
        if (replacements[i].x == x && replacements[i].y == y &&
            replacements[i].w == w && replacements[i].h == h &&
            replacements[i].hash == hash) {
            strncpy(replacements[i].png_path, path, sizeof(replacements[i].png_path) - 1);
            if (replacements[i].decoded) {
                free(replacements[i].pixels_555);
                free(replacements[i].alpha_mask);
                replacements[i].pixels_555 = NULL;
                replacements[i].alpha_mask = NULL;
                replacements[i].decoded = 0;
            }
            return;
        }
    }

    if (num_replacements >= cap_replacements) {
        cap_replacements = cap_replacements ? cap_replacements * 2 : 16;
        replacements = (Replacement *)realloc(replacements, cap_replacements * sizeof(Replacement));
    }
    Replacement *r = &replacements[num_replacements++];
    memset(r, 0, sizeof(*r));
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
    r->hash = hash;
    strncpy(r->png_path, path, sizeof(r->png_path) - 1);
}

static int decode_replacement(Replacement *rep)
{
    if (rep->decoded) return 1;

    int src_w = 0, src_h = 0, channels = 0;
    u8 *img = stbi_load(rep->png_path, &src_w, &src_h, &channels, 4);
    if (!img) {
        plat_log("addons: failed to load %s: %s", rep->png_path, stbi_failure_reason());
        return 0;
    }

    int w = rep->w;
    int h = rep->h;
    rep->pixels_555 = (u16 *)malloc(w * h * sizeof(u16));
    rep->alpha_mask = (u8 *)malloc(w * h * sizeof(u8));

    if (src_w == w && src_h == h) {
        for (int i = 0; i < w * h; i++) {
            u8 a = img[i * 4 + 3];
            if (a >= 128) {
                rep->alpha_mask[i] = 1;
                u8 r = img[i * 4 + 0];
                u8 g = img[i * 4 + 1];
                u8 b = img[i * 4 + 2];
                rep->pixels_555[i] = (u16)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
            } else {
                rep->alpha_mask[i] = 0;
                rep->pixels_555[i] = 0;
            }
        }
    } else if (src_w >= w && src_h >= h) {
        // Downscale with box filter: average source pixels covering each target pixel
        for (int ty = 0; ty < h; ty++) {
            int sy_start = (ty * src_h) / h;
            int sy_end = ((ty + 1) * src_h) / h;
            if (sy_end <= sy_start) sy_end = sy_start + 1;

            for (int tx = 0; tx < w; tx++) {
                int sx_start = (tx * src_w) / w;
                int sx_end = ((tx + 1) * src_w) / w;
                if (sx_end <= sx_start) sx_end = sx_start + 1;

                u32 sum_r = 0, sum_g = 0, sum_b = 0, sum_a = 0;
                int count = 0;
                for (int sy = sy_start; sy < sy_end; sy++) {
                    for (int sx = sx_start; sx < sx_end; sx++) {
                        const u8 *p = &img[(sy * src_w + sx) * 4];
                        sum_r += p[0];
                        sum_g += p[1];
                        sum_b += p[2];
                        sum_a += p[3];
                        count++;
                    }
                }
                int idx = ty * w + tx;
                if (count > 0 && (sum_a / count) >= 128) {
                    rep->alpha_mask[idx] = 1;
                    u8 r = (u8)(sum_r / count);
                    u8 g = (u8)(sum_g / count);
                    u8 b = (u8)(sum_b / count);
                    rep->pixels_555[idx] = (u16)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                } else {
                    rep->alpha_mask[idx] = 0;
                    rep->pixels_555[idx] = 0;
                }
            }
        }
    } else {
        // Scale up with nearest
        for (int ty = 0; ty < h; ty++) {
            int sy = (ty * src_h) / h;
            if (sy >= src_h) sy = src_h - 1;
            for (int tx = 0; tx < w; tx++) {
                int sx = (tx * src_w) / w;
                if (sx >= src_w) sx = src_w - 1;
                const u8 *p = &img[(sy * src_w + sx) * 4];
                int idx = ty * w + tx;
                if (p[3] >= 128) {
                    rep->alpha_mask[idx] = 1;
                    u8 r = p[0], g = p[1], b = p[2];
                    rep->pixels_555[idx] = (u16)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
                } else {
                    rep->alpha_mask[idx] = 0;
                    rep->pixels_555[idx] = 0;
                }
            }
        }
    }

    stbi_image_free(img);
    rep->decoded = 1;
    return 1;
}

static void scan_addons_once(void)
{
    if (addons_scanned) return;
    addons_scanned = 1;

    char root[1024];
    if (!get_addons_dir(root, sizeof(root))) {
        return;
    }

    DIR *d = opendir(root);
    if (!d) return;

    char **subdirs = NULL;
    int num_subdirs = 0, cap_subdirs = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char subpath[1024];
        snprintf(subpath, sizeof(subpath), "%s/%s", root, de->d_name);
        struct stat st;
        if (stat(subpath, &st) == 0 && S_ISDIR(st.st_mode)) {
            if (num_subdirs >= cap_subdirs) {
                cap_subdirs = cap_subdirs ? cap_subdirs * 2 : 16;
                subdirs = (char **)realloc(subdirs, cap_subdirs * sizeof(char *));
            }
            subdirs[num_subdirs++] = strdup(de->d_name);
        }
    }
    closedir(d);

    if (num_subdirs > 1) {
        qsort(subdirs, num_subdirs, sizeof(char *), cmp_str);
    }

    for (int s = 0; s < num_subdirs; s++) {
        char tex_path[1024];
        snprintf(tex_path, sizeof(tex_path), "%s/%s/textures", root, subdirs[s]);
        DIR *td = opendir(tex_path);
        int count = 0;
        if (td) {
            char **pngs = NULL;
            int num_png = 0, cap_png = 0;
            while ((de = readdir(td)) != NULL) {
                if (de->d_name[0] == '.') continue;
                int x, y, w, h;
                u32 hash;
                if (parse_texture_filename(de->d_name, &x, &y, &w, &h, &hash)) {
                    if (num_png >= cap_png) {
                        cap_png = cap_png ? cap_png * 2 : 16;
                        pngs = (char **)realloc(pngs, cap_png * sizeof(char *));
                    }
                    pngs[num_png++] = strdup(de->d_name);
                }
            }
            closedir(td);

            if (num_png > 1) {
                qsort(pngs, num_png, sizeof(char *), cmp_str);
            }

            for (int p = 0; p < num_png; p++) {
                int x, y, w, h;
                u32 hash;
                if (parse_texture_filename(pngs[p], &x, &y, &w, &h, &hash)) {
                    char full_png[1024];
                    snprintf(full_png, sizeof(full_png), "%s/%s", tex_path, pngs[p]);
                    add_replacement(x, y, w, h, hash, full_png);
                    count++;
                }
                free(pngs[p]);
            }
            free(pngs);
        }
        plat_log("addons: %s: %d textures", subdirs[s], count);
        free(subdirs[s]);
    }
    free(subdirs);
}

static u32 calc_fnv1a(const u8 *data, size_t len)
{
    u32 h = 0x811c9dc5u;
    for (size_t i = 0; i < len; i++) {
        h = ((h ^ data[i]) * 0x01000193u);
    }
    return h;
}

static int process_cbmp(u8 *file_buf, u32 file_size, u32 rid, u32 rsize, const ChunkPiece *pieces, int num_pieces)
{
    u32 total_len = 0;
    for (int i = 0; i < num_pieces; i++) total_len += pieces[i].len;
    if (total_len < CBMP_PAGE_SIZE) return 0;

    u8 *payload = (u8 *)malloc(total_len);
    if (!payload) return 0;

    u32 cur = 0;
    for (int i = 0; i < num_pieces; i++) {
        memcpy(payload + cur, file_buf + pieces[i].file_offset, pieces[i].len);
        cur += pieces[i].len;
    }

    u32 page_start_in_payload = total_len - CBMP_PAGE_SIZE;
    u8 *page_px = payload + page_start_in_payload;

    int page_modified = 0;
    int patches_in_page = 0;

    for (int rep_idx = 0; rep_idx < num_replacements; rep_idx++) {
        Replacement *rep = &replacements[rep_idx];
        int x = rep->x, y = rep->y, w = rep->w, h = rep->h;

        u8 *region = (u8 *)malloc(w * h * 2);
        if (!region) continue;
        for (int r = 0; r < h; r++) {
            memcpy(region + r * (w * 2), page_px + (y + r) * 512 + x * 2, w * 2);
        }
        u32 hash = calc_fnv1a(region, w * h * 2);
        free(region);

        if (hash == rep->hash) {
            if (!rep->decoded) {
                if (!decode_replacement(rep)) continue;
            }
            for (int ry = 0; ry < h; ry++) {
                for (int rx = 0; rx < w; rx++) {
                    int pidx = ry * w + rx;
                    if (rep->alpha_mask[pidx]) {
                        u16 c555 = rep->pixels_555[pidx];
                        u32 px_off = (y + ry) * 512 + (x + rx) * 2;
                        page_px[px_off + 0] = (u8)(c555 & 0xFF);
                        page_px[px_off + 1] = (u8)(c555 >> 8);
                    }
                }
            }
            page_modified = 1;
            patches_in_page++;
        }
    }

    if (page_modified) {
        u32 payload_pos = 0;
        for (int i = 0; i < num_pieces; i++) {
            u32 piece_start = payload_pos;
            u32 piece_end = payload_pos + pieces[i].len;
            u32 ov_start = piece_start > page_start_in_payload ? piece_start : page_start_in_payload;
            u32 ov_end = piece_end < total_len ? piece_end : total_len;
            if (ov_end > ov_start) {
                u32 copy_len = ov_end - ov_start;
                u32 file_off = pieces[i].file_offset + (ov_start - piece_start);
                u32 payload_off = ov_start;
                memcpy(file_buf + file_off, payload + payload_off, copy_len);
            }
            payload_pos += pieces[i].len;
        }
    }

    free(payload);
    return patches_in_page;
}

u8 *addons_patch_file(const char *host_path, u32 *out_len)
{
    if (out_len) *out_len = 0;

    FILE *f = fopen(host_path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    if (fsz < 8) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);

    char magic[4];
    if (fread(magic, 1, 4, f) != 4) {
        fclose(f);
        return NULL;
    }
    if (memcmp(magic, "LRTC", 4) != 0) {
        fclose(f);
        return NULL;
    }

    // First mission file opened: scan addons once
    scan_addons_once();

    if (!addons_enabled() || num_replacements == 0) {
        fclose(f);
        return NULL;
    }

    u32 file_size = (u32)fsz;
    u8 *file_buf = (u8 *)malloc(file_size);
    if (!file_buf) {
        fclose(f);
        return NULL;
    }

    fseek(f, 0, SEEK_SET);
    if (fread(file_buf, 1, file_size, f) != file_size) {
        free(file_buf);
        fclose(f);
        return NULL;
    }
    fclose(f);

    ChunkPiece pieces[MAX_CBMP_PIECES];
    int num_pieces = 0;
    u32 cur_rsize = 0, cur_rid = 0, cur_collected = 0;
    int in_cbmp = 0;
    int total_regions_patched = 0;

    u32 o = 0;
    while (o + 8 <= file_size) {
        u32 size = *(u32 *)&file_buf[o + 4];
        if (memcmp(&file_buf[o], "LLIF", 4) == 0 || size < 8) {
            u32 next_shoc = (u32)-1;
            for (u32 k = o + 4; k + 4 <= file_size; k++) {
                if (memcmp(&file_buf[k], "COHS", 4) == 0) {
                    next_shoc = k;
                    break;
                }
            }
            if (next_shoc == (u32)-1) break;
            o = next_shoc;
            continue;
        }

        if (memcmp(&file_buf[o], "COHS", 4) == 0 && o + 20 <= file_size) {
            if (memcmp(&file_buf[o + 16], "RDHS", 4) == 0) {
                if (in_cbmp) {
                    total_regions_patched += process_cbmp(file_buf, file_size, cur_rid, cur_rsize, pieces, num_pieces);
                    in_cbmp = 0;
                }
                if (o + 36 <= file_size && memcmp(&file_buf[o + 24], "pmbC", 4) == 0) {
                    in_cbmp = 1;
                    cur_rid = *(u32 *)&file_buf[o + 28];
                    cur_rsize = *(u32 *)&file_buf[o + 32];
                    cur_collected = 0;
                    num_pieces = 0;
                }
            } else if (memcmp(&file_buf[o + 16], "TADS", 4) == 0 && in_cbmp) {
                u32 piece_len = (size > 20) ? (size - 20) : 0;
                if (o + 20 + piece_len > file_size) {
                    piece_len = file_size - (o + 20);
                }
                if (cur_collected + piece_len > cur_rsize) {
                    piece_len = cur_rsize - cur_collected;
                }
                if (piece_len > 0 && num_pieces < MAX_CBMP_PIECES) {
                    pieces[num_pieces].file_offset = o + 20;
                    pieces[num_pieces].len = piece_len;
                    num_pieces++;
                    cur_collected += piece_len;
                }
            }
        }

        o += size;
    }

    if (in_cbmp) {
        total_regions_patched += process_cbmp(file_buf, file_size, cur_rid, cur_rsize, pieces, num_pieces);
        in_cbmp = 0;
    }

    if (total_regions_patched > 0) {
        const char *slash = strrchr(host_path, '/');
        const char *fname = slash ? slash + 1 : host_path;
        plat_log("addons: %s: %d regions patched", fname, total_regions_patched);
        if (out_len) *out_len = file_size;
        return file_buf;
    }

    free(file_buf);
    return NULL;
}
