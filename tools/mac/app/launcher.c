// Launcher of "Black Ops II.app" (CFBundleExecutable): works out where the game data, saves and logs are, then runs
// Contents/MacOS/bo2zm as a child and returns its exit code. Packed by tools/mac/pack_app.py.
//
// The child is not exec'ed: macOS privacy checks (TCC) attribute a child's file access to the app that started it,
// while a launcher that hands its process over gets EPERM on a removable volume (the data on an external SSD) with
// no prompt. The same approach is used by switch2mac's dm/mac/launcher.c (MIT, Copyright (c) 2026 seedds), which
// this file borrows the idea and the signal forwarding from; the code is written for this project.
//
// Game data folder (game/, update/, optional content/ with the DLC, linked into the support folder), first match wins:
//   1. $BO2_DATA
//   2. the first line of ~/Library/Application Support/bo2-360-nx/data_path.txt
//   3. Info.plist key BO2DataPath (pack_app.py --data-mode external --data-path)
//   4. Contents/Resources/data (pack_app.py --data-mode clone)
// Saves and settings: ~/Library/Application Support/bo2-360-nx (passed as --user_data_root).
// Logs: ~/Library/Logs/bo2-360-nx/bo2zm-<time>.log, the newest 20 kept (passed as --log_file).
// Extra options: ~/Library/Application Support/bo2-360-nx/args.txt, one "name = value" or "--name=value" per line
// (# starts a comment), added before the command line; the command line (open APP --args --name=value) comes last
// and an option given there replaces the same name from args.txt or the launcher.
// Ports: the game binds guest ports 1000 and 1001 at host UDP offset+1000/+1001 (net_system_port_offset, default
// 30000). When no offset is given (args.txt, command line or REX_NET_SYSTEM_PORT_OFFSET) and either port is taken,
// e.g. by a second copy of the game, the launcher steps the offset by 1000 until both are free (20 tries) and logs
// the choice to launcher.log. A moved offset only matters for System Link, where every machine needs the same one.
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <errno.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
static pid_t g_child;
static void forward(int sig) { if (g_child > 0) kill(g_child, sig); }

static void alert(const char *title, const char *msg) {
    fprintf(stderr, "bo2: %s\n", msg);
    if (getenv("BO2_NO_DIALOG")) return;
    const char *script = "on run argv\ndisplay alert (item 1 of argv) message (item 2 of argv) as critical\nend run";
    char *argv[] = {"osascript", "-e", (char *)script, (char *)title, (char *)msg, NULL};
    pid_t pid; int st;
    if (!posix_spawnp(&pid, "osascript", NULL, NULL, argv, environ)) while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
}

static int is_dir(const char *p) { struct stat s; return !stat(p, &s) && S_ISDIR(s.st_mode); }

static void mkdirs(const char *path) {
    char tmp[PATH_MAX]; snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    mkdir(tmp, 0755);
}

static int info_string(const char *key, char *out, size_t n) {
    CFBundleRef b = CFBundleGetMainBundle(); if (!b) return 0;
    CFStringRef k = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
    CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(b, k); CFRelease(k);
    return v && CFGetTypeID(v) == CFStringGetTypeID() && CFStringGetCString(v, out, (CFIndex)n, kCFStringEncodingUTF8) && *out;
}

static void trim(char *s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '\t')) s[--n] = 0;
}

static int cmp_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static void prune_logs(const char *dir, int keep) {   // keep the newest `keep` bo2zm-*.log (names sort by time)
    DIR *d = opendir(dir); if (!d) return;
    char *names[4096]; int n = 0; struct dirent *e;
    while ((e = readdir(d)) && n < 4096)
        if (!strncmp(e->d_name, "bo2zm-", 6) && strstr(e->d_name, ".log")) names[n++] = strdup(e->d_name);
    closedir(d);
    qsort(names, n, sizeof *names, cmp_str);
    for (int i = 0; i < n; i++) {
        if (i < n - keep) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/%s", dir, names[i]); unlink(p); }
        free(names[i]);
    }
}

#define MAXARGS 256
static int udp_free(int port) {   // can this process bind UDP 0.0.0.0:port (as the game's XSocket::Bind does)?
    int fd = socket(AF_INET, SOCK_DGRAM, 0); if (fd < 0) return 1;
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((unsigned short)port); a.sin_addr.s_addr = htonl(INADDR_ANY);
    int ok = !bind(fd, (struct sockaddr *)&a, sizeof a);
    close(fd); return ok;
}

// The DLC packs (tools/install_dlc.py) sit in <data>/content/0000000000000000/<title>/{<type>,Headers/<type>}, but the
// game lists them under the user data root (sdk/src/system/xam/content_manager.cpp). Link each type folder of the data
// folder's content into the support folder, leaving anything already there alone (a real folder or an older link to
// another data folder is replaced only when it is a link). Saves are per profile XUID and never under this XUID.
static void link_entry(const char *src, const char *dst) {
    struct stat st;
    if (!lstat(dst, &st)) {
        if (!S_ISLNK(st.st_mode)) return;
        char cur[PATH_MAX]; ssize_t n = readlink(dst, cur, sizeof cur - 1);
        if (n > 0) { cur[n] = 0; if (!strcmp(cur, src)) return; }
        unlink(dst);
    }
    symlink(src, dst);
}
static void link_content(const char *data, const char *support) {
    char src_root[PATH_MAX], dst_root[PATH_MAX];
    snprintf(src_root, sizeof src_root, "%s/content/0000000000000000", data);
    snprintf(dst_root, sizeof dst_root, "%s/0000000000000000", support);
    DIR *titles = opendir(src_root); if (!titles) return;
    struct dirent *t;
    while ((t = readdir(titles))) {
        if (t->d_name[0] == '.') continue;
        char st_dir[PATH_MAX], dt_dir[PATH_MAX];
        snprintf(st_dir, sizeof st_dir, "%s/%s", src_root, t->d_name);
        snprintf(dt_dir, sizeof dt_dir, "%s/%s", dst_root, t->d_name);
        if (!is_dir(st_dir)) continue;
        DIR *types = opendir(st_dir); if (!types) continue;
        struct dirent *e;
        while ((e = readdir(types))) {
            if (e->d_name[0] == '.') continue;
            char s1[PATH_MAX], d1[PATH_MAX];
            snprintf(s1, sizeof s1, "%s/%s", st_dir, e->d_name);
            snprintf(d1, sizeof d1, "%s/%s", dt_dir, e->d_name);
            if (!strcmp(e->d_name, "Headers")) {   // Headers/<type>: link each type, the folder itself stays real
                DIR *h = opendir(s1); if (!h) continue;
                struct dirent *x;
                while ((x = readdir(h))) {
                    if (x->d_name[0] == '.') continue;
                    char s2[PATH_MAX], d2[PATH_MAX];
                    snprintf(s2, sizeof s2, "%s/%s", s1, x->d_name);
                    snprintf(d2, sizeof d2, "%s/%s", d1, x->d_name);
                    mkdirs(d1); link_entry(s2, d2);
                }
                closedir(h);
            } else if (is_dir(s1)) {
                mkdirs(dt_dir); link_entry(s1, d1);
            }
        }
        closedir(types);
    }
    closedir(titles);
}

static void log_line(const char *logs, const char *msg) {
    char p[PATH_MAX]; snprintf(p, sizeof p, "%s/launcher.log", logs);
    struct stat s; FILE *f = fopen(p, (!stat(p, &s) && s.st_size > 65536) ? "w" : "a");
    char stamp[32]; time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", &tm);
    if (f) { fprintf(f, "[%s] %s\n", stamp, msg); fclose(f); }
    fprintf(stderr, "bo2: %s\n", msg);
}

static char *g_argv[MAXARGS]; static int g_argc;
static void push(const char *s) { if (g_argc < MAXARGS - 1) g_argv[g_argc++] = strdup(s); }

static void read_args_file(const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return;
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char *p = line; while (*p == ' ' || *p == '\t') p++;
        trim(p);
        if (!*p || *p == '#') continue;
        if (!strncmp(p, "--", 2)) { push(p); continue; }
        char *eq = strchr(p, '='); if (!eq) continue;
        *eq = 0; char *name = p, *val = eq + 1; trim(name);
        while (*val == ' ' || *val == '\t') val++;
        size_t vl = strlen(val);
        if (vl >= 2 && val[0] == '"' && val[vl - 1] == '"') { val[vl - 1] = 0; val++; }
        char opt[2200]; snprintf(opt, sizeof opt, "--%s=%s", name, val); push(opt);
    }
    fclose(f);
}

int main(int argc, char **argv) {
    char self[PATH_MAX]; uint32_t sz = sizeof self;
    if (_NSGetExecutablePath(self, &sz)) { alert("Black Ops II", "cannot find the app's own path"); return 1; }
    char real[PATH_MAX]; if (!realpath(self, real)) snprintf(real, sizeof real, "%s", self);
    char macos[PATH_MAX]; snprintf(macos, sizeof macos, "%s", real);
    *strrchr(macos, '/') = 0;
    char res[PATH_MAX], res_raw[PATH_MAX]; snprintf(res_raw, sizeof res_raw, "%s/../Resources", macos);
    if (!realpath(res_raw, res)) snprintf(res, sizeof res, "%s", res_raw);

    const char *home = getenv("HOME"); if (!home) home = "/tmp";
    char support[PATH_MAX], logs[PATH_MAX], data[PATH_MAX] = "";
    const char *sup_env = getenv("BO2_SUPPORT_DIR"), *log_env = getenv("BO2_LOG_DIR");
    if (sup_env && *sup_env) snprintf(support, sizeof support, "%s", sup_env);
    else snprintf(support, sizeof support, "%s/Library/Application Support/bo2-360-nx", home);
    if (log_env && *log_env) snprintf(logs, sizeof logs, "%s", log_env);
    else snprintf(logs, sizeof logs, "%s/Library/Logs/bo2-360-nx", home);
    mkdirs(support); mkdirs(logs);

    const char *env_data = getenv("BO2_DATA");
    if (env_data && *env_data) snprintf(data, sizeof data, "%s", env_data);
    if (!*data) {
        char f[PATH_MAX]; snprintf(f, sizeof f, "%s/data_path.txt", support);
        FILE *fp = fopen(f, "r");
        if (fp) { if (fgets(data, sizeof data, fp)) trim(data); else data[0] = 0; fclose(fp); }
    }
    if (!*data) info_string("BO2DataPath", data, sizeof data);
    if (!*data) { char p[PATH_MAX]; snprintf(p, sizeof p, "%s/data", res); if (is_dir(p)) snprintf(data, sizeof data, "%s", p); }
    if (!*data) {
        char m[PATH_MAX + 400];
        snprintf(m, sizeof m, "No game data folder is set. Put the path of the folder that contains game/ and update/ on one line in\n%s/data_path.txt\nor set BO2_DATA.", support);
        alert("Black Ops II: game data not found", m); return 1;
    }
    char probe[PATH_MAX + 32];
    snprintf(probe, sizeof probe, "%s/game", data);
    if (!is_dir(probe)) {
        char m[PATH_MAX + 200]; snprintf(m, sizeof m, "%s has no game/ folder. Point data_path.txt (in %s) at the folder that holds game/ and update/.", data, support);
        alert("Black Ops II: game data not found", m); return 1;
    }

    link_content(data, support);

    char opt[PATH_MAX + 64], ex[PATH_MAX];
    snprintf(ex, sizeof ex, "%s/bo2zm", macos);
    push(ex);
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char stamp[32]; strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    int base = g_argc;
    snprintf(opt, sizeof opt, "--game_data_root=%s/game", data); push(opt);
    snprintf(opt, sizeof opt, "--update_data_root=%s/update", data); push(opt);
    snprintf(opt, sizeof opt, "--user_data_root=%s", support); push(opt);
    snprintf(opt, sizeof opt, "--log_file=%s/bo2zm-%s.log", logs, stamp); push(opt);
    snprintf(opt, sizeof opt, "%s/args.txt", support); read_args_file(opt);
    for (int i = 1; i < argc; i++) {   // the command line replaces earlier options of the same name
        if (!strncmp(argv[i], "-psn_", 5)) continue;   // the launch serial number Finder adds on old systems
        if (!strncmp(argv[i], "--", 2)) {
            char name[256]; snprintf(name, sizeof name, "%s", argv[i] + 2);
            char *eq = strchr(name, '='); if (eq) *eq = 0;
            for (int j = base; j < g_argc; j++) {
                if (!strncmp(g_argv[j], "--", 2) && !strncmp(g_argv[j] + 2, name, strlen(name)) && (g_argv[j][2 + strlen(name)] == '=' || !g_argv[j][2 + strlen(name)])) {
                    free(g_argv[j]); memmove(&g_argv[j], &g_argv[j + 1], (g_argc - j - 1) * sizeof *g_argv); g_argc--; j--;
                }
            }
        }
        push(argv[i]);
    }
    // The user's args.txt may repeat a launcher option too (e.g. --user_data_root): the later one wins, so drop the earlier.
    for (int i = base; i < g_argc; i++) {
        char *eq = strchr(g_argv[i], '='); if (!eq) continue;
        size_t nl = (size_t)(eq - g_argv[i]);
        for (int j = i + 1; j < g_argc; j++) {
            char *eq2 = strchr(g_argv[j], '=');
            if (eq2 && (size_t)(eq2 - g_argv[j]) == nl && !strncmp(g_argv[i], g_argv[j], nl)) { free(g_argv[i]); memmove(&g_argv[i], &g_argv[i + 1], (g_argc - i - 1) * sizeof *g_argv); g_argc--; i--; break; }
        }
    }
    // Free-port fallback, only when nobody chose an offset.
    int has_offset = getenv("REX_NET_SYSTEM_PORT_OFFSET") != NULL;
    for (int i = base; i < g_argc && !has_offset; i++)
        if (!strncmp(g_argv[i], "--net_system_port_offset", 24) && (g_argv[i][24] == '=' || !g_argv[i][24])) has_offset = 1;
    if (!has_offset && !(udp_free(31000) && udp_free(31001))) {
        int chosen = 0;
        for (int k = 1; k <= 20 && !chosen; k++) {
            int off = 30000 + 1000 * k;
            if (off + 1001 > 65535) break;
            if (udp_free(off + 1000) && udp_free(off + 1001)) chosen = off;
        }
        char m[200];
        if (chosen) {
            snprintf(opt, sizeof opt, "--net_system_port_offset=%d", chosen); push(opt);
            snprintf(m, sizeof m, "UDP 31000/31001 in use (another copy of the game?); using net_system_port_offset=%d (UDP %d/%d)", chosen, chosen + 1000, chosen + 1001);
        } else {
            snprintf(m, sizeof m, "UDP 31000/31001 in use and no free offset found in 20 tries; starting with the default 30000");
        }
        log_line(logs, m);
    }
    g_argv[g_argc] = NULL;

    prune_logs(logs, 20);
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "0", 0);   // the game's shaders expect IEEE results (see tools/mac/bo2zm.command)
    setenv("BO2_LOG_DIR", logs, 0);
    chdir(support);
    if (getenv("BO2_PRINT_COMMAND")) { for (int i = 0; i < g_argc; i++) printf("%s\n", g_argv[i]); return 0; }

    signal(SIGTERM, forward); signal(SIGINT, forward); signal(SIGHUP, forward);
    int err = posix_spawn(&g_child, ex, NULL, NULL, g_argv, environ);
    if (err) { char m[PATH_MAX]; snprintf(m, sizeof m, "cannot start %s: %s", ex, strerror(err)); alert("Black Ops II", m); return 1; }
    int st = 0;
    while (waitpid(g_child, &st, 0) < 0) if (errno != EINTR) break;
    if (WIFSIGNALED(st)) {
        char m[300]; snprintf(m, sizeof m, "The game stopped with signal %d. The log is in %s.", WTERMSIG(st), logs);
        fprintf(stderr, "bo2: %s\n", m);
        return 128 + WTERMSIG(st);
    }
    return WEXITSTATUS(st);
}
