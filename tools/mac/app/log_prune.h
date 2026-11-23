// Which old session logs the launcher deletes (launcher.c, prune_logs). Plain C, header-only, so
// tools/mac/app/log_prune_test.c can test the policy alone.
//
// A previous session's log is evidence, and the session that is about to start must never cost it: a noisy session is
// held down by the game's own per-session cap (sdk/src/core/capped_file_sink.h), and the prune runs before the game
// starts, so the new log is not even in the list. The policy, for the logs of one game sorted oldest first:
//   - the newest `min_keep` are always kept, whatever their age or size;
//   - older ones go when they are older than `max_age_s`, or when keeping them would take the folder past `max_total`
//     bytes (the newest are counted first, so the oldest go first).
#pragma once
#include <stddef.h>

typedef struct { const char *name; long long size; long long mtime; } LogEntry;

#define LOG_KEEP_MIN 20                              // sessions that are always kept
#define LOG_KEEP_DAYS 30                             // older than this, and beyond the floor: deleted
#define LOG_KEEP_BYTES (2048LL * 1024 * 1024)        // total size of the older ones kept, with the floor's own size

// entries[0..n) oldest first. Sets del[i] to 1 for each entry to delete, 0 to keep.
static void log_prune_plan(const LogEntry *entries, int n, long long now, int min_keep, long long max_age_s,
                           long long max_total, char *del) {
    long long total = 0;
    for (int i = n - 1; i >= 0; i--) {
        total += entries[i].size;
        del[i] = 0;
        if (n - i <= min_keep) continue;                       // inside the floor
        if (now - entries[i].mtime > max_age_s || total > max_total) del[i] = 1;
    }
}
