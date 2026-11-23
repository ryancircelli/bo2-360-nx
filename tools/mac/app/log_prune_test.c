// Tests of the launcher's log retention policy (log_prune.h). Build and run:
//   cc -Wall -Werror -o /tmp/log_prune_test tools/mac/app/log_prune_test.c && /tmp/log_prune_test
#include <stdio.h>
#include <stdlib.h>
#include "log_prune.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

#define DAY 86400LL
#define MB (1024LL * 1024)

static int count_deleted(const char *del, int n) { int c = 0; for (int i = 0; i < n; i++) c += del[i]; return c; }

int main(void) {
    const long long now = 1000 * DAY;
    LogEntry e[64]; char del[64]; char names[64][8];
    for (int i = 0; i < 64; i++) { snprintf(names[i], sizeof names[i], "l%02d", i); e[i].name = names[i]; }

    // Fewer sessions than the floor: nothing goes, however old or large.
    for (int i = 0; i < 10; i++) { e[i].size = 900 * MB; e[i].mtime = now - 400 * DAY; }
    log_prune_plan(e, 10, now, 20, 30 * DAY, 2048 * MB, del);
    CHECK(count_deleted(del, 10) == 0);

    // 30 small recent sessions: the 10 oldest are over the floor but young and small, so all 30 stay.
    for (int i = 0; i < 30; i++) { e[i].size = 1 * MB; e[i].mtime = now - (30 - i) * 3600; }
    log_prune_plan(e, 30, now, 20, 30 * DAY, 2048 * MB, del);
    CHECK(count_deleted(del, 30) == 0);

    // Old sessions beyond the floor go by age; the newest 20 stay even when they are old too.
    for (int i = 0; i < 30; i++) { e[i].size = 1 * MB; e[i].mtime = now - 100 * DAY + i * DAY; }
    log_prune_plan(e, 30, now, 20, 30 * DAY, 2048 * MB, del);
    for (int i = 0; i < 10; i++) CHECK(del[i] == 1);
    for (int i = 10; i < 30; i++) CHECK(del[i] == 0);

    // The size budget deletes the oldest first, and only beyond the floor.
    for (int i = 0; i < 30; i++) { e[i].size = 100 * MB; e[i].mtime = now - (30 - i) * 3600; }   // 3000 MB in all
    log_prune_plan(e, 30, now, 20, 30 * DAY, 2048 * MB, del);
    // newest 20 = 2000 MB (floor); the 21st newest brings 2100 MB > 2048, so index 9 and everything older go.
    for (int i = 0; i < 10; i++) CHECK(del[i] == 1);
    for (int i = 10; i < 30; i++) CHECK(del[i] == 0);

    // The floor is kept even when it alone is over the budget.
    for (int i = 0; i < 25; i++) { e[i].size = 500 * MB; e[i].mtime = now - (25 - i) * 3600; }
    log_prune_plan(e, 25, now, 20, 30 * DAY, 2048 * MB, del);
    for (int i = 0; i < 5; i++) CHECK(del[i] == 1);
    for (int i = 5; i < 25; i++) CHECK(del[i] == 0);

    // A noisy newest session is never a reason to delete the sessions before it while they are inside the floor.
    for (int i = 0; i < 20; i++) { e[i].size = 10 * MB; e[i].mtime = now - (20 - i) * 3600; }
    e[19].size = 1500 * MB;
    log_prune_plan(e, 20, now, 20, 30 * DAY, 2048 * MB, del);
    CHECK(count_deleted(del, 20) == 0);

    log_prune_plan(e, 0, now, 20, 30 * DAY, 2048 * MB, del);   // n == 0 is fine

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("log_prune_test: all passed\n");
    return 0;
}
