#pragma once
// The Wi-Fi link supervisor's state machine. Plain C with no ESP-IDF calls, so
// the host tests drive every transition on a fake clock; net.c owns the one
// instance (under its lock), feeds it events, and does the radio work it asks for.
//
//   UNCONFIGURED --ssids saved--> BACKOFF --due--> JOINING --got IP--> CONNECTED
//                                    ^                |                    |
//                                    +----failed------+   <---dropped------+
//   any --wifi scan--> SCANNING --scan done--> BACKOFF (reset)
//
// BACKOFF sets its retry time on entry: never a deadline left over from an
// earlier state (that is what once kept a board offline after ~25 days joined).

#include <stdbool.h>
#include <stdint.h>

#define LINK_BACKOFF_MIN_MS   2000
#define LINK_BACKOFF_MAX_MS   30000
#define LINK_ATTEMPT_MS       20000   // a join with no outcome this long counts as failed
#define LINK_FAILS_TO_ROTATE  3       // failed joins before trying the next SSID

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LINK_UNCONFIGURED,   // no SSIDs saved
    LINK_BACKOFF,        // waiting for next_try_ms
    LINK_JOINING,        // one attempt in flight
    LINK_CONNECTED,      // joined, has an IP
    LINK_SCANNING,       // `wifi scan` has the radio
} link_state_t;

typedef enum {           // why joins are failing, for the screen
    LINK_PROBLEM_NONE,
    LINK_PROBLEM_NO_NETWORK,     // not found (or too weak)
    LINK_PROBLEM_BAD_PASSWORD,   // found, but the handshake failed
    LINK_PROBLEM_CANT_JOIN,      // any other refusal
} link_problem_t;

typedef struct {
    link_state_t state;
    uint32_t since_ms;       // when `state` was entered
    uint32_t next_try_ms;    // BACKOFF only: when to try again
    uint32_t backoff_ms;     // the wait after the next failed join
    int nssids, cur, fails;  // the SSIDs to try, the current one, failed joins on it
    int reason;              // the last failed join's reason code; 0 once joined
} link_t;

void link_reset(link_t *l, uint32_t now);              // boot, new config, scan done: start over
bool link_tick(link_t *l, uint32_t now);               // 1 Hz; true = start a join to SSID `cur`
void link_joined(link_t *l, uint32_t now);             // got an IP
void link_disconnected(link_t *l, int reason, uint32_t now);
void link_join_not_started(link_t *l, uint32_t now);   // the driver refused to start the join
void link_scan(link_t *l, uint32_t now);               // `wifi scan` takes the radio

link_problem_t link_problem(const link_t *l);
const char    *link_state_name(link_state_t s);

#ifdef __cplusplus
}
#endif
