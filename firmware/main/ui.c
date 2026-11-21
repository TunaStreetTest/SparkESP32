// SparkESP32 UI — DGX Spark vitals on the 480x480 AMOLED.
// Three ring gauges (CPU / GPU / unified memory), a per-core heatmap,
// temperature + power tiles, and a liveness state that doubles as the
// Spark's missing power light. Every lv_* call here runs under display_lock().

#include "ui.h"
#include "net.h"
#include "spark_data.h"

#include <stdio.h>
#include <string.h>

#include "lvgl.h"

#define STALE_MS 5000          // no sample for this long -> OFFLINE / NO WI-FI
#define TITLE_MAX 24

// palette
#define C_BG     lv_color_hex(0x0A0A0A)
#define C_CARD   lv_color_hex(0x161616)
#define C_TRACK  lv_color_hex(0x242424)
#define C_GREEN  lv_color_hex(0x76B900)   // NVIDIA green
#define C_WHITE  lv_color_hex(0xF2F2F2)
#define C_DIM    lv_color_hex(0x8A8A8A)
#define C_AMBER  lv_color_hex(0xF0A030)

typedef struct {
    lv_obj_t *arc;
    lv_obj_t *val;    // big % in the center
    lv_obj_t *sub;    // small detail under the value ("99/131G")
} ring_t;

static lv_obj_t *s_scr, *s_name;
static lv_obj_t *s_dot, *s_livetext, *s_srctext;
static ring_t    s_cpu, s_gpu, s_mem;
static lv_obj_t *s_cells[SPARK_MAX_CORES];
static int       s_ncells = -1;
static lv_obj_t *s_temp_val, *s_pow_val;

// 0..100 load -> green (idle) -> yellow -> red (pinned)
static lv_color_t heat_color(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int r, g;
    if (pct < 55) {
        r = 0x40 + pct * (0xE0 - 0x40) / 55;
        g = 0xB9;
    } else {
        int t = (pct - 55) * 255 / 45;
        r = 0xE0 + t * (0xFF - 0xE0) / 255;
        g = 0xB9 - t * (0xB9 - 0x30) / 255;
    }
    return lv_color_make(r, g, 0x10);
}

static lv_obj_t *mklabel(lv_obj_t *p, const lv_font_t *f, lv_color_t c, const char *t)
{
    lv_obj_t *l = lv_label_create(p);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    lv_label_set_text(l, t);
    return l;
}

// a ring gauge centered at (cx,cy) with radius r, titled below
static void make_ring(ring_t *rg, int cx, int cy, int r, const char *title)
{
    lv_obj_t *arc = lv_arc_create(s_scr);
    lv_obj_set_size(arc, r * 2, r * 2);
    lv_obj_set_pos(arc, cx - r, cy - r);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 100);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, NULL, LV_PART_KNOB);
    lv_obj_set_clickable(arc, false);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, C_GREEN, LV_PART_INDICATOR);
    rg->arc = arc;

    rg->val = mklabel(s_scr, &lv_font_montserrat_28, C_WHITE, "--");
    lv_obj_align_to(rg->val, arc, LV_ALIGN_CENTER, 0, -6);
    rg->sub = mklabel(s_scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align_to(rg->sub, arc, LV_ALIGN_CENTER, 0, 16);

    // TOP_MID centers at x=240; offset by (cx-240) to sit under this ring
    lv_obj_t *t = mklabel(s_scr, &lv_font_montserrat_16, C_DIM, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, cx - 240, cy + r + 6);
}

// a bottom readout tile: label + big value
static lv_obj_t *make_tile(int x, int y, int w, int h, const char *label)
{
    lv_obj_t *card = lv_obj_create(s_scr);
    lv_obj_remove_style_all(card);
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, C_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_scrollable(card, false);

    lv_obj_t *l = mklabel(card, &lv_font_montserrat_16, C_DIM, label);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, 16, 14);
    lv_obj_t *v = mklabel(card, &lv_font_montserrat_48, C_WHITE, "--");
    lv_obj_align(v, LV_ALIGN_BOTTOM_LEFT, 16, -12);
    return v;
}

// Title precedence: the saved name, else the Spark's hostname from the feed,
// else a generic label until the first sample arrives.
static char s_saved_name[TITLE_MAX + 1];

static void refresh_title(void)
{
    const char *t = s_saved_name[0] ? s_saved_name : g_spark.host[0] ? g_spark.host : "DGX Spark";
    if (strcmp(lv_label_get_text(s_name), t) != 0) lv_label_set_text(s_name, t);
}

void ui_set_name(const char *name)
{
    strlcpy(s_saved_name, name ? name : "", sizeof(s_saved_name));
    refresh_title();
}

void ui_init(const char *name)
{
    s_scr = lv_screen_active();
    lv_obj_set_style_bg_color(s_scr, C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_scr, false);

    // header: title (dot-truncated so it can't run into the status) + liveness
    s_name = mklabel(s_scr, &lv_font_montserrat_28, C_WHITE, "");
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(s_name, 280);
    lv_obj_set_pos(s_name, 22, 14);
    ui_set_name(name);

    s_dot = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_dot);
    lv_obj_set_size(s_dot, 14, 14);
    lv_obj_set_style_radius(s_dot, 7, 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_COVER, 0);
    s_livetext = mklabel(s_scr, &lv_font_montserrat_16, C_DIM, "WAITING");
    s_srctext = mklabel(s_scr, &lv_font_montserrat_14, C_DIM, "");

    make_ring(&s_cpu, 92, 150, 58, "CPU");
    make_ring(&s_gpu, 240, 150, 58, "GPU");
    make_ring(&s_mem, 388, 150, 58, "MEM");

    lv_obj_t *ch = mklabel(s_scr, &lv_font_montserrat_14, C_DIM, "CPU CORES");
    lv_obj_set_pos(ch, 22, 250);
    for (int i = 0; i < SPARK_MAX_CORES; i++) {
        lv_obj_t *c = lv_obj_create(s_scr);
        lv_obj_remove_style_all(c);
        lv_obj_set_style_radius(c, 3, 0);
        lv_obj_set_style_bg_color(c, C_TRACK, 0);
        lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
        lv_obj_set_hidden(c, true);
        s_cells[i] = c;
    }

    s_temp_val = make_tile(16, 322, 216, 142, "DEVICE TEMP");
    s_pow_val = make_tile(248, 322, 216, 142, "DEVICE POWER");
}

// one row across the width, 22 px side margins
static void layout_cells(int n)
{
    const int x0 = 22, y = 276, h = 30, gap = 3, right = 480 - 22;
    int w = n > 0 ? (right - x0 - gap * (n - 1)) / n : 0;
    if (w < 6) w = 6;
    for (int i = 0; i < SPARK_MAX_CORES; i++) {
        bool show = i < n;
        if (show) {
            lv_obj_set_size(s_cells[i], w, h);
            lv_obj_set_pos(s_cells[i], x0 + i * (w + gap), y);
        }
        lv_obj_set_hidden(s_cells[i], !show);
    }
    s_ncells = n;
}

static void set_ring(ring_t *rg, int32_t pct, bool fresh)
{
    char buf[16];
    bool known = pct != SPARK_UNKNOWN;
    lv_arc_set_value(rg->arc, known ? pct : 0);
    if (known) snprintf(buf, sizeof(buf), "%d%%", (int)pct);
    lv_label_set_text(rg->val, known ? buf : "--");
    lv_obj_align_to(rg->val, rg->arc, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_arc_color(rg->arc, fresh ? C_GREEN : C_TRACK, LV_PART_INDICATOR);
}

void ui_update(void)
{
    char buf[32];
    bool fresh = spark_age_ms() < STALE_MS;

    // liveness — tells "Spark is down" apart from "display can't reach it":
    //   ONLINE    fresh sample (via USB or Wi-Fi)
    //   NO WI-FI  Wi-Fi configured but not joined
    //   SET HOST  joined Wi-Fi, but no agent URL configured
    //   OFFLINE   transport fine (or USB-only), but the Spark stopped answering
    //   WAITING   no Wi-Fi config and nothing on USB yet
    const char *txt;
    lv_color_t col;
    if (fresh) {
        txt = "ONLINE"; col = C_GREEN;
    } else if (net_wifi_configured() && !net_wifi_is_connected()) {
        txt = "NO WI-FI"; col = C_AMBER;
    } else if (net_wifi_configured() && !net_url_configured()) {
        txt = "SET HOST"; col = C_AMBER;
    } else if (net_wifi_configured() || g_spark.valid) {
        txt = "OFFLINE"; col = C_AMBER;
    } else {
        txt = "WAITING"; col = C_DIM;
    }
    refresh_title();
    lv_label_set_text(s_livetext, txt);
    lv_obj_set_style_text_color(s_livetext, col, 0);
    lv_obj_set_style_bg_color(s_dot, col, 0);
    lv_obj_align(s_livetext, LV_ALIGN_TOP_RIGHT, -22, 22);
    lv_obj_align_to(s_dot, s_livetext, LV_ALIGN_OUT_LEFT_MID, -10, 0);
    lv_label_set_text(s_srctext, !fresh ? "" :
                      g_spark.source == SRC_WIFI ? "via WI-FI" : "via USB");
    lv_obj_align(s_srctext, LV_ALIGN_TOP_RIGHT, -22, 46);

    set_ring(&s_cpu, g_spark.valid ? g_spark.cpu : SPARK_UNKNOWN, fresh);
    set_ring(&s_gpu, g_spark.valid ? g_spark.gpu : SPARK_UNKNOWN, fresh);
    set_ring(&s_mem, g_spark.valid ? spark_mem_pct() : SPARK_UNKNOWN, fresh);
    if (g_spark.valid && spark_mem_pct() != SPARK_UNKNOWN) {
        // decimal GB, rounded — matches NVIDIA's dashboard ("99 GB of 131 GB")
        snprintf(buf, sizeof(buf), "%ld/%ldG", (long)((g_spark.mem_used_mb + 500) / 1000),
                 (long)((g_spark.mem_total_mb + 500) / 1000));
        lv_label_set_text(s_mem.sub, buf);
    } else {
        lv_label_set_text(s_mem.sub, "");
    }
    lv_obj_align_to(s_mem.sub, s_mem.arc, LV_ALIGN_CENTER, 0, 16);

    if (g_spark.valid && g_spark.ncores != s_ncells) layout_cells(g_spark.ncores);
    for (int i = 0; i < s_ncells; i++)
        lv_obj_set_style_bg_color(s_cells[i], fresh ? heat_color(g_spark.cores[i]) : C_TRACK, 0);

    if (g_spark.valid && g_spark.temp_c != SPARK_UNKNOWN) {
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)g_spark.temp_c);   // degree sign
        lv_label_set_text(s_temp_val, buf);
    } else {
        lv_label_set_text(s_temp_val, "--");
    }
    if (g_spark.valid && g_spark.power_dw != SPARK_UNKNOWN) {
        snprintf(buf, sizeof(buf), "%d.%d W", (int)(g_spark.power_dw / 10), (int)(g_spark.power_dw % 10));
        lv_label_set_text(s_pow_val, buf);
    } else {
        lv_label_set_text(s_pow_val, "--");
    }
}
