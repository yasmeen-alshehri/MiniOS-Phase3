/**
 * @file scheduler.c
 * @brief Round-robin scheduling and metrics.
 *
 * Uses a circular FIFO queue and one virtual CPU. A slice advances the
 * virtual clock by min(quantum, remaining); there is no context-switch cost.
 */

#include "scheduler.h"
#include "tasks.h"
#include "logger.h"
#include "memory.h"
#include "session.h"

/**
 * Append a unique PID at the logical tail of the bounded circular queue.
 * Modulo arithmetic wraps the physical slot index at MAX_TASKS. Return false
 * for a full queue or a duplicate PID; the caller validates the task state.
 */
bool queue_push(ReadyQueue *queue, int pid) {
    size_t i;
    if (queue->count == MAX_TASKS)
        return false;
    for (i = 0; i < queue->count; ++i)
        if (queue->ids[(queue->head + i) % MAX_TASKS] == pid)
            return false;
    queue->ids[(queue->head + queue->count) % MAX_TASKS] = pid;
    ++queue->count;
    return true;
}

/**
 * Remove the oldest ready PID and advance the circular head.
 * Return false for an empty queue without reading an invalid slot.
 */
bool queue_pop(ReadyQueue *queue, int *pid) {
    if (queue->count == 0)
        return false;
    *pid = queue->ids[queue->head];
    queue->head = (queue->head + 1) % MAX_TASKS;
    --queue->count;
    return true;
}

/**
 * Remove a selected PID while preserving the FIFO order of all others.
 * Shift logical successors, including across the physical end of the array.
 * Return false when the requested PID is absent.
 */
bool queue_remove(ReadyQueue *queue, int pid) {
    size_t i, j;
    for (i = 0; i < queue->count; ++i) {
        if (queue->ids[(queue->head + i) % MAX_TASKS] != pid)
            continue;
        for (j = i; j + 1 < queue->count; ++j)
            queue->ids[(queue->head + j) % MAX_TASKS] =
                queue->ids[(queue->head + j + 1) % MAX_TASKS];
        --queue->count;
        return true;
    }
    return false;
}

/* run echoes at most this many slices to the console and event log; every
 * slice is still written to trace.csv, which remains the complete record. */
#define RUN_ECHO_LIMIT 200

/**
 * Count a trace failure for the exit status; warn only the first time per kind,
 * because a long run could otherwise repeat the same warning many thousands of times.
 */
static void trace_failure(App *app, bool *warned, const char *message) {
    ++app->logger.failures;
    if (!*warned) {
        *warned = true;
        fprintf(stderr, "%s Further failures of this kind are not reported.\n", message);
    }
}

/**
 * Flush the trace once and count a detected failure.
 */
static void trace_flush(App *app) {
    if (app->trace && fflush(app->trace) != 0)
        trace_failure(app, &app->trace_flush_warned, "WARNING: scheduling trace flush failed.");
}

/**
 * Dispatch one READY task for a single round-robin slice.
 * Count READY-only waiting, update first/completion times, and finish or requeue.
 * Return false when idle or when a queue entry is invalid; idle keeps time fixed.
 * With echo false, omit the console line and the per-slice DISPATCH/SLICE events
 * (a finishing slice is still logged) and leave the trace flush to the caller.
 */
static bool dispatch_slice(App *app, FILE *out, bool echo) {
    int pid, slice;
    size_t i;
    long long start;
    Task *task;
    char detail[160];
    /* 1. Select the oldest READY task. An empty queue does not advance time. */
    if (!queue_pop(&app->queue, &pid)) {
        fprintf(out, "IDLE: no READY tasks; virtual clock remains %lld.\n", app->now);
        logger_event(app, "IDLE", 0, "no READY tasks");
        return false;
    }
    task = task_find(app, pid);
    if (!task || task->state != READY) {
        snprintf(app->error, ERROR_SIZE, "Invalid scheduler queue entry.");
        return false;
    }
    /* 2. Dispatch it and limit the slice to its actual remaining work. */
    start = app->now;
    task->state = RUNNING;
    ++task->dispatches;
    if (echo)
        logger_event(app, "DISPATCH", pid, "READY->RUNNING");
    if (task->first_run < 0)
        task->first_run = start;
    slice = task->remaining < app->quantum ? task->remaining : app->quantum;
    /* WAITING tasks receive no ready-wait ticks. The running task is not queued. */
    for (i = 0; i < app->task_count; ++i)
        if (app->tasks[i].state == READY)
            app->tasks[i].ready_wait += slice;
    /* 3. Apply the simulated CPU work; no wall-clock waiting is involved. */
    app->now += slice;
    task->remaining -= slice;
    /* 4. A finished task keeps its result record but releases its payload. */
    if (task->remaining == 0) {
        task->state = FINISHED;
        task->completion = app->now;
        memory_release(&app->memory, pid);
        task->payload = NULL;
    } else {
        task->state = READY;
        /* The dequeue created a free slot and pid is absent, so push cannot fail. */
        (void)queue_push(&app->queue, pid);
    }
    /* 5. Persist the observed slice and then check for session completion. */
    if (echo)
        fprintf(out, "[%lld,%lld) P%d %-12s RUNNING -> %-8s remaining=%d\n",
                start, app->now, pid, task->name, state_name(task->state), task->remaining);
    if (echo || task->state == FINISHED) {
        snprintf(detail, sizeof detail, "start=%lld end=%lld remaining=%d state=%s used_bytes=%zu",
                 start, app->now, task->remaining, state_name(task->state), app->memory.used);
        logger_event(app, "SLICE", pid, detail);
    }
    if (app->trace &&
        fprintf(app->trace, "%d,%d,%lld,%lld,%d,%s\n", app->experiment, pid, start,
                app->now, task->remaining, state_name(task->state)) < 0)
        trace_failure(app, &app->trace_write_warned, "WARNING: scheduling trace write failed.");
    if (echo)
        trace_flush(app);
    session_complete(app, task, out);
    return true;
}

/**
 * Run one slice with full console, event, and trace output.
 */
bool scheduler_step(App *app, FILE *out) {
    return dispatch_slice(app, out, true);
}

/**
 * Execute slices until the ready queue is empty or dispatch fails.
 * WAITING tasks remain blocked; the scheduler never wakes them automatically.
 * Only the first RUN_ECHO_LIMIT slices are echoed; the trace keeps all of them.
 */
void scheduler_run(App *app, FILE *out) {
    long long slices = 0, hidden = 0;
    size_t i, waiting = 0;
    bool stopped = false;
    char detail[160];
    if (app->queue.count == 0) {
        (void)scheduler_step(app, out);
        return;
    }
    while (app->queue.count > 0) {
        bool echo = slices < RUN_ECHO_LIMIT;
        if (!dispatch_slice(app, out, echo)) {
            stopped = true;
            break;
        }
        ++slices;
        if (!echo)
            ++hidden;
    }
    trace_flush(app);
    if (hidden)
        fprintf(out, "... %lld further slice(s) not shown; full record in %s/trace.csv\n",
                hidden, app->data_dir);
    snprintf(detail, sizeof detail, "slices=%lld hidden=%lld end=%lld", slices, hidden, app->now);
    logger_event(app, "RUN", 0, detail);
    if (stopped) {
        fprintf(out, "Run stopped: %s\n", app->error);
        return;
    }
    /* Mention blocked work only when some task is actually WAITING. */
    for (i = 0; i < app->task_count; ++i)
        if (app->tasks[i].state == WAITING)
            ++waiting;
    if (waiting)
        fprintf(out, "Run paused at tick %lld: ready queue empty; %zu WAITING task(s) need wake.\n",
                app->now, waiting);
    else
        fprintf(out, "Run complete at tick %lld: no READY or WAITING tasks remain.\n", app->now);
}

/**
 * Print completed-task metrics as a console table or CSV.
 * Turnaround = completion - arrival; response = first run - arrival.
 * Waiting comes from the READY counter, which correctly excludes blocked time.
 *
 * The console form also prints system-wide metrics for the current experiment:
 *   dispatches  = slices granted to all retained tasks, including unfinished ones;
 *   makespan    = latest completion - earliest arrival, over FINISHED tasks;
 *   throughput  = finished tasks / makespan (makespan >= 1 because bursts >= 1);
 *   utilization = busy ticks / elapsed ticks, busy = sum(burst - remaining).
 * The clock advances only inside slices, so busy always equals elapsed time:
 * utilization is 100% by model construction and is labelled as such.
 */
void metrics_print(const App *app, FILE *out, bool csv) {
    size_t i, completed = 0, unfinished = 0;
    double turnaround_sum = 0, waiting_sum = 0, response_sum = 0;
    long long dispatches = 0, busy = 0, first_arrival = 0, last_completion = 0;
    fputs(csv ? "pid,name,arrival,burst,first_run,completion,turnaround,waiting,response,dispatches\n" :
          "PID NAME             ARR BURST FIRST DONE TURN WAIT RESP DISP\n", out);
    for (i = 0; i < app->task_count; ++i) {
        const Task *t = &app->tasks[i];
        long long turnaround, response;
        /* System totals include unfinished tasks because their slices used the CPU. */
        dispatches += t->dispatches;
        busy += t->burst - t->remaining;
        if (t->state != FINISHED) {
            ++unfinished;
            continue;
        }
        /* Incomplete tasks have no final metrics and were skipped above. */
        turnaround = t->completion - t->arrival;
        response = t->first_run - t->arrival;
        if (csv)
            fprintf(out, "%d,%s,%lld,%d,%lld,%lld,%lld,%lld,%lld,%d\n", t->pid, t->name,
                    t->arrival, t->burst, t->first_run, t->completion, turnaround, t->ready_wait,
                    response, t->dispatches);
        else
            fprintf(out, "%-3d %-16s %-3lld %-5d %-5lld %-4lld %-4lld %-4lld %-4lld %d\n",
                    t->pid, t->name, t->arrival, t->burst, t->first_run, t->completion,
                    turnaround, t->ready_wait, response, t->dispatches);
        if (completed == 0 || t->arrival < first_arrival)
            first_arrival = t->arrival;
        if (completed == 0 || t->completion > last_completion)
            last_completion = t->completion;
        ++completed;
        turnaround_sum += (double)turnaround;
        waiting_sum += (double)t->ready_wait;
        response_sum += (double)response;
    }
    if (csv)
        return;
    if (completed) {
        long long makespan = last_completion - first_arrival;
        fprintf(out, "Means (%zu finished): turnaround=%.2f waiting=%.2f response=%.2f ticks\n",
                completed, turnaround_sum / (double)completed, waiting_sum / (double)completed,
                response_sum / (double)completed);
        fprintf(out, "Makespan=%lld ticks [%lld,%lld) throughput=%.4f tasks/tick\n", makespan,
                first_arrival, last_completion, (double)completed / (double)makespan);
    } else
        fputs("No finished tasks; means, makespan, and throughput are unavailable.\n", out);
    if (unfinished)
        fprintf(out, "Note: %zu unfinished task(s) excluded from means, makespan, and throughput.\n",
                unfinished);
    fprintf(out, "Dispatches=%lld (all tasks in this experiment)\n", dispatches);
    if (app->now > 0)
        fprintf(out, "CPU utilization=%.2f%% (busy=%lld/elapsed=%lld ticks; idle time is not "
                "modelled, so this is 100%% by construction)\n",
                100.0 * (double)busy / (double)app->now, busy, app->now);
    else
        fputs("CPU utilization unavailable: no virtual time has elapsed.\n", out);
}

/**
 * Write current finished-task metrics to DATA_DIR/metrics.csv.
 * Check the stream and fclose because buffered writes may fail only on close.
 * Return false with app->error instead of claiming a failed export succeeded.
 */
bool metrics_export(App *app) {
    char path[PATH_SIZE + 20];
    FILE *stream;
    bool okay;
    snprintf(path, sizeof path, "%s/metrics.csv", app->data_dir);
    stream = fopen(path, "w");
    if (!stream) {
        snprintf(app->error, ERROR_SIZE, "Cannot open metrics.csv.");
        return false;
    }
    metrics_print(app, stream, true);
    okay = !ferror(stream);
    if (fclose(stream) != 0)
        okay = false;
    if (!okay) {
        snprintf(app->error, ERROR_SIZE, "Metrics write/close failed.");
        return false;
    }
    logger_event(app, "METRICS", 0, "metrics.csv exported; finished tasks only");
    return true;
}
