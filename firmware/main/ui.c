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
static int       s_ncells;
static lv_obj_t *s_temp_val, *s_pow_val;

// 0..100 load -> green (idle) -> yellow -> red (pinned)
static lv_color_t heat_color(int pct)
{
    if (pct < 55) return lv_color_make(0x40 + pct * (0xE0 - 0x40) / 55, 0xB9, 0x10);
    return lv_color_make(0xE0 + (pct - 55) * (0xFF - 0xE0) / 45,
                         0xB9 - (pct - 55) * (0xB9 - 0x30) / 45, 0x10);
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
    lv_obj_set_scrollable(arc, false);             // its labels (below) must never scroll it
    lv_obj_set_style_arc_width(arc, 10, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, C_TRACK, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 10, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, C_GREEN, LV_PART_INDICATOR);
    rg->arc = arc;

    // children of the arc, aligned once: LVGL re-centres them when the text changes
    rg->val = mklabel(arc, &lv_font_montserrat_28, C_WHITE, "--");
    lv_obj_align(rg->val, LV_ALIGN_CENTER, 0, -6);
    rg->sub = mklabel(arc, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(rg->sub, LV_ALIGN_CENTER, 0, 16);

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
static char s_saved_name[UI_TITLE_MAX + 1];

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
    lv_obj_align(s_livetext, LV_ALIGN_TOP_RIGHT, -22, 22);
    s_srctext = mklabel(s_scr, &lv_font_montserrat_14, C_DIM, "");
    lv_obj_align(s_srctext, LV_ALIGN_TOP_RIGHT, -22, 46);

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
    int w = n ? (right - x0 - gap * (n - 1)) / n : 0;   // n <= 32 keeps w >= 10
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
    lv_obj_set_style_arc_color(rg->arc, fresh ? C_GREEN : C_TRACK, LV_PART_INDICATOR);
}

status_t ui_status(void)
{
    link_t link = net_link();
    status_in_t in = {
        .fresh = spark_age_ms() < SPARK_STALE_MS,
        .ever_fed = g_spark.valid,
        .source = g_spark.source,
        .wifi_configured = net_wifi_configured(),
        .url_configured = net_url_configured(),
        .link = link.state,
        .problem = link_problem(&link),
        .feed = net_feed(),
    };
    return status_of(&in);
}

void ui_update(void)
{
    char buf[32];
    status_t st = ui_status();                     // the header's state + reason (status.c)
    bool fresh = st.screen == SCREEN_ONLINE;
    lv_color_t col = fresh ? C_GREEN : st.screen == SCREEN_WAITING ? C_DIM : C_AMBER;

    refresh_title();
    lv_label_set_text(s_livetext, st.label);
    lv_obj_set_style_text_color(s_livetext, col, 0);
    lv_obj_set_style_bg_color(s_dot, col, 0);
    lv_obj_align_to(s_dot, s_livetext, LV_ALIGN_OUT_LEFT_MID, -10, 0);   // after the text's new width
    lv_label_set_text(s_srctext, st.reason);

    // fields are SPARK_UNKNOWN until the first sample, so no per-field `valid` checks
    set_ring(&s_cpu, g_spark.cpu, fresh);
    set_ring(&s_gpu, g_spark.gpu, fresh);
    set_ring(&s_mem, spark_mem_pct(), fresh);
    if (spark_mem_pct() != SPARK_UNKNOWN) {
        // decimal GB, rounded — matches NVIDIA's dashboard ("99 GB of 131 GB")
        snprintf(buf, sizeof(buf), "%ld/%ldG", (long)((g_spark.mem_used_mb + 500LL) / 1000),
                 (long)((g_spark.mem_total_mb + 500LL) / 1000));
        lv_label_set_text(s_mem.sub, buf);
    } else {
        lv_label_set_text(s_mem.sub, "");
    }

    if (g_spark.ncores != s_ncells) layout_cells(g_spark.ncores);
    for (int i = 0; i < s_ncells; i++)
        lv_obj_set_style_bg_color(s_cells[i], fresh ? heat_color(g_spark.cores[i]) : C_TRACK, 0);

    if (g_spark.temp_c != SPARK_UNKNOWN) {
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)g_spark.temp_c);   // degree sign
        lv_label_set_text(s_temp_val, buf);
    } else {
        lv_label_set_text(s_temp_val, "--");
    }
    if (g_spark.power_dw != SPARK_UNKNOWN) {
        snprintf(buf, sizeof(buf), "%d.%d W", (int)(g_spark.power_dw / 10), (int)(g_spark.power_dw % 10));
        lv_label_set_text(s_pow_val, buf);
    } else {
        lv_label_set_text(s_pow_val, "--");
    }
}
