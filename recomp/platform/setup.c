/* Where the game's files are, and installing them on first start.
 *
 * Next to the executable:
 *   game_files/   the files from the player's CD (the folder holding FCopLAPD.exe is game_dir)
 *   user_info/    settings, key bindings and saved games (user_dir)
 *
 * When game_files/ has no FCopLAPD.exe, the game installs it from a CD image: one given on
 * the command line, one lying next to the executable, or one the player picks. Raw .bin/.cue
 * dumps work too. Without an image the player can copy the CD into game_files/ by hand.
 */
#define _GNU_SOURCE
#include <SDL.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "host.h"
extern char font8x8_basic[128][8];  /* dialog.c includes the font */

char user_dir[1024];
static char base_dir[1024];  /* the executable's folder, no trailing slash */

static int is_dir(const char *p) { struct stat st; return !stat(p, &st) && S_ISDIR(st.st_mode); }
static int is_file(const char *p) { struct stat st; return !stat(p, &st) && S_ISREG(st.st_mode); }

static int ends_with(const char *s, const char *ext)
{
    size_t a = strlen(s), b = strlen(ext);
    return a >= b && !strcasecmp(s + a - b, ext);
}

/* the folder under dir (up to 3 levels down) holding FCopLAPD.exe, in any letter case */
static int find_game(const char *dir, int depth, char *out, size_t n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    char sub[1024];
    int found = 0;
    if (!d) return 0;
    while (!found && (e = readdir(d)))
        if (!strcasecmp(e->d_name, "FCopLAPD.exe")) {
            snprintf(out, n, "%s", dir);
            found = 1;
        }
    rewinddir(d);
    while (!found && depth > 0 && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
        if (is_dir(sub)) found = find_game(sub, depth - 1, out, n);
    }
    closedir(d);
    return found;
}

/* ---- reading an ISO 9660 CD image (Joliet names when present) ----------------------------- */
typedef struct {
    FILE *f;
    long sector, offset;  /* bytes per sector in the file, and where the 2048 data bytes start */
    u32 root, root_len;   /* root directory extent */
    int joliet;
    char volume[33];
} Iso;

static int iso_read(Iso *iso, u32 lba, u8 *buf, u32 count)
{
    if (iso->sector == 2048) {
        if (fseeko(iso->f, (off_t)lba * 2048, SEEK_SET)) return -1;
        return fread(buf, 2048, count, iso->f) == count ? 0 : -1;
    }
    for (u32 k = 0; k < count; k++) {
        if (fseeko(iso->f, (off_t)(lba + k) * iso->sector + iso->offset, SEEK_SET)) return -1;
        if (fread(buf + 2048 * k, 2048, 1, iso->f) != 1) return -1;
    }
    return 0;
}

static u32 le32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }

/* a .cue sheet names the .bin that holds the data track */
static int cue_bin(const char *cue, char *out, size_t n)
{
    FILE *f = fopen(cue, "r");
    char line[1024], *a, *b, dir[1024], *slash;
    int ok = 0;
    if (!f) return 0;
    snprintf(dir, sizeof dir, "%s", cue);
    slash = strrchr(dir, '/');
    if (slash) *slash = 0; else strcpy(dir, ".");
    while (!ok && fgets(line, sizeof line, f))
        if ((a = strstr(line, "FILE \"")) && (b = strrchr(a + 6, '"'))) {
            *b = 0;
            snprintf(out, n, "%s/%s", dir, a + 6);
            ok = 1;
        }
    fclose(f);
    return ok;
}

static int iso_open(Iso *iso, const char *path)
{
    static const struct { long sector, offset; } layouts[] = {{2048, 0}, {2352, 16}, {2352, 24}, {2336, 8}};
    char bin[1024];
    u8 vd[2048];
    memset(iso, 0, sizeof *iso);
    if (ends_with(path, ".cue")) {
        if (!cue_bin(path, bin, sizeof bin)) return -1;
        path = bin;
    }
    if (!(iso->f = fopen(path, "rb"))) return -1;
    for (unsigned k = 0; k < sizeof layouts / sizeof layouts[0] && !iso->root; k++) {
        iso->sector = layouts[k].sector;
        iso->offset = layouts[k].offset;
        for (u32 lba = 16; lba < 32 && !iso_read(iso, lba, vd, 1); lba++) {
            if (memcmp(vd + 1, "CD001", 5) || vd[0] == 255) break;
            if (vd[0] == 1 && !iso->root) {         /* primary volume descriptor */
                iso->root = le32(vd + 156 + 2);
                iso->root_len = le32(vd + 156 + 10);
                memcpy(iso->volume, vd + 40, 32);
                for (int j = 31; j >= 0 && iso->volume[j] == ' '; j--) iso->volume[j] = 0;
            } else if (vd[0] == 2 && vd[88] == '%' && vd[89] == '/' && strchr("@CE", vd[90])) {
                iso->root = le32(vd + 156 + 2);     /* Joliet: same tree, UCS-2 names */
                iso->root_len = le32(vd + 156 + 10);
                iso->joliet = 1;
                break;
            }
        }
    }
    if (!iso->root) { fclose(iso->f); return -1; }
    return 0;
}

/* walk a directory: fn(name, lba, size, is_dir) for each entry; names are UTF-8, version cut off */
typedef int (*IsoFn)(void *ctx, const char *name, u32 lba, u32 size, int dir);

static int iso_list(Iso *iso, u32 lba, u32 len, IsoFn fn, void *ctx)
{
    u32 nsec = (len + 2047) / 2048;
    u8 *buf = malloc((size_t)nsec * 2048);
    int r = 0;
    if (!buf || iso_read(iso, lba, buf, nsec)) { free(buf); return -1; }
    for (u32 pos = 0; pos < len && !r;) {
        u8 *e = buf + pos, nlen;
        char name[256];
        size_t o = 0;
        if (!e[0]) { pos = (pos / 2048 + 1) * 2048; continue; }  /* records never cross sectors */
        nlen = e[32];
        if (33 + nlen > e[0] || pos + e[0] > len) break;
        if (!(nlen == 1 && e[33] <= 1)) {                        /* skip "." and ".." */
            if (iso->joliet) {
                for (int k = 0; k + 1 < nlen && o + 4 < sizeof name; k += 2) {
                    unsigned ch = e[33 + k] << 8 | e[34 + k];
                    if (ch < 0x80) name[o++] = (char)ch;
                    else if (ch < 0x800) { name[o++] = (char)(0xc0 | ch >> 6); name[o++] = (char)(0x80 | (ch & 63)); }
                    else { name[o++] = (char)(0xe0 | ch >> 12); name[o++] = (char)(0x80 | (ch >> 6 & 63)); name[o++] = (char)(0x80 | (ch & 63)); }
                }
            } else {
                for (int k = 0; k < nlen; k++) name[o++] = (char)e[33 + k];
            }
            name[o] = 0;
            if (strchr(name, ';')) *strchr(name, ';') = 0;
            if (o && name[strlen(name) - 1] == '.') name[strlen(name) - 1] = 0;
            /* names come from the image: never let one leave the folder it is written into */
            if (name[0] && !strchr(name, '/') && !strchr(name, '\\') && strcmp(name, ".") && strcmp(name, ".."))
                r = fn(ctx, name, le32(e + 2), le32(e + 10), e[25] & 2);
        }
        pos += e[0];
    }
    free(buf);
    return r;
}

/* ---- the setup screen ------------------------------------------------------------------- */
/* One window in the game's blue for everything the first start asks or shows, drawn with the
   dialog font at a fixed logical size and scaled to about half the screen. Without a screen
   (tests, SDL's offscreen driver) the texts go to the log instead. */
#define UI_W 1120
#define UI_H 630
enum { C_BG = 0x0e1638, C_BAND = 0x16224e, C_ACCENT = 0x5a8cff, C_TEXT = 0xe6e8f2, C_DIM = 0x9aa4c8,
       C_WARN = 0xffc040, C_BUTTON = 0x22346e, C_HOVER = 0x3558c0 };
static SDL_Window *uw;
static SDL_Renderer *ur;

typedef struct { const char *label; int id; SDL_Rect r; } Button;

static int ui_open(void)
{
    SDL_DisplayMode m;
    int w = UI_W, h = UI_H;
    const char *drv;
    if (ur) return 0;
    if (SDL_InitSubSystem(SDL_INIT_VIDEO)) return -1;
    drv = SDL_GetCurrentVideoDriver();
    if (drv && (!strcmp(drv, "offscreen") || !strcmp(drv, "dummy"))) return -1;
    if (!SDL_GetDesktopDisplayMode(0, &m)) {
        w = m.w * 11 / 20;
        h = w * 9 / 16;
        if (h > m.h * 3 / 4) { h = m.h * 3 / 4; w = h * 16 / 9; }
    }
    uw = SDL_CreateWindow("Future Cop: L.A.P.D. setup", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h,
                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (uw && !(ur = SDL_CreateRenderer(uw, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC)))
        ur = SDL_CreateRenderer(uw, -1, 0);
    if (!ur) {
        if (uw) SDL_DestroyWindow(uw);
        uw = NULL;
        return -1;
    }
    SDL_RenderSetLogicalSize(ur, UI_W, UI_H);  /* mouse positions arrive in these units too */
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    return 0;
}

static void ui_close(void)
{
    if (ur) SDL_DestroyRenderer(ur);
    if (uw) SDL_DestroyWindow(uw);
    ur = NULL;
    uw = NULL;
}

static void ui_color(u32 rgb) { SDL_SetRenderDrawColor(ur, rgb >> 16, rgb >> 8 & 255, rgb & 255, 255); }

static void ui_text(int x, int y, int scale, const char *t, u32 rgb)
{
    ui_color(rgb);
    for (; *t; t++, x += 8 * scale)
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                if ((unsigned char)*t < 128 && font8x8_basic[(unsigned char)*t][row] >> col & 1) {
                    SDL_Rect px = {x + col * scale, y + row * scale, scale, scale};
                    SDL_RenderFillRect(ur, &px);
                }
}

static void ui_text_mid(int y, int scale, const char *t, u32 rgb)
{
    ui_text((UI_W - (int)strlen(t) * 8 * scale) / 2, y, scale, t, rgb);
}

/* lines starting with \1 are drawn in the accent colour; total > 0 draws a progress bar */
static void ui_draw(const char *const *lines, const char *status, Button *b, int nb, int hover, u64 done, u64 total)
{
    SDL_Rect band = {0, 0, UI_W, 116}, rule = {0, 116, UI_W, 4};
    int y = 150;
    ui_color(C_BG);
    SDL_RenderClear(ur);
    ui_color(C_BAND);
    SDL_RenderFillRect(ur, &band);
    ui_color(C_ACCENT);
    SDL_RenderFillRect(ur, &rule);
    ui_text_mid(30, 5, "FUTURE COP: L.A.P.D.", C_TEXT);
    ui_text_mid(86, 2, "first start", C_DIM);
    for (; *lines; lines++, y += 28)
        ui_text(64, y, 2, lines[0][0] == '\1' ? *lines + 1 : *lines, lines[0][0] == '\1' ? C_ACCENT : C_TEXT);
    if (status && *status) ui_text(64, y + 12, 2, status, C_WARN);
    if (total) {
        SDL_Rect bar = {64, 420, UI_W - 128, 36}, fill = {68, 424, 0, 28};
        char line[64];
        ui_color(C_ACCENT);
        SDL_RenderDrawRect(ur, &bar);
        fill.w = (int)((bar.w - 8) * (double)done / (double)total);
        SDL_RenderFillRect(ur, &fill);
        snprintf(line, sizeof line, "%llu of %llu MB", (unsigned long long)(done >> 20), (unsigned long long)(total >> 20));
        ui_text_mid(472, 2, line, C_DIM);
    }
    for (int k = 0, x = (UI_W - nb * 300 - (nb - 1) * 32) / 2; k < nb; k++, x += 332) {
        b[k].r = (SDL_Rect){x, 534, 300, 60};
        ui_color(k == hover ? C_HOVER : C_BUTTON);
        SDL_RenderFillRect(ur, &b[k].r);
        ui_color(C_ACCENT);
        SDL_RenderDrawRect(ur, &b[k].r);
        ui_text(x + (300 - (int)strlen(b[k].label) * 16) / 2, 556, 2, b[k].label, C_TEXT);
    }
    SDL_RenderPresent(ur);
}

/* the id of the button an event presses, or -1; Return presses the last button, Esc the first */
static int ui_button(const SDL_Event *ev, Button *b, int nb, int *hover)
{
    SDL_Point p;
    if (ev->type == SDL_KEYDOWN && ev->key.keysym.sym == SDLK_ESCAPE) return b[0].id;
    if (ev->type == SDL_KEYDOWN && (ev->key.keysym.sym == SDLK_RETURN || ev->key.keysym.sym == SDLK_KP_ENTER))
        return b[nb - 1].id;
    if (ev->type == SDL_MOUSEMOTION || ev->type == SDL_MOUSEBUTTONUP) {
        p.x = ev->type == SDL_MOUSEMOTION ? ev->motion.x : ev->button.x;
        p.y = ev->type == SDL_MOUSEMOTION ? ev->motion.y : ev->button.y;
        *hover = -1;
        for (int k = 0; k < nb; k++)
            if (SDL_PointInRect(&p, &b[k].r)) *hover = k;
        if (ev->type == SDL_MOUSEBUTTONUP && ev->button.button == SDL_BUTTON_LEFT && *hover >= 0) return b[*hover].id;
    }
    return -1;
}

static int ui_closed(const SDL_Event *ev)
{
    return ev->type == SDL_QUIT || (ev->type == SDL_WINDOWEVENT && ev->window.event == SDL_WINDOWEVENT_CLOSE);
}

/* a message with an OK button, shown until the player closes it */
static void ui_message(const char *const *lines)
{
    Button b[1] = {{"OK", 0, {0, 0, 0, 0}}};
    int hover = -1;
    SDL_Event ev;
    for (const char *const *l = lines; *l; l++) plat_log("%s", **l == '\1' ? *l + 1 : *l);
    if (ui_open()) return;
    for (;;) {
        ui_draw(lines, NULL, b, 1, hover, 0, 0);
        if (!SDL_WaitEvent(&ev) || ui_closed(&ev) || ui_button(&ev, b, 1, &hover) == 0) return;
    }
}

/* the copy's progress; returns -1 when the player cancels */
static int progress(const char *what, u64 done, u64 total)
{
    static Uint32 last;
    static Button b[1] = {{"Cancel", 1, {0, 0, 0, 0}}};
    static int hover = -1;
    char name[64];
    const char *lines[] = {"Copying the game files from your CD image...", "", name, NULL};
    SDL_Event ev;
    if (!ur) return 0;
    while (SDL_PollEvent(&ev))
        if (ui_closed(&ev) || ui_button(&ev, b, 1, &hover) == 1) return -1;
    if (SDL_GetTicks() - last < 30 && done < total) return 0;
    last = SDL_GetTicks();
    snprintf(name, sizeof name, "\1%.60s", what);
    ui_draw(lines, NULL, b, 1, hover, done, total);
    return 0;
}

/* ---- extracting ------------------------------------------------------------------------- */
typedef struct {
    Iso *iso;
    char dir[1024];       /* host folder being written */
    int depth;
    int counting;         /* first pass: only add up the sizes */
    int game_only;        /* the known CD layout: only the folders the game reads */
    int exe_pending;      /* FCopLAPD.exe is written last, so a cut-short copy is retried */
    u32 exe_lba, exe_size;
    char exe_path[1024];
    u64 done, total;
} Extract;

static int copy_extent(Extract *x, const char *path, u32 lba, u32 size)
{
    u8 *buf = malloc(64 * 2048);
    FILE *out = fopen(path, "wb");
    int r = buf && out ? 0 : -1;
    while (!r && size) {
        u32 n = size < 64 * 2048 ? size : 64 * 2048, nsec = (n + 2047) / 2048;
        if (iso_read(x->iso, lba, buf, nsec) || fwrite(buf, 1, n, out) != n) r = -1;
        lba += nsec;
        size -= n;
        x->done += n;
        if (!r && progress(path + strlen(base_dir) + 1, x->done, x->total)) r = -2;
    }
    if (out && fclose(out)) r = -1;
    free(buf);
    return r;
}

static int extract_entry(void *ctx, const char *name, u32 lba, u32 size, int dir)
{
    Extract *x = ctx;
    char path[1024], saved[1024];
    int r;
    if (x->depth == 0 && x->game_only && strcasecmp(name, "FCopLAPD") && strcasecmp(name, "Video")) return 0;
    snprintf(path, sizeof path, "%s/%s", x->dir, name);
    if (dir) {
        if (!x->counting && mkdir(path, 0755) && errno != EEXIST) return -1;
        snprintf(saved, sizeof saved, "%s", x->dir);
        snprintf(x->dir, sizeof x->dir, "%s", path);
        x->depth++;
        r = iso_list(x->iso, lba, size, extract_entry, x);
        x->depth--;
        snprintf(x->dir, sizeof x->dir, "%s", saved);
        return r;
    }
    if (x->counting) { x->total += size; return 0; }
    if (!strcasecmp(name, "FCopLAPD.exe")) {
        x->exe_pending = 1;
        x->exe_lba = lba;
        x->exe_size = size;
        snprintf(x->exe_path, sizeof x->exe_path, "%s", path);
        return 0;
    }
    return copy_extent(x, path, lba, size);
}

static int has_exe(void *ctx, const char *name, u32 lba, u32 size, int dir)
{
    (void)ctx; (void)lba; (void)size;
    return !dir && !strcasecmp(name, "FCopLAPD.exe");
}

/* finds "FCopLAPD/FCopLAPD.exe" in the image */
static int find_sub(void *ctx, const char *name, u32 lba, u32 size, int dir)
{
    return dir && !strcasecmp(name, "FCopLAPD") && iso_list((Iso *)ctx, lba, size, has_exe, NULL) > 0;
}

static int install_from(const char *image, const char *dest)
{
    Iso iso;
    Extract x;
    int r;
    if (iso_open(&iso, image)) return -1;
    memset(&x, 0, sizeof x);
    x.iso = &iso;
    snprintf(x.dir, sizeof x.dir, "%s", dest);
    x.game_only = iso_list(&iso, iso.root, iso.root_len, find_sub, &iso) > 0;
    x.counting = 1;
    r = iso_list(&iso, iso.root, iso.root_len, extract_entry, &x);
    x.counting = 0;
    plat_log("installing from %s (volume %s, %llu MB)", image, iso.volume, (unsigned long long)(x.total >> 20));
    if (!r) ui_open();
    if (!r) r = iso_list(&iso, iso.root, iso.root_len, extract_entry, &x);
    if (!r && x.exe_pending) r = copy_extent(&x, x.exe_path, x.exe_lba, x.exe_size);
    fclose(iso.f);
    return r;
}

/* a CD image of the game: an ISO 9660 image that holds FCopLAPD.exe */
static int is_game_image(const char *path)
{
    Iso iso;
    int ok;
    if (iso_open(&iso, path)) return 0;
    ok = iso_list(&iso, iso.root, iso.root_len, find_sub, &iso) > 0 ||
         iso_list(&iso, iso.root, iso.root_len, has_exe, NULL) > 0;
    fclose(iso.f);
    return ok;
}

/* a game image lying in dir */
static int find_image(const char *dir, char *out, size_t n)
{
    static const char *const exts[] = {".cue", ".iso", ".bin", ".img"};
    DIR *d = opendir(dir);
    struct dirent *e;
    int found = 0;
    if (!d) return 0;
    for (unsigned k = 0; k < 4 && !found; k++) {
        rewinddir(d);
        while (!found && (e = readdir(d)))
            if (ends_with(e->d_name, exts[k])) {
                snprintf(out, n, "%s/%s", dir, e->d_name);
                found = is_game_image(out);
            }
    }
    closedir(d);
    return found;
}

/* the desktop's file chooser (zenity or kdialog on Linux) */
static int pick_image(char *out, size_t n)
{
    static const char *const cmds[] = {
        "zenity --file-selection --title='Choose your Future Cop CD image' "
        "--file-filter='CD images | *.iso *.ISO *.cue *.CUE *.bin *.BIN *.img *.IMG' 2>/dev/null",
        "kdialog --title 'Choose your Future Cop CD image' --getopenfilename . "
        "'*.iso *.ISO *.cue *.CUE *.bin *.BIN *.img *.IMG' 2>/dev/null",
    };
    for (unsigned k = 0; k < sizeof cmds / sizeof cmds[0]; k++) {
        FILE *p = popen(cmds[k], "r");
        int status;
        if (!p) continue;
        if (!fgets(out, (int)n, p)) out[0] = 0;
        status = pclose(p);
        out[strcspn(out, "\n")] = 0;
        if (WIFEXITED(status) && WEXITSTATUS(status) == 127) continue;  /* not installed */
        return out[0] ? 1 : 0;  /* chosen, or cancelled */
    }
    return -1;
}

/* No game files yet: ask for the CD image. Returns its path, "" when the player copied the
   files into dest by hand meanwhile, or NULL to quit. */
static const char *ask_image(const char *dest, char *buf, size_t n)
{
    static const char *const intro[] = {
        "Future Cop needs the files from your original CD.", "",
        "Choose your CD image (.iso, or .bin with .cue), or drop it",
        "onto this window. The game copies what it needs into", NULL};
    const char *lines[24];
    char path[16][64], status[128] = "";
    Button b[3] = {{"Quit", 0, {0, 0, 0, 0}}, {"Open folder", 2, {0, 0, 0, 0}}, {"Choose CD image", 1, {0, 0, 0, 0}}};
    int nl = 0, hover = -1;
    Uint32 checked = 0;
    SDL_Event ev;
    for (int k = 0; intro[k]; k++) lines[nl++] = intro[k];
    for (size_t k = 0, len = strlen(dest); k < len && nl < 16; k += 60, nl++) {  /* long paths: several lines */
        snprintf(path[nl], sizeof path[0], "\1  %.60s", dest + k);
        lines[nl] = path[nl];
    }
    lines[nl++] = "";
    lines[nl++] = "You can also copy the CD into that folder yourself:";
    lines[nl++] = "the game starts as soon as the files are there.";
    lines[nl] = NULL;
    if (ui_open()) {
        for (int k = 0; k < nl; k++) plat_log("%s", lines[k][0] == '\1' ? lines[k] + 1 : lines[k]);
        return NULL;
    }
    for (;;) {
        ui_draw(lines, status, b, 3, hover, 0, 0);
        if (!SDL_WaitEventTimeout(&ev, 500)) ev.type = SDL_FIRSTEVENT;
        if (SDL_GetTicks() - checked > 1000) {  /* copied by hand? */
            checked = SDL_GetTicks();
            if (find_game(dest, 3, buf, n)) { buf[0] = 0; return buf; }
        }
        if (ui_closed(&ev)) return NULL;
        if (ev.type == SDL_DROPFILE) {
            snprintf(buf, n, "%s", ev.drop.file);
            SDL_free(ev.drop.file);
            if (is_game_image(buf)) return buf;
            snprintf(status, sizeof status, "That file is not a Future Cop CD image.");
            continue;
        }
        switch (ui_button(&ev, b, 3, &hover)) {
        case 0:
            return NULL;
        case 2: {
            char cmd[1200];
            snprintf(cmd, sizeof cmd, "xdg-open '%s' >/dev/null 2>&1 &", dest);
            if (!strchr(dest, '\'') && system(cmd) == 0)
                snprintf(status, sizeof status, "Waiting for the files in that folder...");
            else
                snprintf(status, sizeof status, "Could not open the folder.");
            break;
        }
        case 1:
            switch (pick_image(buf, n)) {
            case 1:
                if (is_game_image(buf)) return buf;
                snprintf(status, sizeof status, "That file is not a Future Cop CD image.");
                break;
            case -1:
                snprintf(status, sizeof status, "No file chooser found: drop the image onto this window.");
                break;
            }
            break;
        }
    }
}

/* copy a settings file written by earlier versions (in the game folder) into user_info/ */
static void adopt(const char *name)
{
    char from[1200], to[1200], buf[4096];
    FILE *a, *b;
    size_t n;
    snprintf(to, sizeof to, "%s/%s", user_dir, name);
    if (is_file(to)) return;
    snprintf(from, sizeof from, "%s/%s", game_dir, name);
    if (!(a = fopen(from, "rb"))) return;
    if ((b = fopen(to, "wb"))) {
        while ((n = fread(buf, 1, sizeof buf, a))) fwrite(buf, 1, n, b);
        fclose(b);
        plat_log("copied %s into %s", name, user_dir);
    }
    fclose(a);
}

/* Fills game_dir and user_dir. argv[1] may name the game folder or a CD image. 0: ready */
int setup_paths(int argc, char **argv)
{
    char files[1100], image[1024] = "", *bp = SDL_GetBasePath();
    const char *arg = argc > 1 ? argv[1] : getenv("FCOP_GAME_DIR");
    snprintf(base_dir, sizeof base_dir, "%s", bp ? bp : "./");
    SDL_free(bp);
    if (strlen(base_dir) > 1 && base_dir[strlen(base_dir) - 1] == '/') base_dir[strlen(base_dir) - 1] = 0;
    snprintf(files, sizeof files, "%s/game_files", base_dir);

    if (arg && is_dir(arg) && find_game(arg, 3, game_dir, 1024)) {
        /* a game folder given on the command line (tests, or files kept elsewhere) */
    } else if (!find_game(files, 3, game_dir, 1024)) {
        if (mkdir(files, 0755) && errno != EEXIST) {
            plat_log("cannot create %s: %s", files, strerror(errno));
            return -1;
        }
        if (arg && is_file(arg) && is_game_image(arg)) snprintf(image, sizeof image, "%s", arg);
        else if (!find_image(base_dir, image, sizeof image) && !find_image(files, image, sizeof image) &&
                 !ask_image(files, image, sizeof image)) {
            ui_close();
            return -1;
        }
        switch (image[0] ? install_from(image, files) : 0) {  /* "": copied by hand */
        case 0: break;
        case -2: plat_log("installation cancelled"); ui_close(); return -1;
        default: {
            char why[160];
            const char *lines[] = {"Could not copy the game files from your CD image:", "", why, NULL};
            snprintf(why, sizeof why, "\1%.60s", strerror(errno));
            ui_message(lines);
            ui_close();
            return -1;
        }
        }
        ui_close();
        if (!find_game(files, 3, game_dir, 1024)) {
            static const char *const lines[] = {"The CD image did not contain FCopLAPD.exe.", NULL};
            ui_message(lines);
            ui_close();
            return -1;
        }
    }

    snprintf(user_dir, sizeof user_dir, "%s/user_info", base_dir);
    if (mkdir(user_dir, 0755) && errno != EEXIST) {
        /* the executable's folder is read-only: keep them in the user's data folder */
        char *pref = SDL_GetPrefPath("Electronic Arts", "Future Cop");
        snprintf(user_dir, sizeof user_dir, "%s", pref ? pref : ".");
        SDL_free(pref);
        if (strlen(user_dir) > 1 && user_dir[strlen(user_dir) - 1] == '/') user_dir[strlen(user_dir) - 1] = 0;
    }
    adopt("registry.txt");
    adopt("controls.txt");
    adopt("save.cfg");
    plat_log("game files: %s; settings and saves: %s", game_dir, user_dir);
    return 0;
}
