/**
 * @file test_core.c
 * @brief White-box tests for invariants, memory ownership, queue, and shutdown.
 *
 * Linked with --wrap=malloc,free so that a real allocation failure can be forced
 * and the reported shutdown totals can be compared with actual free() calls.
 *
 *   test_core DATA_DIR                   run the tests
 *   test_core DATA_DIR --shutdown-probe  shut down with deliberately drifted totals
 */

#include "core.h"
#include "logger.h"
#include "memory.h"
#include "scheduler.h"
#include "session.h"
#include "shell.h"
#include "tasks.h"

#include <stdlib.h>
#include <string.h>

void *__real_malloc(size_t bytes);
void __real_free(void *pointer);
void *__wrap_malloc(size_t bytes);
void __wrap_free(void *pointer);

static bool fail_malloc;
static size_t free_calls;

void *__wrap_malloc(size_t bytes) {
    return fail_malloc ? NULL : __real_malloc(bytes);
}

void __wrap_free(void *pointer) {
    if (pointer)
        ++free_calls;
    __real_free(pointer);
}

static const char *directory;
static FILE *sink;
static int failures, total;

static void report(bool passed, const char *name) {
    ++total;
    if (!passed)
        ++failures;
    printf("%s %s\n", passed ? "PASS" : "FAIL", name);
}

/* A failed fixture must stop the run instead of producing misleading results. */
static void require(bool condition, const char *what) {
    if (condition)
        return;
    printf("FAIL fixture: %s\n", what);
    exit(1);
}

static void start(App *app) {
    require(app_init(app, directory, 2), "app_init");
}

/*
 * Hand-checked state at tick 4:
 *   P1 A  FINISHED session: ran [0,2), notified
 *   P2 B  READY:    ran [2,4), remaining 3, waited 2
 *   P3 C  WAITING:  never ran, waited 4 before being blocked
 *   P4 D  READY:    arrived at 4, never ran
 *   queue: P2 P4
 */
static void fixture(App *app) {
    start(app);
    require(session_start(app, "A", 2, 10), "create A");
    require(task_create(app, "B", 5, 20, false), "create B");
    require(task_create(app, "C", 3, 30, false), "create C");
    require(scheduler_step(app, sink), "first slice");
    require(scheduler_step(app, sink), "second slice");
    require(task_block(app, 3), "block C");
    require(task_create(app, "D", 4, 40, false), "create D");
}

static AllocationRecord *record_of(App *app, int pid) {
    size_t i;
    for (i = 0; i < MAX_TASKS; ++i)
        if (app->memory.records[i].pointer && app->memory.records[i].owner == pid)
            return &app->memory.records[i];
    require(false, "allocation record");
    return NULL;
}

static AllocationRecord *free_slot(App *app) {
    size_t i;
    for (i = 0; i < MAX_TASKS; ++i)
        if (!app->memory.records[i].pointer)
            return &app->memory.records[i];
    require(false, "free slot");
    return NULL;
}

/* Corrupt a valid state, expect rejection, then restore it for a clean shutdown. */
#define REJECTS(name, corruption) do { \
        App app, saved; \
        Task *A = &app.tasks[0], *B = &app.tasks[1], *C = &app.tasks[2], *D = &app.tasks[3]; \
        bool valid, rejected; \
        fixture(&app); \
        valid = app_check(&app); \
        saved = app; \
        (void)A; (void)B; (void)C; (void)D; \
        corruption; \
        rejected = !app_check(&app); \
        app = saved; \
        report(valid && rejected, "check rejects " name); \
        app_shutdown(&app); \
    } while (0)

static void test_check_records(void) {
    REJECTS("wrong PID", A->pid = 9);
    REJECTS("duplicate PID", A->pid = 2);
    REJECTS("empty name", A->name[0] = '\0');
    REJECTS("illegal name character", A->name[0] = '/');
    REJECTS("unterminated name", memset(A->name, 'x', sizeof A->name));
    REJECTS("RUNNING between commands", C->state = RUNNING);
    REJECTS("unknown state", C->state = (TaskState)9);
    REJECTS("zero burst", (D->burst = 0, D->remaining = 0));
    /* A's two ticks are moved to B so that no progress or clock rule fires first. */
    REJECTS("finished task with zero burst",
            (A->burst = 0, A->dispatches = 0, A->first_run = -1,
             B->burst = 7, B->first_run = 0, B->ready_wait = 0));
    REJECTS("burst over limit", (D->burst = MAX_TICKS + 1, D->remaining = MAX_TICKS + 1));
    REJECTS("negative remaining", D->remaining = -1);
    REJECTS("remaining over burst", D->remaining = 5);
    REJECTS("unfinished task with no work left", (B->burst = 2, B->remaining = 0));
    REJECTS("zero payload size", A->payload_bytes = 0);
    REJECTS("payload size over budget", A->payload_bytes = MEMORY_BUDGET + 1);
    REJECTS("finished session not notified", A->notified = false);
    REJECTS("unfinished task notified", B->notified = true);
    REJECTS("ordinary task notified", A->is_session = false);
}

static void test_check_timing(void) {
    REJECTS("negative arrival", A->arrival = -1);
    REJECTS("arrival after now", D->arrival = 5);
    REJECTS("arrivals out of creation order", (B->arrival = 1, B->ready_wait = 1));
    REJECTS("progress without dispatch", A->dispatches = 0);
    REJECTS("negative dispatches", B->dispatches = -1);
    REJECTS("more dispatches than ticks run", B->dispatches = 3);
    REJECTS("dispatch without progress", D->dispatches = 1);
    REJECTS("first run before any progress", D->first_run = 4);
    REJECTS("missing first run", B->first_run = -1);
    REJECTS("first run too late for progress", B->first_run = 3);
    REJECTS("completion of unfinished task", B->completion = 4);
    REJECTS("completion after now", A->completion = 5);
    REJECTS("completion before burst could end", A->completion = 1);
    REJECTS("completion too soon after first run", A->first_run = 1);
    REJECTS("negative waiting", D->ready_wait = -1);
    REJECTS("finished waiting over its lifetime", A->ready_wait = 1);
    REJECTS("unfinished waiting over its lifetime", C->ready_wait = 5);
    REJECTS("clock differs from executed ticks", app.now = 5);
}

static void test_check_shared_state(void) {
    REJECTS("task count over capacity", (app.task_count = MAX_TASKS + 1, app.next_pid = MAX_TASKS + 2));
    REJECTS("wrong next PID", app.next_pid = 9);
    REJECTS("zero quantum", app.quantum = 0);
    REJECTS("quantum over limit", app.quantum = MAX_TICKS + 1);
    REJECTS("zero experiment", app.experiment = 0);
    REJECTS("queue count over capacity", app.queue.count = MAX_TASKS + 1);
    /* Adding MAX_TASKS keeps the modulo order intact, so only the range check fires. */
    REJECTS("queue head out of range", app.queue.head += MAX_TASKS);
    REJECTS("READY task missing from queue", --app.queue.count);
    REJECTS("duplicate queue entry", app.queue.ids[(app.queue.head + app.queue.count++) % MAX_TASKS] = 2);
    REJECTS("unknown PID in queue", app.queue.ids[(app.queue.head + app.queue.count++) % MAX_TASKS] = 17);
    REJECTS("WAITING task in queue", (void)queue_push(&app.queue, 3));
    REJECTS("WAITING task queued in place of a READY one", app.queue.ids[app.queue.head] = 3);
    REJECTS("FINISHED task in queue", (void)queue_push(&app.queue, 1));
}

static void test_check_memory(void) {
    static char stray, other;
    REJECTS("used-byte drift", ++app.memory.used);
    REJECTS("allocation-count drift", ++app.memory.count);
    REJECTS("missing payload pointer", B->payload = NULL);
    REJECTS("payload pointer mismatch", B->payload = &stray);
    REJECTS("payload size mismatch", ++B->payload_bytes);
    REJECTS("payload kept after finish", A->payload = &stray);
    REJECTS("finished task owning memory",
            (*free_slot(&app) = (AllocationRecord){&stray, 1, 10}, app.memory.used += 10, ++app.memory.count));
    REJECTS("orphan allocation",
            (*free_slot(&app) = (AllocationRecord){&stray, 17, 1}, ++app.memory.used, ++app.memory.count));
    REJECTS("invalid owner",
            (*free_slot(&app) = (AllocationRecord){&stray, 0, 1}, ++app.memory.used, ++app.memory.count));
    REJECTS("zero-byte allocation",
            (*free_slot(&app) = (AllocationRecord){&stray, 17, 0}, ++app.memory.count));
    REJECTS("duplicate owner",
            (*free_slot(&app) = (AllocationRecord){&other, 2, 20}, app.memory.used += 20, ++app.memory.count));
    REJECTS("shared payload pointer", record_of(&app, 4)->pointer = record_of(&app, 2)->pointer);
    REJECTS("total over budget",
            (record_of(&app, 4)->bytes = MEMORY_BUDGET, D->payload_bytes = MEMORY_BUDGET,
             app.memory.used += MEMORY_BUDGET - 40));
}

static void test_check_valid_states(void) {
    App app;
    bool okay;
    int i;
    start(&app);
    report(app_check(&app), "check accepts an empty system");
    app_shutdown(&app);

    fixture(&app);
    okay = app_check(&app);
    while (app.queue.count)
        okay = scheduler_step(&app, sink) && app_check(&app) && okay;
    okay = task_wake(&app, 3) && app_check(&app) && okay;
    scheduler_run(&app, sink);
    okay = app_check(&app) && okay;
    report(okay, "check accepts every state of a block/wake lifecycle");
    app_shutdown(&app);

    start(&app);
    okay = task_create(&app, "Long", MAX_TICKS, MEMORY_BUDGET, false);
    app.quantum = MAX_TICKS;
    okay = app_check(&app) && okay;
    okay = scheduler_step(&app, sink) && app_check(&app) && okay;
    report(okay, "check accepts maximum burst, quantum, and payload");
    app_shutdown(&app);

    /* A fixed seed gives the same 20,000-operation sequence on each run. */
    srand(433);
    start(&app);
    okay = true;
    for (i = 0; i < 20000 && okay; ++i) {
        int pid = rand() % (MAX_TASKS + 1) + 1;
        switch (rand() % 16) {
            case 0: case 1: case 2: case 3:
                (void)task_create(&app, "T", rand() % 9 + 1, (size_t)(rand() % 5000 + 1), rand() % 2 == 0);
                break;
            case 4: case 5:
                (void)task_block(&app, pid);
                break;
            case 6: case 7:
                (void)task_wake(&app, pid);
                break;
            case 8:
                app.quantum = rand() % 6 + 1;
                break;
            case 9:
                if (rand() % 4 == 0)
                    app_reset(&app);
                break;
            default:
                (void)scheduler_step(&app, sink);
        }
        okay = app_check(&app);
    }
    report(okay, "check accepts 20000 random legal operations");
    app_shutdown(&app);
}

/* memory_check is public, so it must reject bad registries without app_check's help. */
static void test_memory_check(void) {
    static char one, two;
    Memory valid = {0}, m;
    valid.records[0] = (AllocationRecord){&one, 1, 10};
    valid.records[1] = (AllocationRecord){&two, 2, 20};
    valid.used = 30;
    valid.count = 2;
    report(memory_check(&valid), "memory_check accepts a valid registry");
    m = valid;
    m.records[0].owner = 0;
    report(!memory_check(&m), "memory_check rejects an invalid owner");
    m = valid;
    m.records[0].bytes = 0;
    m.used = 20;
    report(!memory_check(&m), "memory_check rejects a zero-byte record");
    m = valid;
    m.records[1].owner = 1;
    report(!memory_check(&m), "memory_check rejects a duplicate owner");
    m = valid;
    m.records[1].pointer = &one;
    report(!memory_check(&m), "memory_check rejects a duplicate pointer");
}

static void test_queue(void) {
    ReadyQueue queue = {0};
    int i, pid;
    bool okay = true;
    for (i = 1; i <= MAX_TASKS; ++i)
        okay = queue_push(&queue, i) && okay;
    report(okay && !queue_push(&queue, 99), "queue rejects a push when full");

    for (i = 1; i <= 15; ++i)
        okay = queue_pop(&queue, &pid) && pid == i && okay;
    report(!queue_push(&queue, 18), "queue rejects a duplicate PID");
    for (i = 21; i <= 30; ++i)
        okay = queue_push(&queue, i) && okay;
    /* 16..30 now spans the physical end of the array. */
    okay = queue_remove(&queue, 20) && !queue_remove(&queue, 20) && okay;
    for (i = 16; i <= 30; ++i)
        if (i != 20)
            okay = queue_pop(&queue, &pid) && pid == i && okay;
    report(okay && !queue_pop(&queue, &pid), "queue keeps FIFO order across wraparound and removal");
}

static void test_memory(void) {
    App app;
    bool okay;
    size_t before;
    start(&app);
    fail_malloc = true;
    okay = !task_create(&app, "NoMem", 1, 100, false);
    fail_malloc = false;
    okay = okay && strstr(app.error, "Host allocation failed") && app.task_count == 0 &&
           app.next_pid == 1 && app.queue.count == 0 && app.memory.used == 0 && app_check(&app);
    report(okay, "real malloc failure rolls task creation back");
    app_shutdown(&app);

    start(&app);
    require(task_create(&app, "A", 1, 100, false), "create A");
    before = free_calls;
    okay = memory_release(&app.memory, 1) && !memory_release(&app.memory, 1);
    report(okay && free_calls == before + 1, "a second release does not free again");
    app_shutdown(&app);

    start(&app);
    require(task_create(&app, "A", 1, 100, false), "create A");
    report(!memory_allocate(&app.memory, 1, 10, app.error) && app.memory.used == 100,
           "one PID cannot own two allocations");
    app_shutdown(&app);

    fixture(&app);
    app.memory.fail_next = true;
    before = free_calls;
    app_reset(&app);
    report(free_calls == before + 3 && app.memory.used == 0 && app.memory.count == 0 &&
           !app.memory.fail_next && app_check(&app), "reset frees every payload and disarms the hook");
    app_shutdown(&app);
}

static bool last_log_line(char *line, size_t size) {
    char path[PATH_SIZE], next[512];
    FILE *file;
    snprintf(path, sizeof path, "%s/events.log", directory);
    file = fopen(path, "r");
    if (!file)
        return false;
    line[0] = '\0';
    while (fgets(next, sizeof next, file))
        snprintf(line, size, "%s", next);
    fclose(file);
    return true;
}

static void test_shutdown(void) {
    App app;
    char line[512];
    size_t before;
    fixture(&app);
    before = free_calls;
    app_shutdown(&app);
    /* fclose may free inside libc, but only calls linked from our objects are counted. */
    report(free_calls == before + 3 && last_log_line(line, sizeof line) &&
           strstr(line, "used_bytes=0 allocations=0 released_allocations=3 released_bytes=90"),
           "shutdown totals match the real free calls");

    fixture(&app);
    app.memory.used += 7;
    ++app.memory.count;
    app_shutdown(&app);
    report(app.memory.used == 7 && app.memory.count == 1 && last_log_line(line, sizeof line) &&
           strstr(line, "used_bytes=7 allocations=1 released_allocations=3 released_bytes=90"),
           "shutdown exposes accounting drift instead of printing zero");
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s DATA_DIR [--shutdown-probe]\n", argv[0]);
        return 2;
    }
    directory = argv[1];
    sink = tmpfile();
    require(sink != NULL, "tmpfile");

    if (argc == 3) {
        App app;
        require(!strcmp(argv[2], "--shutdown-probe"), "known option");
        fixture(&app);
        fclose(sink);
        app.memory.used += 7;
        ++app.memory.count;
        return shell_run(&app, false);
    }
    test_check_valid_states();
    test_check_records();
    test_check_timing();
    test_check_shared_state();
    test_check_memory();
    test_memory_check();
    test_queue();
    test_memory();
    test_shutdown();
    fclose(sink);
    printf("DONE %d\n", total);
    return failures ? 1 : 0;
}
