#include "link.h"

// 802.11 / ESP-IDF disconnect reasons (esp_wifi_types_generic.h), copied so this
// file stays free of ESP-IDF and runs in the host tests.
#define REASON_4WAY_HANDSHAKE_TIMEOUT  15
#define REASON_NO_AP_FOUND             201
#define REASON_AUTH_FAIL               202
#define REASON_HANDSHAKE_TIMEOUT       204
#define REASON_NO_AP_IN_RSSI_THRESHOLD 212

static void enter(link_t *l, link_state_t s, uint32_t now)
{
    l->state = s;
    l->since_ms = now;
}

// BACKOFF's entry action: the retry time is always set here, relative to now.
static void enter_backoff(link_t *l, uint32_t wait_ms, uint32_t now)
{
    enter(l, LINK_BACKOFF, now);
    l->next_try_ms = now + wait_ms;
}

// A join that ended without an IP: wait, and wait longer each time.
static void join_failed(link_t *l, int reason, uint32_t now)
{
    l->fails++;
    if (reason) l->reason = reason;
    enter_backoff(l, l->backoff_ms, now);
    l->backoff_ms = l->backoff_ms * 2 > LINK_BACKOFF_MAX_MS ? LINK_BACKOFF_MAX_MS : l->backoff_ms * 2;
}

void link_reset(link_t *l, uint32_t now)
{
    l->cur = l->fails = l->reason = 0;
    l->backoff_ms = LINK_BACKOFF_MIN_MS;
    if (l->nssids > 0) enter_backoff(l, 0, now);
    else enter(l, LINK_UNCONFIGURED, now);
}

bool link_tick(link_t *l, uint32_t now)
{
    switch (l->state) {
    case LINK_BACKOFF:
        if ((int32_t)(now - l->next_try_ms) < 0) return false;   // set <= 30 s ago: no wrap
        if (l->fails >= LINK_FAILS_TO_ROTATE && l->nssids > 1) {
            l->cur = (l->cur + 1) % l->nssids;
            l->fails = 0;
        }
        enter(l, LINK_JOINING, now);
        return true;
    case LINK_JOINING:
        if (now - l->since_ms >= LINK_ATTEMPT_MS) join_failed(l, 0, now);   // no event at all
        return false;
    default:
        return false;
    }
}

void link_joined(link_t *l, uint32_t now)
{
    enter(l, LINK_CONNECTED, now);
    l->fails = l->reason = 0;
    l->backoff_ms = LINK_BACKOFF_MIN_MS;
}

void link_disconnected(link_t *l, int reason, uint32_t now)
{
    if (l->state == LINK_JOINING) join_failed(l, reason, now);
    else if (l->state == LINK_CONNECTED) enter_backoff(l, 0, now);   // dropped: rejoin now
    // BACKOFF / SCANNING / UNCONFIGURED: our own disconnect, already accounted for
}

void link_join_not_started(link_t *l, uint32_t now)
{
    if (l->state == LINK_JOINING) join_failed(l, 0, now);
}

void link_scan(link_t *l, uint32_t now)
{
    enter(l, LINK_SCANNING, now);
}

link_problem_t link_problem(const link_t *l)
{
    switch (l->reason) {
    case 0:
        return LINK_PROBLEM_NONE;
    case REASON_NO_AP_FOUND:
    case REASON_NO_AP_IN_RSSI_THRESHOLD:
        return LINK_PROBLEM_NO_NETWORK;
    case REASON_4WAY_HANDSHAKE_TIMEOUT:
    case REASON_AUTH_FAIL:
    case REASON_HANDSHAKE_TIMEOUT:
        return LINK_PROBLEM_BAD_PASSWORD;
    default:
        return LINK_PROBLEM_CANT_JOIN;
    }
}

const char *link_state_name(link_state_t s)
{
    static const char *const names[] = {"unconfigured", "backoff", "joining", "connected", "scanning"};
    return names[s];
}
