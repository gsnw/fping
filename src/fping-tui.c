/*
 * fping-tui.c - ncurses TUI frontend for fping
 *
 * Build:
 *   gcc -Wall -Wextra -O2 -o fping-tui fping-tui.c -lncurses -lm
 *
 * Usage:
 *   fping-tui host1 host2 host3 ...
 *   fping-tui -g 192.168.1.0/24
 *   fping-tui -f hostfile.txt
 *
 * Keys:
 *   q / ESC  - Quit
 *   r        - Reset statistics
 *   s        - Sort by host name
 *   l        - Sort by packet loss
 *   p        - Sort by avg latency
 *   UP/DOWN  - Scroll host list
 */

#define _GNU_SOURCE
#include <locale.h>
#include <ncursesw/ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <float.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/select.h>

/* Configuration */

#define MAX_HOSTS        512
#define HOSTNAME_LEN     64
#define HISTORY_LEN      60     // Entries in the sparkline circular buffer
#define FPING_INTERVAL   "200"  // ms between pings per host
#define REFRESH_MS       200    // Screen refresh interval in ms

/* Data Structures */

typedef enum {
    HOST_UNKNOWN,
    HOST_ALIVE,
    HOST_TIMEOUT,
    HOST_UNREACHABLE
} HostStatus;

typedef struct {
    char       name[HOSTNAME_LEN];
    HostStatus status;

    double     rtt_last;    // ms, -1 = timeout / no reply yet
    double     rtt_min;
    double     rtt_max;
    double     rtt_sum;
    long       rtt_count;

    long       sent;
    long       received;

    // Ring buffer for Sparkline (0 = timeout, >0 = RTT in ms)
    double     history[HISTORY_LEN];
    int        history_idx;
    int        history_full;

    time_t     last_seen;
} HostEntry;

typedef enum { SORT_NAME, SORT_LOSS, SORT_LATENCY } SortMode;

/* Global variables */

static HostEntry     hosts[MAX_HOSTS];
static int           num_hosts    = 0;
static volatile int  running      = 1;
static pid_t         fping_pid    = -1;
static int           pipe_fd      = -1;   // End of the fping pipe
static int           scroll_offset = 0;
static SortMode      sort_mode    = SORT_NAME;

// Line buffer for the non-blocking pipe reader
static char  line_buf[512];
static int   line_pos = 0;

/* Sparkline */

static const char *spark_chars[] = {
    "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"
};
#define SPARK_LEVELS (int)(sizeof(spark_chars)/sizeof(spark_chars[0]))

/* Color Pair IDs */

#define COL_HEADER    1
#define COL_ALIVE     2
#define COL_TIMEOUT   3
#define COL_UNKNOWN   4
#define COL_SPARK_LO  5
#define COL_SPARK_HI  6
#define COL_BORDER    7
#define COL_TITLE     8
#define COL_LOSS_OK   9
#define COL_LOSS_WARN 10
#define COL_LOSS_BAD  11

/* Host Management */

static HostEntry *find_or_create_host(const char *name)
{
    for (int i = 0; i < num_hosts; i++)
        if (strcmp(hosts[i].name, name) == 0)
            return &hosts[i];

    if (num_hosts >= MAX_HOSTS)
        return NULL;

    HostEntry *h = &hosts[num_hosts++];
    memset(h, 0, sizeof(*h));
    strncpy(h->name, name, HOSTNAME_LEN - 1);
    h->status   = HOST_UNKNOWN;
    h->rtt_last = -1.0;
    h->rtt_min  = DBL_MAX;
    return h;
}

static void history_push(HostEntry *h, double val)
{
    h->history[h->history_idx] = val;
    h->history_idx = (h->history_idx + 1) % HISTORY_LEN;
    if (h->history_idx == 0) h->history_full = 1;
}

static void update_host_rtt(HostEntry *h, double rtt_ms)
{
    h->status   = HOST_ALIVE;
    h->rtt_last = rtt_ms;
    h->sent++;
    h->received++;
    if (rtt_ms < h->rtt_min) h->rtt_min = rtt_ms;
    if (rtt_ms > h->rtt_max) h->rtt_max = rtt_ms;
    h->rtt_sum += rtt_ms;
    h->rtt_count++;
    h->last_seen = time(NULL);
    history_push(h, rtt_ms);
}

static void update_host_timeout(HostEntry *h)
{
    h->status   = HOST_TIMEOUT;
    h->rtt_last = -1.0;
    h->sent++;
    history_push(h, 0.0);
}

/* fping line parser */
/*
 * Expected formats:
 *   "host : [seq], NNN bytes, RTT ms"
 *   "host : [seq], timed out"
 *   "host is unreachable"
 */
static void parse_fping_line(char *line)
{
    size_t len = strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
        line[--len] = '\0';
    if (len == 0) return;

    // Format with “ : ” -> Response or timeout
    char *colon = strstr(line, " : ");
    if (colon) {
        char hostname[HOSTNAME_LEN];
        int hlen = (int)(colon - line);
        if (hlen >= HOSTNAME_LEN) hlen = HOSTNAME_LEN - 1;
        memcpy(hostname, line, hlen);
        hostname[hlen] = '\0';
        while (hlen > 0 && hostname[hlen-1] == ' ')
            hostname[--hlen] = '\0';

        const char *rest = colon + 3;
        HostEntry *h = find_or_create_host(hostname);
        if (!h) return;

        if (strstr(rest, "timed out")) {
            update_host_timeout(h);
        } else {
            const char *ms_pos = strstr(rest, " ms");
            if (ms_pos) {
                const char *p = ms_pos - 1;
                while (p > rest && (*p == '.' || (*p >= '0' && *p <= '9')))
                    p--;
                update_host_rtt(h, atof(p + 1));
            }
        }
        return;
    }

    // host is unreachable
    char *unreach = strstr(line, " is unreachable");
    if (unreach) {
        char hostname[HOSTNAME_LEN];
        int hlen = (int)(unreach - line);
        if (hlen >= HOSTNAME_LEN) hlen = HOSTNAME_LEN - 1;
        memcpy(hostname, line, hlen);
        hostname[hlen] = '\0';

        HostEntry *h = find_or_create_host(hostname);
        if (h) {
            h->status = HOST_UNREACHABLE;
            h->sent++;
            history_push(h, 0.0);
        }
    }
}

/* Non-blocking pipe reader */

static void drain_pipe(void)
{
    char buf[256];
    ssize_t n;

    while ((n = read(pipe_fd, buf, sizeof(buf))) > 0) {
        for (ssize_t i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (line_pos > 0) {
                    line_buf[line_pos] = '\0';
                    parse_fping_line(line_buf);
                    line_pos = 0;
                }
            } else {
                if (line_pos < (int)sizeof(line_buf) - 1)
                    line_buf[line_pos++] = c;
            }
        }
    }
    // n == 0 -> fping exits; n == -1, errno == EAGAIN -> no more data
    if (n == 0)
        running = 0;
}

/* Sorting */

static int sorted_idx[MAX_HOSTS];

static double loss_pct(const HostEntry *h)
{
    if (h->sent == 0) return 0.0;
    return 100.0 * (h->sent - h->received) / h->sent;
}

static double avg_rtt(const HostEntry *h)
{
    if (h->rtt_count == 0) return DBL_MAX;
    return h->rtt_sum / h->rtt_count;
}

static int cmp_name(const void *a, const void *b)
{
    return strcmp(hosts[*(int*)a].name, hosts[*(int*)b].name);
}
static int cmp_loss(const void *a, const void *b)
{
    double la = loss_pct(&hosts[*(int*)a]);
    double lb = loss_pct(&hosts[*(int*)b]);
    if (la != lb) return (la < lb) ? -1 : 1;
    return strcmp(hosts[*(int*)a].name, hosts[*(int*)b].name);
}
static int cmp_latency(const void *a, const void *b)
{
    double pa = avg_rtt(&hosts[*(int*)a]);
    double pb = avg_rtt(&hosts[*(int*)b]);
    if (pa != pb) return (pa < pb) ? -1 : 1;
    return strcmp(hosts[*(int*)a].name, hosts[*(int*)b].name);
}

static void rebuild_sorted(void)
{
    for (int i = 0; i < num_hosts; i++) sorted_idx[i] = i;
    switch (sort_mode) {
        case SORT_NAME:    qsort(sorted_idx, num_hosts, sizeof(int), cmp_name);    break;
        case SORT_LOSS:    qsort(sorted_idx, num_hosts, sizeof(int), cmp_loss);    break;
        case SORT_LATENCY: qsort(sorted_idx, num_hosts, sizeof(int), cmp_latency); break;
    }
}

/* Sparkline */

static void draw_sparkline(WINDOW *win, int y, int x, int width,
                           const HostEntry *h)
{
    int count = h->history_full ? HISTORY_LEN : h->history_idx;
    if (count == 0) { mvwprintw(win, y, x, "%*s", width, ""); return; }

    double max_rtt = 0.0;
    int start = h->history_full ? h->history_idx : 0;
    for (int i = 0; i < count; i++) {
        double v = h->history[(start + i) % HISTORY_LEN];
        if (v > max_rtt) max_rtt = v;
    }
    if (max_rtt < 1.0) max_rtt = 1.0;

    int draw_count = (count < width) ? count : width;
    int data_start = (count < width)
        ? (h->history_full ? h->history_idx : 0)
        : ((start + count - draw_count) % HISTORY_LEN);

    int pad = width - draw_count;
    for (int p = 0; p < pad; p++) mvwaddch(win, y, x + p, ' ');

    for (int i = 0; i < draw_count; i++) {
        double v = h->history[(data_start + i) % HISTORY_LEN];
        if (v <= 0.0) {
            wattron(win, COLOR_PAIR(COL_TIMEOUT));
            mvwaddch(win, y, x + pad + i, '|');
            wattroff(win, COLOR_PAIR(COL_TIMEOUT));
        } else {
            int level = (int)((v / max_rtt) * (SPARK_LEVELS - 1));
            if (level < 0) level = 0;
            if (level >= SPARK_LEVELS) level = SPARK_LEVELS - 1;
            int col = (level < SPARK_LEVELS / 2) ? COL_SPARK_LO : COL_SPARK_HI;
            wattron(win, COLOR_PAIR(col));
            mvwaddstr(win, y, x + pad + i, spark_chars[level]);
            wattroff(win, COLOR_PAIR(col));
        }
    }
}

/* Main drawing routine */

static void draw_ui(WINDOW *win)
{
    int rows, cols;
    getmaxyx(win, rows, cols);
    werase(win);

    /* Titelzeile */
    wattron(win, COLOR_PAIR(COL_TITLE) | A_BOLD);
    mvwhline(win, 0, 0, ' ', cols);
    mvwprintw(win, 0, 2,
        " fping-tui  |  %d hosts  |  sort: %s  |"
        "  q=quit r=reset s=name l=loss p=latency",
        num_hosts,
        sort_mode == SORT_NAME ? "name" :
        sort_mode == SORT_LOSS ? "loss" : "latency");
    wattroff(win, COLOR_PAIR(COL_TITLE) | A_BOLD);

    // Column header
    int spark_w = 20;
    int rtt_col  = cols - 1;
    int spark_col = rtt_col - 30;
    if (spark_col < 30) { spark_col = 30; spark_w = rtt_col - spark_col - 1; }

    wattron(win, COLOR_PAIR(COL_HEADER) | A_BOLD);
    mvwprintw(win, 1, 0, "%-*s %5s  %6s %6s %6s  %5s  %-*s",
              24, "HOST", "SENT",
              "MIN", "AVG", "MAX", "LOSS%",
              spark_w, "HISTORY");
    wattroff(win, COLOR_PAIR(COL_HEADER) | A_BOLD);

    // dividing line
    wattron(win, COLOR_PAIR(COL_BORDER));
    mvwhline(win, 2, 0, ACS_HLINE, cols);
    wattroff(win, COLOR_PAIR(COL_BORDER));

    // Host lines
    int visible_rows = rows - 4;
    if (visible_rows < 1) visible_rows = 1;

    rebuild_sorted();

    if (scroll_offset > num_hosts - visible_rows)
        scroll_offset = num_hosts - visible_rows;
    if (scroll_offset < 0) scroll_offset = 0;

    for (int row = 0; row < visible_rows; row++) {
        int idx = scroll_offset + row;
        if (idx >= num_hosts) break;

        const HostEntry *h = &hosts[sorted_idx[idx]];
        int y = 3 + row;

        int status_col;
        const char *status_mark;
        switch (h->status) {
            case HOST_ALIVE:       status_col = COL_ALIVE;   status_mark = "●"; break;
            case HOST_TIMEOUT:     status_col = COL_TIMEOUT; status_mark = "○"; break;
            case HOST_UNREACHABLE: status_col = COL_TIMEOUT; status_mark = "✗"; break;
            default:               status_col = COL_UNKNOWN; status_mark = "?"; break;
        }

        wattron(win, COLOR_PAIR(status_col));
        mvwaddstr(win, y, 0, status_mark);
        wattroff(win, COLOR_PAIR(status_col));
        mvwprintw(win, y, 2, "%-22.22s", h->name);
        mvwprintw(win, y, 25, "%5ld ", h->sent);

        if (h->rtt_count > 0) {
            wattron(win, COLOR_PAIR(COL_ALIVE));
            mvwprintw(win, y, 32, "%6.1f %6.1f %6.1f ",
                      h->rtt_min, avg_rtt(h), h->rtt_max);
            wattroff(win, COLOR_PAIR(COL_ALIVE));
        } else {
            wattron(win, COLOR_PAIR(COL_UNKNOWN));
            mvwprintw(win, y, 32, "%6s %6s %6s ", "---", "---", "---");
            wattroff(win, COLOR_PAIR(COL_UNKNOWN));
        }

        double lp = loss_pct(h);
        int loss_col = (lp < 1.0)  ? COL_LOSS_OK  :
                       (lp < 10.0) ? COL_LOSS_WARN : COL_LOSS_BAD;
        wattron(win, COLOR_PAIR(loss_col) | A_BOLD);
        mvwprintw(win, y, 53, "%5.1f%%", lp);
        wattroff(win, COLOR_PAIR(loss_col) | A_BOLD);

        draw_sparkline(win, y, 61, spark_w, h);
    }

    // Scroll indicators
    if (scroll_offset > 0) {
        wattron(win, COLOR_PAIR(COL_HEADER));
        mvwprintw(win, 3, cols - 3, " ▲ ");
        wattroff(win, COLOR_PAIR(COL_HEADER));
    }
    if (scroll_offset + visible_rows < num_hosts) {
        wattron(win, COLOR_PAIR(COL_HEADER));
        mvwprintw(win, rows - 2, cols - 3, " ▼ ");
        wattroff(win, COLOR_PAIR(COL_HEADER));
    }

    // Status bar
    time_t now = time(NULL);
    char timebuf[32];
    struct tm *tm_info = localtime(&now);
    strftime(timebuf, sizeof(timebuf), "%H:%M:%S", tm_info);

    wattron(win, COLOR_PAIR(COL_BORDER));
    mvwhline(win, rows - 1, 0, ' ', cols);
    mvwprintw(win, rows - 1, 1,
        "Updated: %s  |  Interval: %s ms  |  Hosts: %d/%d visible",
        timebuf, FPING_INTERVAL, visible_rows, num_hosts);
    wattroff(win, COLOR_PAIR(COL_BORDER));

    wrefresh(win);
}

/* ncurses init/cleanup */

static void init_ncurses(void)
{
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);
    curs_set(0);

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(COL_HEADER,    COLOR_WHITE,  COLOR_BLUE);
        init_pair(COL_ALIVE,     COLOR_GREEN,  -1);
        init_pair(COL_TIMEOUT,   COLOR_RED,    -1);
        init_pair(COL_UNKNOWN,   COLOR_YELLOW, -1);
        init_pair(COL_SPARK_LO,  COLOR_GREEN,  -1);
        init_pair(COL_SPARK_HI,  COLOR_YELLOW, -1);
        init_pair(COL_BORDER,    COLOR_BLACK,  COLOR_WHITE);
        init_pair(COL_TITLE,     COLOR_WHITE,  COLOR_BLUE);
        init_pair(COL_LOSS_OK,   COLOR_GREEN,  -1);
        init_pair(COL_LOSS_WARN, COLOR_YELLOW, -1);
        init_pair(COL_LOSS_BAD,  COLOR_RED,    -1);
    }
}

static void cleanup_ncurses(void)
{
    endwin();
}

/* Signal handler */

static void sig_handler(int sig)
{
    (void)sig;
    running = 0;
    if (fping_pid > 0)
        kill(fping_pid, SIGTERM);
}

/* Launch fping */

static int launch_fping(int argc, char *argv[])
{
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return -1; }

    fping_pid = fork();
    if (fping_pid < 0) { perror("fork"); return -1; }

    if (fping_pid == 0) {
        /* Kind-Prozess */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        char **fping_argv = malloc((5 + argc + 1) * sizeof(char *));
        if (!fping_argv) _exit(1);
        int ai = 0;
        fping_argv[ai++] = "fping";
        fping_argv[ai++] = "-l";
        fping_argv[ai++] = "-e";
        fping_argv[ai++] = "-p";
        fping_argv[ai++] = FPING_INTERVAL;
        for (int i = 0; i < argc; i++) fping_argv[ai++] = argv[i];
        fping_argv[ai] = NULL;
        execvp("fping", fping_argv);
        perror("execvp fping");
        _exit(127);
    }

    // Parent process: Set read-end to non-blocking
    close(pipefd[1]);
    int flags = fcntl(pipefd[0], F_GETFL, 0);
    fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);
    return pipefd[0];
}

/* Reset statistics */

static void reset_stats(void)
{
    for (int i = 0; i < num_hosts; i++) {
        HostEntry *h = &hosts[i];
        h->rtt_last  = -1.0;
        h->rtt_min   = DBL_MAX;
        h->rtt_max   = 0.0;
        h->rtt_sum   = 0.0;
        h->rtt_count = 0;
        h->sent      = 0;
        h->received  = 0;
        memset(h->history, 0, sizeof(h->history));
        h->history_idx  = 0;
        h->history_full = 0;
        h->status       = HOST_UNKNOWN;
    }
    scroll_offset = 0;
}

/* fping-tui main */

int main(int argc, char *argv[])
{
    setlocale(LC_ALL, "");

    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s host [host ...]\n"
            "       %s -g 192.168.1.0/24\n"
            "       %s -f hostfile.txt\n"
            "\nKeys: q=quit  r=reset  s=sort-name  l=sort-loss  p=sort-latency\n",
            argv[0], argv[0], argv[0]);
        return 1;
    }

    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGCHLD, SIG_IGN);

    pipe_fd = launch_fping(argc - 1, argv + 1);
    if (pipe_fd < 0) {
        fprintf(stderr, "Failed to launch fping.\n");
        return 1;
    }

    init_ncurses();

    while (running) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipe_fd, &rfds);

        struct timeval tv = {
            .tv_sec  = 0,
            .tv_usec = REFRESH_MS * 1000
        };

        int sel = select(pipe_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel > 0 && FD_ISSET(pipe_fd, &rfds))
            drain_pipe();

        int ch = getch();
        switch (ch) {
            case 'q': case 'Q': case 27:
                running = 0; break;
            case 'r': case 'R':
                reset_stats(); break;
            case 's': case 'S':
                sort_mode = SORT_NAME; break;
            case 'l': case 'L':
                sort_mode = SORT_LOSS; break;
            case 'p': case 'P':
                sort_mode = SORT_LATENCY; break;
            case KEY_UP:
                if (scroll_offset > 0) scroll_offset--; break;
            case KEY_DOWN:
                scroll_offset++; break;
            case KEY_PPAGE:
                scroll_offset -= 10;
                if (scroll_offset < 0) scroll_offset = 0; break;
            case KEY_NPAGE:
                scroll_offset += 10; break;
            case KEY_RESIZE:
                break;
            default:
                break;
        }

        draw_ui(stdscr);
    }

    cleanup_ncurses();

    if (fping_pid > 0) {
        kill(fping_pid, SIGTERM);
        waitpid(fping_pid, NULL, 0);
    }
    close(pipe_fd);

    return 0;
}