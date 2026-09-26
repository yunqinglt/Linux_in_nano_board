#ifndef G474_CMDLINE_H
#define G474_CMDLINE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CMDLINE_MAX_ARGUMENTS 8u
#define CMDLINE_MAX_COMMAND_LENGTH 31u

typedef enum {
    CMDLINE_STATUS_OK = 0,
    CMDLINE_STATUS_EMPTY,
    CMDLINE_STATUS_UNKNOWN_COMMAND,
    CMDLINE_STATUS_INVALID_ARGUMENT,
    CMDLINE_STATUS_TOO_MANY_ARGUMENTS,
    CMDLINE_STATUS_MOUNT_FULL,
    CMDLINE_STATUS_DUPLICATE_COMMAND,
    CMDLINE_STATUS_BAD_FORMAT,
    CMDLINE_STATUS_ARGUMENT_COUNT,
    CMDLINE_STATUS_ARGUMENT_VALUE,
    CMDLINE_STATUS_ARGUMENT_RANGE,
    CMDLINE_STATUS_IO_ERROR,
} cmdline_status_t;

typedef struct {
    void *context;
    size_t argc;
    const char *const *argv;
} cmdline_argument_t;

typedef cmdline_status_t (*cmdline_entry_t)(cmdline_argument_t arguments);

/* One row in a command table, matching the user-facing name to its entry. */
typedef struct {
    const char *name;
    cmdline_entry_t entry;
} cmdline_table_t;

typedef struct {
    const cmdline_table_t *table;
    size_t count;
    void *context;
} cmdline_mount_t;

typedef struct {
    cmdline_mount_t *mounts;
    size_t mount_capacity;
    size_t mount_count;
    char last_command[CMDLINE_MAX_COMMAND_LENGTH + 1u];
} cmdline_t;

typedef cmdline_status_t (*cmdline_visit_t)(
    const cmdline_table_t *command,
    void *command_context,
    void *visitor_context);

cmdline_status_t Cmdline_Init(cmdline_t *cmdline,
                              cmdline_mount_t *mount_storage,
                              size_t mount_capacity);
/* mount_storage, table, names, entries, and context are borrowed.  They must
 * remain valid and table contents must remain unchanged while mounted. */
cmdline_status_t Cmdline_Mount(cmdline_t *cmdline,
                               const cmdline_table_t *table,
                               size_t count,
                               void *context);

/*
 * Tokenizes line in place and synchronously invokes the matching entry.
 * argv[0] is the command name and up to CMDLINE_MAX_ARGUMENTS user arguments
 * follow it.  argv strings are valid only until this function returns.
 */
cmdline_status_t Cmdline_Execute(cmdline_t *cmdline, char *line);

cmdline_status_t Cmdline_ForEach(const cmdline_t *cmdline,
                                 cmdline_visit_t visitor,
                                 void *visitor_context);
const char *Cmdline_LastCommand(const cmdline_t *cmdline);

/*
 * Parses argv[1..] using the formatter's core conversion vocabulary:
 *   %c -> char *              %s -> const char **
 *   %d -> int *               %u -> unsigned int *
 *   %x/%X -> unsigned int *
 *
 * Whitespace separates conversions, the argument count must match exactly,
 * and no allocation or copying is performed.  %x accepts an optional 0x
 * prefix; integer overflow is reported as CMDLINE_STATUS_ARGUMENT_RANGE.
 * Caller-owned outputs are left unchanged unless every argument and output
 * pointer is valid.
 */
cmdline_status_t Cmdline_ParseArguments(cmdline_argument_t arguments,
                                        const char *format,
                                        ...);

#ifdef __cplusplus
}
#endif

#endif /* G474_CMDLINE_H */
