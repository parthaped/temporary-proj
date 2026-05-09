#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#define GROUPS 2
#define MAX_HIDDEN_PER_RES 512

typedef struct { int start; int length; int child_arg; } Task;

typedef struct {
    int max;
    long long sum;
    int len_done;
    int count;
    int pos[MAX_HIDDEN_PER_RES];
} Result;

/* loop until n bytes are moved; single read/write may be short on big buffers */
static ssize_t read_all(int fd, void *buf, size_t n) {
    size_t got = 0;
    char *p = (char *)buf;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return (ssize_t)got;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static ssize_t write_all(int fd, const void *buf, size_t n) {
    size_t put = 0;
    const char *p = (const char *)buf;
    while (put < n) {
        ssize_t r = write(fd, p + put, n - put);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        put += (size_t)r;
    }
    return (ssize_t)put;
}

static double get_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void log_trace_csv(pid_t pid, double s, double e, int n) {
    FILE *f = fopen("trace.csv", "a");
    if (!f) return;
    fprintf(f, "%d,%.6f,%.6f,%d\n", (int)pid, s, e, n);
    fclose(f);
}

static void explain_wait_status(pid_t pid, int st) {
    if (WIFEXITED(st))
        printf("Process %ld exited normally, exit status = %d\n",
               (long)pid, WEXITSTATUS(st));
    else if (WIFSIGNALED(st))
        printf("Process %ld terminated by signal %d\n",
               (long)pid, WTERMSIG(st));
    else if (WIFSTOPPED(st))
        printf("Process %ld stopped by signal %d\n",
               (long)pid, WSTOPSIG(st));
}

/* ---------------------------- worker ---------------------------- */
static void run_worker(int in_fd, int out_fd) {
    Task t;
    if (read_all(in_fd, &t, sizeof t) != (ssize_t)sizeof t) exit(127);

    printf("ECE 434 Sp26: I'm process %d with return arg %d and my parent is %d.\n",
           getpid(), t.child_arg, getppid());

    int *seg = (int *)malloc((size_t)t.length * sizeof(int));
    if (!seg) exit(127);
    if (read_all(in_fd, seg, (size_t)t.length * sizeof(int))
        != (ssize_t)((size_t)t.length * sizeof(int))) exit(127);

    double t0 = get_now();
    Result r;
    r.max = INT_MIN; r.sum = 0; r.len_done = t.length; r.count = 0;
    for (int j = 0; j < t.length; j++) {
        if (seg[j] > r.max) r.max = seg[j];
        r.sum += seg[j];
        if (seg[j] < 0 && r.count < MAX_HIDDEN_PER_RES) {
            r.pos[r.count++] = t.start + j;
            printf("ECE 434 Sp26: I am process %d with return arg %d. "
                   "I found the hidden key in position A[%d].\n",
                   getpid(), t.child_arg, t.start + j);
        }
    }

    write_all(out_fd, &r, sizeof r);
    log_trace_csv(getpid(), t0, get_now(), t.length);
    free(seg);
    exit(t.child_arg);
}

/* ------------------------- group leader ------------------------- */
static void run_group_leader(int leader_arg, int workers_in_group,
                             int in_fd, int out_fd) {
    Task g;
    if (read_all(in_fd, &g, sizeof g) != (ssize_t)sizeof g) exit(127);

    printf("ECE 434 Sp26: I'm group leader arg=%d (PID %d, PPID %d), "
           "handling A[%d..%d] across %d workers.\n",
           leader_arg, getpid(), getppid(),
           g.start, g.start + g.length - 1, workers_in_group);

    int *gbuf = (int *)malloc((size_t)g.length * sizeof(int));
    if (!gbuf) exit(127);
    if (read_all(in_fd, gbuf, (size_t)g.length * sizeof(int))
        != (ssize_t)((size_t)g.length * sizeof(int))) exit(127);

    int chunk = g.length / workers_in_group;
    int rem   = g.length % workers_in_group;
    int offset = 0;

    int (*p2w)[2] = malloc((size_t)workers_in_group * sizeof(int[2]));
    int (*w2p)[2] = malloc((size_t)workers_in_group * sizeof(int[2]));
    pid_t  *wpid  = malloc((size_t)workers_in_group * sizeof(pid_t));

    for (int i = 0; i < workers_in_group; i++) {
        if (pipe(p2w[i]) < 0 || pipe(w2p[i]) < 0) exit(127);
        pid_t pid = fork();
        if (pid == 0) {
            /*
             * Only close the wrong ends of OUR own pipe pair. Closing
             * earlier siblings' FDs is unsafe because their FD numbers may
             * have been recycled by the kernel and now point at a freshly
             * created pipe end we still need.
             */
            close(p2w[i][1]); close(w2p[i][0]);
            run_worker(p2w[i][0], w2p[i][1]);
            exit(127);
        }
        wpid[i] = pid;
        close(p2w[i][0]); close(w2p[i][1]);

        int len = chunk + (i < rem ? 1 : 0);
        Task wt;
        wt.start = g.start + offset;
        wt.length = len;
        wt.child_arg = leader_arg * 100 + (i + 1);
        write_all(p2w[i][1], &wt, sizeof wt);
        write_all(p2w[i][1], &gbuf[offset], (size_t)len * sizeof(int));
        close(p2w[i][1]);
        offset += len;
    }
    free(gbuf);

    Result agg;
    agg.max = INT_MIN; agg.sum = 0; agg.len_done = 0; agg.count = 0;
    for (int i = 0; i < workers_in_group; i++) {
        Result r;
        if (read_all(w2p[i][0], &r, sizeof r) != (ssize_t)sizeof r) continue;
        if (r.max > agg.max) agg.max = r.max;
        agg.sum += r.sum;
        agg.len_done += r.len_done;
        for (int k = 0; k < r.count && agg.count < MAX_HIDDEN_PER_RES; k++)
            agg.pos[agg.count++] = r.pos[k];
        close(w2p[i][0]);
    }

    write_all(out_fd, &agg, sizeof agg);

    for (int i = 0; i < workers_in_group; i++) {
        int st;
        waitpid(wpid[i], &st, 0);
        explain_wait_status(wpid[i], st);
    }
    free(p2w); free(w2p); free(wpid);
    exit(leader_arg);
}

/* ---------------------------- main ----------------------------- */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s L H PN\n", argv[0]);
        return 1;
    }
    int L = atoi(argv[1]);
    int H = atoi(argv[2]);
    int PN = atoi(argv[3]);
    if (PN < GROUPS) {
        fprintf(stderr, "PN must be at least %d so we can build a 2-level tree.\n", GROUPS);
        return 1;
    }
    (void)H;

    setbuf(stdout, NULL);

    FILE *fp = fopen("input.txt", "r");
    if (!fp) { perror("input.txt"); return 1; }
    int *arr = (int *)malloc((size_t)L * sizeof(int));
    if (!arr) { fclose(fp); return 1; }
    for (int i = 0; i < L; i++)
        if (fscanf(fp, "%d", &arr[i]) != 1) { fclose(fp); free(arr); return 1; }
    fclose(fp);

    FILE *trace = fopen("trace.csv", "w");
    if (trace) { fprintf(trace, "pid,start,end,elements\n"); fclose(trace); }

    double t_root_start = get_now();

    int g_workers[GROUPS];
    for (int g = 0; g < GROUPS; g++)
        g_workers[g] = PN / GROUPS + (g < (PN % GROUPS) ? 1 : 0);

    int p2g[GROUPS][2], g2p[GROUPS][2];
    pid_t gpid[GROUPS];
    int gchunk = L / GROUPS;
    int grem   = L % GROUPS;
    int goff   = 0;

    for (int g = 0; g < GROUPS; g++) {
        if (pipe(p2g[g]) < 0 || pipe(g2p[g]) < 0) { perror("pipe"); return 1; }
        pid_t pid = fork();
        if (pid == 0) {
            /* same FD-recycle hazard, only close our own wrong ends */
            close(p2g[g][1]); close(g2p[g][0]);
            run_group_leader(g + 1, g_workers[g], p2g[g][0], g2p[g][1]);
            exit(127);
        }
        gpid[g] = pid;
        close(p2g[g][0]); close(g2p[g][1]);

        int gl = gchunk + (g < grem ? 1 : 0);
        Task gt;
        gt.start = goff;
        gt.length = gl;
        gt.child_arg = (g + 1) * 1000;
        write_all(p2g[g][1], &gt, sizeof gt);
        write_all(p2g[g][1], &arr[goff], (size_t)gl * sizeof(int));
        close(p2g[g][1]);
        goff += gl;
    }
    free(arr);

    char cmd[64];
    snprintf(cmd, sizeof cmd, "pstree -p %d", (int)getpid());
    system(cmd);

    Result rg;
    int total_count = 0;
    int g_pos[MAX_HIDDEN_PER_RES * GROUPS];
    int g_max = INT_MIN;
    long long g_sum = 0;
    int g_len = 0;

    for (int g = 0; g < GROUPS; g++) {
        if (read_all(g2p[g][0], &rg, sizeof rg) != (ssize_t)sizeof rg) continue;
        if (rg.max > g_max) g_max = rg.max;
        g_sum += rg.sum;
        g_len += rg.len_done;
        for (int k = 0; k < rg.count
             && total_count < (int)(sizeof g_pos / sizeof g_pos[0]); k++)
            g_pos[total_count++] = rg.pos[k];
        close(g2p[g][0]);
    }

    for (int g = 0; g < GROUPS; g++) {
        int st; waitpid(gpid[g], &st, 0);
        explain_wait_status(gpid[g], st);
    }

    double t_total = get_now() - t_root_start;
    double avg = g_len > 0 ? (double)g_sum / (double)g_len : 0.0;

    printf("\n===== Final root summary =====\n");
    printf("Max=%d, Avg=%.2f over %d elements (L=%d), Total Runtime=%.4fs\n",
           g_max, avg, g_len, L, t_total);
    printf("Hidden keys found globally: %d\n", total_count);
    printf("Positions:");
    for (int k = 0; k < total_count; k++) printf(" A[%d]", g_pos[k]);
    printf("\n");

    return 0;
}
