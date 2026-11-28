/**
 * @file shell.c
 * @brief Command input and routing.
 *
 * Reads complete lines, validates arguments, and calls the matching service.
 * Commands execute between CPU slices; this module does not run real processes.
 */

#include "shell.h"
#include "logger.h"
#include "notes.h"
#include "scheduler.h"
#include "session.h"
#include "tasks.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define LINE_SIZE 2048

/**
 * Take the next whitespace-delimited word without allocating memory.
 * Replace its separator with a null terminator and advance the caller cursor.
 * This leaves the rest of a note command available as multi-word note text.
 */
static char *word(char **cursor) {
    char *start;
    while (**cursor == ' ' || **cursor == '\t')
        ++*cursor;
    if (!**cursor)
        return NULL;
    start = *cursor;
    while (**cursor && **cursor != ' ' && **cursor != '\t')
        ++*cursor;
    if (**cursor)
        *(*cursor)++ = '\0';
    return start;
}

/**
 * Parse a complete decimal integer within the caller-supplied range.
 * Reject signs, non-digits, overflow, and out-of-range values before writing
 * the result; a failed parse must not partially change application state.
 */
static bool number(const char *text, long minimum, long maximum, long *result) {
    char *end;
    long value;
    size_t i;
    if (!text || !*text)
        return false;
    for (i = 0; text[i]; ++i)
        if (text[i] < '0' || text[i] > '9')
            return false;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || *end || value < minimum || value > maximum)
        return false;
    *result = value;
    return true;
}

/**
 * Print the command grammar and remind the user that time is virtual.
 */
static void help(void) {
    puts("Commands (names use letters/digits/_/-; time is in virtual ticks):");
    puts("  task create NAME BURST BYTES   | tasks");
    puts("  session start NAME TICKS BYTES | block PID | wake PID");
    puts("  quantum N | step | run | metrics | export | memory");
    puts("  note create NAME TEXT | note append NAME TEXT");
    puts("  note read NAME | note delete NAME");
    puts("  check | reset | debug fail-next-allocation | help | quit");
}

/**
 * Route one validated command to the appropriate service module.
 * Return 0 for success/blank input, 1 for a recoverable command error, or 2
 * for quit. The shared syntax label reports malformed commands consistently.
 */
static int execute(App *app, char *line) {
    char *cursor = line, *cmd = word(&cursor), *sub, *name, *a, *b;
    long value, bytes;
    bool okay = false;
    if (!cmd || cmd[0] == '#')
        return 0;
    app->error[0] = '\0';
    /* Task/session commands: validate every field before calling a service. */
    if (!strcmp(cmd, "task") || !strcmp(cmd, "session")) {
        bool session = !strcmp(cmd, "session");
        sub = word(&cursor);
        name = word(&cursor);
        a = word(&cursor);
        b = word(&cursor);
        if (!sub || strcmp(sub, session ? "start" : "create") || !name ||
            !number(a, 1, MAX_TICKS, &value) || !number(b, 1, MEMORY_BUDGET, &bytes) || word(&cursor))
            goto syntax;
        okay = session ? session_start(app, name, (int)value, (size_t)bytes) :
                        task_create(app, name, (int)value, (size_t)bytes, false);
        if (okay)
            printf("Created P%d %s: READY, burst=%ld, payload=%ld bytes\n",
                         app->next_pid - 1, name, value, bytes);

    } else if (!strcmp(cmd, "block") || !strcmp(cmd, "wake") || !strcmp(cmd, "quantum")) {
        /* Commands that take exactly one numeric argument. */
        a = word(&cursor);
        if (!number(a, 1, MAX_TICKS, &value) || word(&cursor))
            goto syntax;
        if (!strcmp(cmd, "quantum")) {
            app->quantum = (int)value;
            logger_event(app, "QUANTUM", 0, a);
            okay = true;
        } else
            okay = !strcmp(cmd, "block") ? task_block(app, (int)value) : task_wake(app, (int)value);
        if (okay)
            printf("OK: %s %ld\n", cmd, value);

    } else if (!strcmp(cmd, "note")) {
        /* Note commands preserve the rest of the input as the text payload. */
        sub = word(&cursor);
        name = word(&cursor);
        if (!sub || !name)
            goto syntax;
        if (!strcmp(sub, "create") || !strcmp(sub, "append")) {
            while (*cursor == ' ' || *cursor == '\t')
                ++cursor;
            if (!*cursor)
                goto syntax;
            okay = !strcmp(sub, "create") ? note_create(app, name, cursor) : note_append(app, name, cursor);
        } else {
            if (word(&cursor))
                goto syntax;
            if (!strcmp(sub, "read"))
                okay = note_read(app, name, stdout);
            else if (!strcmp(sub, "delete"))
                okay = note_delete(app, name);
            else
                goto syntax;
        }
        if (okay)
            printf("OK: note %s %s\n", sub, name);

    } else if (!strcmp(cmd, "debug")) {
        /* Explicit teaching/test hook; never enabled by an ordinary task command. */
        sub = word(&cursor);
        if (!sub || strcmp(sub, "fail-next-allocation") || word(&cursor))
            goto syntax;
        app->memory.fail_next = true;
        logger_event(app, "DEBUG", 0, "next valid allocation will fail");
        puts("Debug hook armed: next valid allocation will fail.");
        okay = true;
    } else {
        /* Remaining commands accept no additional arguments. */
        if (word(&cursor))
            goto syntax;
        okay = true;
        if (!strcmp(cmd, "help"))
            help();
        else if (!strcmp(cmd, "quit"))
            return 2;
        else if (!strcmp(cmd, "tasks"))
            tasks_print(app, stdout);
        else if (!strcmp(cmd, "step"))
            (void)scheduler_step(app, stdout);
        else if (!strcmp(cmd, "run"))
            scheduler_run(app, stdout);
        else if (!strcmp(cmd, "metrics"))
            metrics_print(app, stdout, false);
        else if (!strcmp(cmd, "export")) {
            okay = metrics_export(app);
            if (okay)
                puts("OK: metrics.csv exported (finished tasks only).");
        } else if (!strcmp(cmd, "memory"))
            printf("Memory: used=%zu/%u bytes free=%zu allocations=%zu\n", app->memory.used,
                   MEMORY_BUDGET, MEMORY_BUDGET - app->memory.used, app->memory.count);
        else if (!strcmp(cmd, "check")) {
            okay = app_check(app);
            if (okay)
                puts("CHECK PASS: queue, task states, and allocation ownership are consistent.");
            else
                snprintf(app->error, ERROR_SIZE, "Invariant check failed.");
        } else if (!strcmp(cmd, "reset")) {
            app_reset(app);
            puts("Reset: tasks cleared, clock=0, memory=0; notes preserved; quantum retained.");
        } else
            goto syntax;
    }
    /* Service errors are logged once and return control to the prompt. */
    if (okay)
        return 0;
    logger_event(app, "ERROR", 0, app->error);
    printf("ERROR: %s\n", app->error);
    return 1;

    /* A shared error path keeps malformed input handling consistent. */
syntax:
    snprintf(app->error, ERROR_SIZE, "Invalid command or argument; enter help for syntax and limits.");
    logger_event(app, "ERROR", 0, app->error);
    printf("ERROR: %s\n", app->error);
    return 1;
}

/**
 * Read one whole input line, byte by byte, up to and excluding its newline.
 * Unlike fgets + strlen, this never stops at a NUL byte, so the complete line is
 * always consumed and none of it can be read again as a separate command.
 * Store at most size - 2 content bytes (2,046, as before) and set *too_long when
 * more arrive; set *nul when any byte is NUL. Return false at EOF before any byte
 * or on a read error, where a partial line is not executed.
 */
static bool read_line(char *line, size_t size, bool *too_long, bool *nul) {
    size_t length = 0;
    int ch;
    bool any = false;
    *too_long = *nul = false;
    while ((ch = getchar()) != EOF) {
        any = true;
        if (ch == '\n')
            break;
        if (ch == '\0')
            *nul = true;
        if (length < size - 2)
            line[length++] = (char)ch;
        else
            *too_long = true;
    }
    line[length] = '\0';
    return any && !ferror(stdin);
}

/**
 * Read commands until quit, EOF, an input failure, or an invariant failure.
 * Interactive mode prints a prompt; batch mode echoes commands for evidence.
 * Always shut down the successfully initialized app before returning.
 */
int shell_run(App *app, bool interactive) {
    char line[LINE_SIZE];
    size_t bytes, count;
    int result = 0;
    puts("MiniOS for a Study Room | C11 Phase 3");
    printf("One simulated CPU | quantum=%d | payload budget=%u bytes\n", app->quantum, MEMORY_BUDGET);
    puts("Type help for commands. Notes persist; task state starts fresh on restart.");
    while (true) {
        size_t length;
        int status;
        bool too_long, nul;
        if (interactive) {
            fputs("minios> ", stdout);
            fflush(stdout);
        }
        if (!read_line(line, sizeof line, &too_long, &nul))
            break;
        /* read_line consumed the WHOLE line, so a rejected line's tail cannot execute. */
        if (too_long) {
            puts("ERROR: command too long (maximum 2046 bytes); entire line discarded.");
            logger_event(app, "ERROR", 0, "oversized command discarded");
            continue;
        }
        if (nul) {
            puts("ERROR: command contains a NUL byte; entire line discarded.");
            logger_event(app, "ERROR", 0, "command with NUL byte discarded");
            continue;
        }
        /* No NUL is present, so strlen measures the whole stored line. */
        length = strlen(line);
        while (length && (line[length - 1] == '\n' || line[length - 1] == '\r'))
            line[--length] = '\0';
        if (!interactive && length && line[0] != '#')
            printf("minios> %s\n", line);
        /* Verify shared state after both successful and rejected commands. */
        status = execute(app, line);
        if (status == 2)
            break;
        if (!app_check(app)) {
            fputs("FATAL: internal invariant failed.\n", stderr);
            result = 1;
            break;
        }
    }
    if (ferror(stdin)) {
        fputs("ERROR: standard input read failed.\n", stderr);
        result = 1;
    }
    /* EOF follows the same cleanup path as the quit command. */
    bytes = app->memory.used;
    count = app->memory.count;
    app_shutdown(app);
    printf("Shutdown: used_bytes=%zu allocations=%zu released_allocations=%zu released_bytes=%zu\n",
           app->memory.used, app->memory.count, count - app->memory.count,
           bytes - app->memory.used);
    return result || app->logger.failures ? 1 : 0;
}
