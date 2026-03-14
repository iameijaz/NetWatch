#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200112L
#endif

/*
 */
#ifndef _WIN32
#endif

/*
 * netwatch — minimal internet connectivity monitor
 *
 * Detects when internet goes up or down by attempting a TCP connection
 * to a known IP (no DNS required). Alerts via system notification,
 * stdout, or a user-defined command.
 *
 * Cross-platform: Linux, macOS, Windows (Winsock2)
 *
 * Author: Ijaz Ahmed [Verbit]
 * License: MIT
 */

#ifdef _WIN32
  #define _WIN32_WINNT 0x0600
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  #define CLOSE_SOCKET(s) closesocket(s)
  #define SLEEP_MS(ms)    Sleep(ms)
  typedef SOCKET sock_t;
  #define INVALID_SOCK    INVALID_SOCKET
#else
  #include <sys/socket.h>
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <unistd.h>
  #include <fcntl.h>
  #include <sys/select.h>
  #include <time.h>
  #define CLOSE_SOCKET(s) close(s)
  static void _sleep_ms(int ms) { \
      struct timespec ts = {ms/1000, (ms%1000)*1000000L}; \
      nanosleep(&ts, NULL); \
  }
  #define SLEEP_MS(ms) _sleep_ms(ms)
  typedef int sock_t;
  #define INVALID_SOCK    (-1)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>

/* ── Defaults ─────────────────────────────────────────────────────── */

#define DEFAULT_HOST        "8.8.8.8"   /* Google DNS — reliable, no DNS needed */
#define FALLBACK_HOST       "1.1.1.1"   /* Cloudflare DNS — fallback */
#define DEFAULT_PORT        53          /* DNS port — almost never firewalled */
#define DEFAULT_INTERVAL_S  5           /* Check every 5 seconds */
#define DEFAULT_TIMEOUT_MS  2000        /* 2s connect timeout */
#define VERSION             "1.0.0"

/* ── State ────────────────────────────────────────────────────────── */

typedef enum { UNKNOWN = -1, OFFLINE = 0, ONLINE = 1 } net_state_t;

static volatile int g_running = 1;
static net_state_t  g_last_state = UNKNOWN;
static long         g_offline_since = 0;   /* epoch seconds */
static int          g_total_downs = 0;
static long         g_total_downtime = 0;  /* seconds */

/* ── Config ───────────────────────────────────────────────────────── */

typedef struct {
    const char *host;
    const char *fallback;
    int         port;
    int         interval_s;
    int         timeout_ms;
    int         quiet;          /* suppress periodic status lines */
    int         verbose;        /* show every check result */
    int         notify;         /* system notification on change */
    int         once;           /* check once and exit (0/1) */
    int         stats;          /* print stats on exit */
    const char *on_up;          /* command to run when coming online */
    const char *on_down;        /* command to run when going offline */
    const char *logfile;        /* log events to file */
} config_t;

static config_t cfg = {
    .host        = DEFAULT_HOST,
    .fallback    = FALLBACK_HOST,
    .port        = DEFAULT_PORT,
    .interval_s  = DEFAULT_INTERVAL_S,
    .timeout_ms  = DEFAULT_TIMEOUT_MS,
    .quiet       = 0,
    .verbose     = 0,
    .notify      = 0,
    .once        = 0,
    .stats       = 0,
    .on_up       = NULL,
    .on_down     = NULL,
    .logfile     = NULL,
};

/* ── Helpers ──────────────────────────────────────────────────────── */

static void signal_handler(int sig) {
    (void)sig;
    g_running = 0;
}

static const char *timestamp(void) {
    static char buf[32];
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", t);
    return buf;
}

static void log_event(const char *msg) {
    if (cfg.logfile) {
        FILE *f = fopen(cfg.logfile, "a");
        if (f) {
            fprintf(f, "[%s] %s\n", timestamp(), msg);
            fclose(f);
        }
    }
}


static int run_cmd(const char *cmd) {
    if (!cmd || !*cmd) return 0;
    int r = system(cmd);
    return r;
}

static void send_notify(const char *title, const char *body) {
#ifdef _WIN32
    /* Windows: use msg command (works without admin on local session) */
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "powershell -WindowStyle Hidden -Command \""
        "Add-Type -AssemblyName System.Windows.Forms;"
        "[System.Windows.Forms.MessageBox]::Show('%s','%s',"
        "[System.Windows.Forms.MessageBoxButtons]::OK,"
        "[System.Windows.Forms.MessageBoxIcon]::Information)"
        "\"", body, title);
    run_cmd(cmd);
#elif defined(__APPLE__)
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'display notification \"%s\" with title \"%s\"'",
        body, title);
    run_cmd(cmd);
#else
    /* Linux: notify-send (libnotify) */
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "notify-send '%s' '%s' 2>/dev/null", title, body);
    run_cmd(cmd);
#endif
}

static void run_hook(const char *cmd) {
    run_cmd(cmd);
}

/* ── Core: non-blocking TCP connect with timeout ──────────────────── */

static int try_connect(const char *host, int port, int timeout_ms) {
    sock_t s;
    struct sockaddr_in addr;
    int result = 0;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    if (addr.sin_addr.s_addr == (in_addr_t)-1) return 0;

    s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCK) return 0;

    /* Set non-blocking */
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(s, FIONBIO, &mode);
#else
    int flags = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, flags | O_NONBLOCK);
#endif

    connect(s, (struct sockaddr *)&addr, sizeof(addr));

    /* Wait for connection with select() */
    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(s, &wset);
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    if (select((int)s + 1, NULL, &wset, NULL, &tv) > 0) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &len);
        result = (err == 0) ? 1 : 0;
    }

    CLOSE_SOCKET(s);
    return result;
}

/* Try primary, then fallback */
static int check_connectivity(void) {
    if (try_connect(cfg.host, cfg.port, cfg.timeout_ms)) return 1;
    /* Small gap before fallback — avoids false positive on transient drop */
    SLEEP_MS(200);
    return try_connect(cfg.fallback, cfg.port, cfg.timeout_ms);
}

/* ── State transition handler ─────────────────────────────────────── */

static void on_state_change(net_state_t new_state) {
    long now = (long)time(NULL);

    if (new_state == ONLINE) {
        char msg[256];
        if (g_offline_since > 0) {
            long duration = now - g_offline_since;
            g_total_downtime += duration;
            snprintf(msg, sizeof(msg),
                "Internet restored (was down %lds)", duration);
        } else {
            snprintf(msg, sizeof(msg), "Internet is online");
        }

        printf("\033[32m[%s] ↑ ONLINE   — %s\033[0m\n",
               timestamp(), msg);
        fflush(stdout);
        log_event(msg);

        if (cfg.notify) send_notify("netwatch — Online ↑", msg);
        run_hook(cfg.on_up);
        g_offline_since = 0;

    } else {
        g_total_downs++;
        g_offline_since = now;
        const char *msg = "Internet connection lost";

        printf("\033[31m[%s] ↓ OFFLINE  — %s\033[0m\n",
               timestamp(), msg);
        fflush(stdout);
        log_event(msg);

        if (cfg.notify) send_notify("netwatch — Offline ↓", msg);
        run_hook(cfg.on_down);
    }

    g_last_state = new_state;
}

/* ── Stats ────────────────────────────────────────────────────────── */

static void print_stats(void) {
    long extra = 0;
    if (g_last_state == OFFLINE && g_offline_since > 0)
        extra = (long)time(NULL) - g_offline_since;

    printf("\n── netwatch stats ────────────────────\n");
    printf("  Outages detected : %d\n", g_total_downs);
    printf("  Total downtime   : %lds\n", g_total_downtime + extra);
    printf("──────────────────────────────────────\n");
}

/* ── Usage ────────────────────────────────────────────────────────── */

static void usage(const char *prog) {
    printf(
        "netwatch v%s — internet connectivity monitor\n\n"
        "Usage: %s [options]\n\n"
        "Options:\n"
        "  -h, --host <ip>       Primary check host (default: %s)\n"
        "  -p, --port <n>        TCP port to probe   (default: %d)\n"
        "  -i, --interval <s>    Check interval secs (default: %d)\n"
        "  -t, --timeout <ms>    Connect timeout ms  (default: %d)\n"
        "  -n, --notify          System notification on state change\n"
        "  -q, --quiet           Suppress periodic output\n"
        "  -v, --verbose         Show every check result\n"
        "  -1, --once            Check once, exit 0=online 1=offline\n"
        "  -s, --stats           Print stats on exit\n"
        "  -l, --log <file>      Log events to file\n"
        "      --on-up <cmd>     Command to run when coming online\n"
        "      --on-down <cmd>   Command to run when going offline\n"
        "      --help            Show this help\n"
        "      --version         Show version\n\n"
        "Examples:\n"
        "  netwatch                          # monitor with defaults\n"
        "  netwatch -i 10 -n                 # check every 10s, notify\n"
        "  netwatch -1                       # one-shot check\n"
        "  netwatch --on-up 'sync_data.sh'   # run script on reconnect\n"
        "  netwatch -q -l /var/log/net.log   # silent, log to file\n"
        "  netwatch -v -i 2                  # verbose, 2s interval\n\n"
        "Exit codes (--once mode): 0=online, 1=offline, 2=error\n",
        VERSION, prog, DEFAULT_HOST, DEFAULT_PORT,
        DEFAULT_INTERVAL_S, DEFAULT_TIMEOUT_MS
    );
}

/* ── Main ─────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {

#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return 2;
    }
#endif

    /* Parse args */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            /* -h is also --host if followed by an IP-looking arg */
            if (i + 1 < argc && argv[i+1][0] != '-' && !strcmp(argv[i], "-h")) {
                cfg.host = argv[++i];
            } else {
                usage(argv[0]);
                return 0;
            }
        }
        else if (!strcmp(argv[i], "--host")) {
            if (++i >= argc) { fprintf(stderr, "--host needs an argument\n"); return 2; }
            cfg.host = argv[i];
        }
        else if (!strcmp(argv[i], "--port") || !strcmp(argv[i], "-p")) {
            if (++i >= argc) { fprintf(stderr, "%s needs an argument\n", argv[i-1]); return 2; }
            cfg.port = atoi(argv[i]);
        }
        else if (!strcmp(argv[i], "--interval") || !strcmp(argv[i], "-i")) {
            if (++i >= argc) { fprintf(stderr, "%s needs an argument\n", argv[i-1]); return 2; }
            cfg.interval_s = atoi(argv[i]);
            if (cfg.interval_s < 1) cfg.interval_s = 1;
        }
        else if (!strcmp(argv[i], "--timeout") || !strcmp(argv[i], "-t")) {
            if (++i >= argc) { fprintf(stderr, "%s needs an argument\n", argv[i-1]); return 2; }
            cfg.timeout_ms = atoi(argv[i]);
        }
        else if (!strcmp(argv[i], "--notify") || !strcmp(argv[i], "-n")) {
            cfg.notify = 1;
        }
        else if (!strcmp(argv[i], "--quiet") || !strcmp(argv[i], "-q")) {
            cfg.quiet = 1;
        }
        else if (!strcmp(argv[i], "--verbose") || !strcmp(argv[i], "-v")) {
            cfg.verbose = 1;
        }
        else if (!strcmp(argv[i], "--once") || !strcmp(argv[i], "-1")) {
            cfg.once = 1;
        }
        else if (!strcmp(argv[i], "--stats") || !strcmp(argv[i], "-s")) {
            cfg.stats = 1;
        }
        else if (!strcmp(argv[i], "--log") || !strcmp(argv[i], "-l")) {
            if (++i >= argc) { fprintf(stderr, "%s needs an argument\n", argv[i-1]); return 2; }
            cfg.logfile = argv[i];
        }
        else if (!strcmp(argv[i], "--on-up")) {
            if (++i >= argc) { fprintf(stderr, "--on-up needs an argument\n"); return 2; }
            cfg.on_up = argv[i];
        }
        else if (!strcmp(argv[i], "--on-down")) {
            if (++i >= argc) { fprintf(stderr, "--on-down needs an argument\n"); return 2; }
            cfg.on_down = argv[i];
        }
        else if (!strcmp(argv[i], "--version")) {
            printf("netwatch v%s\n", VERSION);
            return 0;
        }
        else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return 2;
        }
    }

    /* One-shot mode */
    if (cfg.once) {
        int online = check_connectivity();
        printf("%s\n", online ? "online" : "offline");
#ifdef _WIN32
        WSACleanup();
#endif
        return online ? 0 : 1;
    }

    /* Continuous mode */
    signal(SIGINT,  signal_handler);
    signal(SIGTERM, signal_handler);

    if (!cfg.quiet) {
        printf("netwatch v%s — monitoring every %ds "
               "(Ctrl+C to stop)\n\n", VERSION, cfg.interval_s);
    }

    log_event("netwatch started");

    while (g_running) {
        int online = check_connectivity();
        net_state_t state = online ? ONLINE : OFFLINE;

        if (state != g_last_state) {
            on_state_change(state);
        } else if (cfg.verbose) {
            printf("[%s] %s\n", timestamp(),
                   online ? "online" : "offline");
            fflush(stdout);
        }

        /* Sleep in small increments so Ctrl+C is responsive */
        for (int ms = 0; ms < cfg.interval_s * 1000 && g_running; ms += 100)
            SLEEP_MS(100);
    }

    log_event("netwatch stopped");

    if (cfg.stats) print_stats();

    if (!cfg.quiet) printf("\nnetwatch stopped.\n");

#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
