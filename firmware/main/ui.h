#pragma once
// All functions here must be called with display_lock() held.

#ifdef __cplusplus
extern "C" {
#endif

#define UI_TITLE_MAX 24             // header title, printable ASCII (saved as "name")

void ui_init(const char *name);     // build the static UI; name = saved header title
void ui_update(void);               // repaint from g_spark + staleness
void ui_set_name(const char *name); // NULL/"" -> the Spark's hostname from the feed

#ifdef __cplusplus
}
#endif
