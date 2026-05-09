/* enable sigqueue / SI_QUEUE / siginfo_t / usleep on Linux glibc */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#define GROUPS 2
#define MAX_HIDDEN_PER_RES 512

#ifndef RULE3_EXPERIMENT
#define RULE3_EXPERIMENT 1   /* 1 = custom SIGINT handler, 2 = SIG_IGN */
#endif

typedef struct { int start; int length; int child_arg; } Task;

typedef struct {
    int max;
    long long sum;
    int len_done;
    int count;
    int pos[MAX_HIDDEN_PER_RES];
} Result;

static ssize_t read_all(int fd, void *buf, size_t n) {
    size_t got = 0; char *p = (char *)buf;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        if (r == 0) return (ssize_t)got;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

static ssize_t write_all(int fd, const void *buf, size_t n) {
    size_t put = 0; const char *p = (const char *)buf;
    while (put < n) {
        ssize_t r = write(fd, p + put, n - put);
        if (r < 0) { if (errno == EINTR) continue; return -1; }
        put += (size_t)r;
    }
    return (ssize_t)put;
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
    else if (WIFCONTINUED(st))
        printf("Process %ld continued\n", (long)pid);
}

/* ---------- worker-side signal handlers ---------- */
static volatile sig_atomic_t got_sigint = 0;
static int worker_arg_for_handler = 0;

static void worker_sigint_handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)info; (void)ctx;
    got_sigint = 1;
    char b[200];
    int n = snprintf(b, sizeof b,
        "RULE3: worker arg=%d PID=%d PPID=%d caught SIGINT (handler does NOT exit).\n",
        worker_arg_for_handler, getpid(), getppid());
    write(STDERR_FILENO, b, n);
}

static void worker_sigusr1_handler(int sig, siginfo_t *info, void *ctx) {
    (void)sig; (void)ctx;
    int secret = (info && info->si_code == SI_QUEUE)
                 ? info->si_value.sival_int : SIGTERM;
    char b[200];
    int n = snprintf(b, sizeof b,
        "RULE2: worker arg=%d PID=%d caught SIGUSR1, secret signal = %d. Raising it.\n",
        worker_arg_for_handler, getpid(), secret);
    write(STDERR_FILENO, b, n);
    raise(secret);
}

static void install_worker_handlers(int my_arg) {
    worker_arg_for_handler = my_arg;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO;   /* deliberately NOT SA_RESTART */
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = worker_sigusr1_handler;
    sigaction(SIGUSR1, &sa, NULL);
#if RULE3_EXPERIMENT == 1
    sa.sa_sigaction = worker_sigint_handler;
    sigaction(SIGINT, &sa, NULL);
#else
    struct sigaction ig;
    memset(&ig, 0, sizeof ig);
    ig.sa_handler = SIG_IGN;
    sigaction(SIGINT, &ig, NULL);
#endif
}

/* ---------- worker ---------- */
static void run_worker(int in_fd, int out_fd) {
    Task t;
    if (read_all(in_fd, &t, sizeof t) != (ssize_t)sizeof t) _exit(127);

    install_worker_handlers(t.child_arg);

    printf("ECE 434 Sp26: I'm worker arg=%d (PID %d, PPID %d).\n",
           t.child_arg, getpid(), getppid());

    int *seg = (int *)malloc((size_t)t.length * sizeof(int));
    if (!seg) _exit(127);
    if (read_all(in_fd, seg, (size_t)t.length * sizeof(int))
        != (ssize_t)((size_t)t.length * sizeof(int))) _exit(127);

    Result r;
    r.max = INT_MIN; r.sum = 0; r.len_done = t.length; r.count = 0;
    for (int j = 0; j < t.length; j++) {
        if (seg[j] > r.max) r.max = seg[j];
        r.sum += seg[j];
        if (seg[j] < 0 && r.count < MAX_HIDDEN_PER_RES) {
            r.pos[r.count++] = t.start + j;
        }
    }
    free(seg);

    write_all(out_fd, &r, sizeof r);

    printf("Worker arg=%d PID=%d done computing (count=%d). raise(SIGTSTP).\n",
           t.child_arg, getpid(), r.count);
    raise(SIGTSTP);

    /* resume on SIGCONT */
    printf("Worker arg=%d PID=%d resumed by SIGCONT.\n",
           t.child_arg, getpid());

    /*
     * If RULE 2: SIGUSR1 handler will fire and call raise(secret), so we
     * never come back from the handler.
     * If RULE 1: nothing else happens, we wait the full 100s and exit.
     * If RULE 3: SIGINT will interrupt sleep, then we wait for SIGQUIT.
     */
    unsigned int remaining = sleep(100);

    if (got_sigint) {
        printf("Worker arg=%d PID=%d sleep returned %u sec early (SIGINT path). "
               "Now pausing to receive SIGQUIT.\n",
               t.child_arg, getpid(), remaining);
        while (1) pause();
    }

    printf("Worker arg=%d PID=%d exiting normally with code %d.\n",
           t.child_arg, getpid(), t.child_arg);
    exit(t.child_arg);
}

/* ---------- group leader ---------- */
static void run_group_leader(int leader_arg, int workers_in_group,
                             int in_fd, int out_fd) {
    Task g;
    if (read_all(in_fd, &g, sizeof g) != (ssize_t)sizeof g) _exit(127);

    printf("Group leader arg=%d (PID %d, PPID %d): A[%d..%d], %d workers.\n",
           leader_arg, getpid(), getppid(),
           g.start, g.start + g.length - 1, workers_in_group);

    int *gbuf = (int *)malloc((size_t)g.length * sizeof(int));
    if (!gbuf) _exit(127);
    if (read_all(in_fd, gbuf, (size_t)g.length * sizeof(int))
        != (ssize_t)((size_t)g.length * sizeof(int))) _exit(127);

    int chunk = g.length / workers_in_group;
    int rem   = g.length % workers_in_group;
    int offset = 0;

    int (*p2w)[2] = malloc((size_t)workers_in_group * sizeof(int[2]));
    int (*w2p)[2] = malloc((size_t)workers_in_group * sizeof(int[2]));
    pid_t  *wpid  = malloc((size_t)workers_in_group * sizeof(pid_t));
    Result *wres  = malloc((size_t)workers_in_group * sizeof(Result));

    for (int i = 0; i < workers_in_group; i++) {
        if (pipe(p2w[i]) < 0 || pipe(w2p[i]) < 0) _exit(127);
        pid_t pid = fork();
        if (pid == 0) {
            /* only close our own wrong ends; earlier siblings' FDs may
               have been recycled and would clobber a still-needed pipe */
            close(p2w[i][1]); close(w2p[i][0]);
            run_worker(p2w[i][0], w2p[i][1]);
            _exit(127);
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

    for (int i = 0; i < workers_in_group; i++) {
        if (read_all(w2p[i][0], &wres[i], sizeof(Result)) != (ssize_t)sizeof(Result)) {
            wres[i].count = 0; wres[i].max = INT_MIN;
            wres[i].sum = 0; wres[i].len_done = 0;
        }
        close(w2p[i][0]);
    }

    /* small pause so workers actually reach raise(SIGTSTP) before we kill */
    usleep(300 * 1000);

    int idx_max = 0, idx_min = 0;
    for (int i = 1; i < workers_in_group; i++) {
        if (wres[i].count > wres[idx_max].count) idx_max = i;
        if (wres[i].count < wres[idx_min].count) idx_min = i;
    }
    if (workers_in_group > 1 && idx_max == idx_min)
        idx_max = (idx_min + 1) % workers_in_group;

    printf("Leader arg=%d: max idx=%d (count=%d, PID %d), "
           "min idx=%d (count=%d, PID %d).\n",
           leader_arg,
           idx_max, wres[idx_max].count, wpid[idx_max],
           idx_min, wres[idx_min].count, wpid[idx_min]);

    /* RULE 1 - max */
    printf("Leader %d -> RULE 1 to worker idx=%d PID=%d "
           "(SIGCONT, child sleeps 100, exits).\n",
           leader_arg, idx_max, wpid[idx_max]);
    kill(wpid[idx_max], SIGCONT);

    /* RULE 2 - middles */
    for (int i = 0; i < workers_in_group; i++) {
        if (i == idx_max || i == idx_min) continue;
        printf("Leader %d -> RULE 2 to worker idx=%d PID=%d "
               "(SIGCONT + SIGUSR1 secret=%d).\n",
               leader_arg, i, wpid[i], SIGTERM);
        kill(wpid[i], SIGCONT);
        usleep(50 * 1000);
        union sigval sv;
        sv.sival_int = SIGTERM;
        if (sigqueue(wpid[i], SIGUSR1, sv) < 0) perror("sigqueue");
    }

    /* RULE 3 - min */
    if (workers_in_group > 1) {
        printf("Leader %d -> RULE 3 to worker idx=%d PID=%d "
               "(SIGCONT, parent sleep(10), SIGINT, sleep(2), SIGQUIT).\n",
               leader_arg, idx_min, wpid[idx_min]);
        kill(wpid[idx_min], SIGCONT);
        sleep(10);
        kill(wpid[idx_min], SIGINT);
        sleep(2);
        kill(wpid[idx_min], SIGQUIT);
    }

    /* aggregate; per spec the leader inherits the FEWEST worker's hidden keys */
    Result agg;
    agg.max = INT_MIN; agg.sum = 0; agg.len_done = 0; agg.count = 0;
    for (int i = 0; i < workers_in_group; i++) {
        if (wres[i].max > agg.max) agg.max = wres[i].max;
        agg.sum += wres[i].sum;
        agg.len_done += wres[i].len_done;
    }
    for (int k = 0; k < wres[idx_min].count && agg.count < MAX_HIDDEN_PER_RES; k++)
        agg.pos[agg.count++] = wres[idx_min].pos[k];

    write_all(out_fd, &agg, sizeof agg);

    for (int i = 0; i < workers_in_group; i++) {
        int st;
        waitpid(wpid[i], &st, 0);
        explain_wait_status(wpid[i], st);
    }
    free(p2w); free(w2p); free(wpid); free(wres);
    exit(leader_arg);
}

/* ---------- main / root ---------- */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s L H PN\n", argv[0]);
        return 1;
    }
    int L = atoi(argv[1]);
    int H = atoi(argv[2]);
    int PN = atoi(argv[3]);
    if (PN < 2 * GROUPS) PN = 2 * GROUPS;
    (void)H;

    setbuf(stdout, NULL);

    FILE *fp = fopen("input.txt", "r");
    if (!fp) { perror("input.txt"); return 1; }
    int *arr = (int *)malloc((size_t)L * sizeof(int));
    if (!arr) { fclose(fp); return 1; }
    for (int i = 0; i < L; i++)
        if (fscanf(fp, "%d", &arr[i]) != 1) { fclose(fp); free(arr); return 1; }
    fclose(fp);

    int g_workers[GROUPS];
    for (int g = 0; g < GROUPS; g++)
        g_workers[g] = PN / GROUPS + (g < (PN % GROUPS) ? 1 : 0);

    int p2g[GROUPS][2], g2p[GROUPS][2];
    pid_t gpid[GROUPS];
    int gchunk = L / GROUPS;
    int grem = L % GROUPS;
    int goff = 0;

    for (int g = 0; g < GROUPS; g++) {
        if (pipe(p2g[g]) < 0 || pipe(g2p[g]) < 0) { perror("pipe"); return 1; }
        pid_t pid = fork();
        if (pid == 0) {
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
    printf("\n[ROOT] pstree right after spawning the tree:\n");
    system(cmd);

    Result rg;
    int g_max = INT_MIN;
    long long g_sum = 0;
    int g_len = 0;
    int total_pos = 0;
    int g_pos[MAX_HIDDEN_PER_RES * GROUPS];

    for (int g = 0; g < GROUPS; g++) {
        if (read_all(g2p[g][0], &rg, sizeof rg) != (ssize_t)sizeof rg) continue;
        if (rg.max > g_max) g_max = rg.max;
        g_sum += rg.sum;
        g_len += rg.len_done;
        for (int k = 0; k < rg.count
             && total_pos < (int)(sizeof g_pos / sizeof g_pos[0]); k++)
            g_pos[total_pos++] = rg.pos[k];
        close(g2p[g][0]);
    }

    printf("\n[ROOT] pstree while RULE 1 branches are sleeping:\n");
    system(cmd);

    for (int g = 0; g < GROUPS; g++) {
        int st; waitpid(gpid[g], &st, 0);
        explain_wait_status(gpid[g], st);
    }

    double avg = g_len > 0 ? (double)g_sum / (double)g_len : 0.0;
    printf("\n===== ROOT FINAL =====\n");
    printf("Max=%d, Avg=%.2f over %d elements (L=%d)\n", g_max, avg, g_len, L);
    printf("Hidden keys reported up the tree (fewest-per-leader): %d\n", total_pos);
    printf("Positions:");
    for (int k = 0; k < total_pos; k++) printf(" A[%d]", g_pos[k]);
    printf("\n");

    return 0;
}
