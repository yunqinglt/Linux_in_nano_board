#include "cmdline.h"

#include <limits.h>
#include <stdarg.h>

static int cmdline_is_space(char character)
{
    return character == ' ' || character == '\t';
}

static int cmdline_text_equal(const char *left, const char *right)
{
    while (*left != '\0' && *left == *right) {
        ++left;
        ++right;
    }
    return *left == *right;
}

static int cmdline_name_is_valid(const char *name)
{
    const unsigned char *cursor = (const unsigned char *)name;
    size_t length = 0u;

    if (cursor == NULL || *cursor == '\0') {
        return 0;
    }
    while (*cursor != '\0') {
        if (*cursor <= 0x20u || *cursor > 0x7Eu) {
            return 0;
        }
        if (++length > CMDLINE_MAX_COMMAND_LENGTH) {
            return 0;
        }
        ++cursor;
    }
    return 1;
}

static void cmdline_set_last_command(cmdline_t *cmdline, const char *name)
{
    size_t index = 0u;

    while (name[index] != '\0' && index < CMDLINE_MAX_COMMAND_LENGTH) {
        cmdline->last_command[index] = name[index];
        ++index;
    }
    cmdline->last_command[index] = '\0';
}

static const cmdline_table_t *cmdline_find(const cmdline_t *cmdline,
                                            const char *name,
                                            void **context)
{
    size_t mount_index;

    for (mount_index = 0u;
         mount_index < cmdline->mount_count;
         ++mount_index) {
        const cmdline_mount_t *mount = &cmdline->mounts[mount_index];
        size_t command_index;

        for (command_index = 0u;
             command_index < mount->count;
             ++command_index) {
            const cmdline_table_t *command = &mount->table[command_index];

            if (cmdline_text_equal(command->name, name)) {
                if (context != NULL) {
                    *context = mount->context;
                }
                return command;
            }
        }
    }
    return NULL;
}

static unsigned int cmdline_hex_digit(char character)
{
    if (character >= '0' && character <= '9') {
        return (unsigned int)(character - '0');
    }
    if (character >= 'a' && character <= 'f') {
        return (unsigned int)(character - 'a') + 10u;
    }
    if (character >= 'A' && character <= 'F') {
        return (unsigned int)(character - 'A') + 10u;
    }
    return UINT_MAX;
}

static cmdline_status_t cmdline_parse_magnitude(const char *text,
                                                 unsigned int base,
                                                 unsigned int limit,
                                                 unsigned int *result)
{
    unsigned int value = 0u;
    size_t digit_count = 0u;
    int overflow = 0;

    while (*text != '\0') {
        const unsigned int digit = cmdline_hex_digit(*text++);

        if (digit == UINT_MAX || digit >= base) {
            return CMDLINE_STATUS_ARGUMENT_VALUE;
        }
        if (!overflow) {
            if (value > (limit - digit) / base) {
                overflow = 1;
            } else {
                value = (value * base) + digit;
            }
        }
        ++digit_count;
    }
    if (digit_count == 0u) {
        return CMDLINE_STATUS_ARGUMENT_VALUE;
    }
    if (overflow) {
        return CMDLINE_STATUS_ARGUMENT_RANGE;
    }
    *result = value;
    return CMDLINE_STATUS_OK;
}

static cmdline_status_t cmdline_parse_unsigned(const char *text,
                                                unsigned int base,
                                                unsigned int *result)
{
    if (*text == '+') {
        ++text;
    } else if (*text == '-') {
        return CMDLINE_STATUS_ARGUMENT_VALUE;
    }

    if (base == 16u && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X')) {
        text += 2;
    }
    return cmdline_parse_magnitude(text, base, UINT_MAX, result);
}

static cmdline_status_t cmdline_parse_signed(const char *text, int *result)
{
    const unsigned int negative_limit = 0u - (unsigned int)INT_MIN;
    unsigned int magnitude;
    unsigned int limit;
    int negative = 0;
    cmdline_status_t status;

    if (*text == '-' || *text == '+') {
        negative = (*text == '-');
        ++text;
    }
    limit = negative ? negative_limit : (unsigned int)INT_MAX;
    status = cmdline_parse_magnitude(text, 10u, limit, &magnitude);
    if (status != CMDLINE_STATUS_OK) {
        return status;
    }

    if (negative) {
        *result = (magnitude == negative_limit)
                      ? INT_MIN
                      : -(int)magnitude;
    } else {
        *result = (int)magnitude;
    }
    return CMDLINE_STATUS_OK;
}

cmdline_status_t Cmdline_Init(cmdline_t *cmdline,
                              cmdline_mount_t *mount_storage,
                              size_t mount_capacity)
{
    if (cmdline == NULL || mount_storage == NULL || mount_capacity == 0u) {
        return CMDLINE_STATUS_INVALID_ARGUMENT;
    }

    cmdline->mounts = mount_storage;
    cmdline->mount_capacity = mount_capacity;
    cmdline->mount_count = 0u;
    cmdline->last_command[0] = '\0';
    return CMDLINE_STATUS_OK;
}

cmdline_status_t Cmdline_Mount(cmdline_t *cmdline,
                               const cmdline_table_t *table,
                               size_t count,
                               void *context)
{
    size_t command_index;

    if (cmdline == NULL || cmdline->mounts == NULL || table == NULL ||
        count == 0u) {
        return CMDLINE_STATUS_INVALID_ARGUMENT;
    }
    if (cmdline->mount_count >= cmdline->mount_capacity) {
        return CMDLINE_STATUS_MOUNT_FULL;
    }

    for (command_index = 0u; command_index < count; ++command_index) {
        const cmdline_table_t *command = &table[command_index];
        size_t earlier_index;

        if (!cmdline_name_is_valid(command->name) || command->entry == NULL) {
            return CMDLINE_STATUS_INVALID_ARGUMENT;
        }
        if (cmdline_find(cmdline, command->name, NULL) != NULL) {
            return CMDLINE_STATUS_DUPLICATE_COMMAND;
        }
        for (earlier_index = 0u;
             earlier_index < command_index;
             ++earlier_index) {
            if (cmdline_text_equal(table[earlier_index].name, command->name)) {
                return CMDLINE_STATUS_DUPLICATE_COMMAND;
            }
        }
    }

    cmdline->mounts[cmdline->mount_count].table = table;
    cmdline->mounts[cmdline->mount_count].count = count;
    cmdline->mounts[cmdline->mount_count].context = context;
    ++cmdline->mount_count;
    return CMDLINE_STATUS_OK;
}

cmdline_status_t Cmdline_Execute(cmdline_t *cmdline, char *line)
{
    const char *argv[CMDLINE_MAX_ARGUMENTS + 1u];
    size_t argc = 0u;
    char *cursor;
    void *context = NULL;
    const cmdline_table_t *command;
    cmdline_argument_t arguments;

    if (cmdline == NULL || cmdline->mounts == NULL || line == NULL) {
        return CMDLINE_STATUS_INVALID_ARGUMENT;
    }
    cmdline->last_command[0] = '\0';
    cursor = line;

    while (cmdline_is_space(*cursor)) {
        ++cursor;
    }
    if (*cursor == '\0') {
        return CMDLINE_STATUS_EMPTY;
    }

    while (*cursor != '\0') {
        if (argc >= (CMDLINE_MAX_ARGUMENTS + 1u)) {
            cmdline_set_last_command(cmdline, argv[0]);
            return CMDLINE_STATUS_TOO_MANY_ARGUMENTS;
        }
        argv[argc++] = cursor;
        while (*cursor != '\0' && !cmdline_is_space(*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        *cursor++ = '\0';
        while (cmdline_is_space(*cursor)) {
            ++cursor;
        }
    }

    cmdline_set_last_command(cmdline, argv[0]);
    command = cmdline_find(cmdline, argv[0], &context);
    if (command == NULL) {
        return CMDLINE_STATUS_UNKNOWN_COMMAND;
    }

    arguments.context = context;
    arguments.argc = argc;
    arguments.argv = argv;
    return command->entry(arguments);
}

cmdline_status_t Cmdline_ForEach(const cmdline_t *cmdline,
                                 cmdline_visit_t visitor,
                                 void *visitor_context)
{
    size_t mount_index;

    if (cmdline == NULL || cmdline->mounts == NULL || visitor == NULL) {
        return CMDLINE_STATUS_INVALID_ARGUMENT;
    }

    for (mount_index = 0u;
         mount_index < cmdline->mount_count;
         ++mount_index) {
        const cmdline_mount_t *mount = &cmdline->mounts[mount_index];
        size_t command_index;

        for (command_index = 0u;
             command_index < mount->count;
             ++command_index) {
            const cmdline_status_t status = visitor(
                &mount->table[command_index],
                mount->context,
                visitor_context);

            if (status != CMDLINE_STATUS_OK) {
                return status;
            }
        }
    }
    return CMDLINE_STATUS_OK;
}

const char *Cmdline_LastCommand(const cmdline_t *cmdline)
{
    return (cmdline != NULL && cmdline->last_command[0] != '\0')
               ? cmdline->last_command
               : NULL;
}

cmdline_status_t Cmdline_ParseArguments(cmdline_argument_t arguments,
                                        const char *format,
                                        ...)
{
    typedef union {
        char character;
        const char *text;
        int signed_value;
        unsigned int unsigned_value;
    } cmdline_parsed_value_t;

    char conversions[CMDLINE_MAX_ARGUMENTS];
    cmdline_parsed_value_t parsed_values[CMDLINE_MAX_ARGUMENTS];
    void *destinations[CMDLINE_MAX_ARGUMENTS];
    const char *cursor = format;
    size_t conversion_count = 0u;
    size_t index;
    va_list values;

    if (format == NULL || arguments.argc == 0u || arguments.argv == NULL) {
        return CMDLINE_STATUS_INVALID_ARGUMENT;
    }

    /* Validate the complete format before count or value errors can mask it. */
    for (;;) {
        char conversion;

        while (cmdline_is_space(*cursor)) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        if (*cursor != '%' || cursor[1] == '\0') {
            return CMDLINE_STATUS_BAD_FORMAT;
        }
        conversion = cursor[1];
        cursor += 2;
        if (*cursor != '\0' && !cmdline_is_space(*cursor)) {
            return CMDLINE_STATUS_BAD_FORMAT;
        }
        switch (conversion) {
        case 'c':
        case 's':
        case 'd':
        case 'u':
        case 'x':
        case 'X':
            break;
        default:
            return CMDLINE_STATUS_BAD_FORMAT;
        }
        if (conversion_count < CMDLINE_MAX_ARGUMENTS) {
            conversions[conversion_count] = conversion;
        }
        ++conversion_count;
    }

    if (conversion_count > CMDLINE_MAX_ARGUMENTS ||
        conversion_count != (arguments.argc - 1u)) {
        return CMDLINE_STATUS_ARGUMENT_COUNT;
    }
    for (index = 0u; index < arguments.argc; ++index) {
        if (arguments.argv[index] == NULL) {
            return CMDLINE_STATUS_INVALID_ARGUMENT;
        }
    }

    /* Validate every token before any caller-owned output is modified. */
    for (index = 0u; index < conversion_count; ++index) {
        const char *text = arguments.argv[index + 1u];
        cmdline_status_t status;

        switch (conversions[index]) {
        case 'c':
            if (text[0] == '\0' || text[1] != '\0') {
                return CMDLINE_STATUS_ARGUMENT_VALUE;
            }
            parsed_values[index].character = text[0];
            break;
        case 's':
            parsed_values[index].text = text;
            break;
        case 'd':
            status = cmdline_parse_signed(
                text,
                &parsed_values[index].signed_value);
            if (status != CMDLINE_STATUS_OK) {
                return status;
            }
            break;
        case 'u':
            status = cmdline_parse_unsigned(
                text,
                10u,
                &parsed_values[index].unsigned_value);
            if (status != CMDLINE_STATUS_OK) {
                return status;
            }
            break;
        case 'x':
        case 'X':
            status = cmdline_parse_unsigned(
                text,
                16u,
                &parsed_values[index].unsigned_value);
            if (status != CMDLINE_STATUS_OK) {
                return status;
            }
            break;
        default:
            return CMDLINE_STATUS_BAD_FORMAT;
        }
    }

    /* Fetch all typed destinations before writing any of them. */
    va_start(values, format);
    for (index = 0u; index < conversion_count; ++index) {
        switch (conversions[index]) {
        case 'c': {
            destinations[index] = va_arg(values, char *);
            break;
        }
        case 's': {
            destinations[index] = va_arg(values, const char **);
            break;
        }
        case 'd': {
            destinations[index] = va_arg(values, int *);
            break;
        }
        case 'u': {
            destinations[index] = va_arg(values, unsigned int *);
            break;
        }
        case 'x':
        case 'X': {
            destinations[index] = va_arg(values, unsigned int *);
            break;
        }
        default:
            destinations[index] = NULL;
            break;
        }
        if (destinations[index] == NULL) {
            va_end(values);
            return CMDLINE_STATUS_INVALID_ARGUMENT;
        }
    }
    va_end(values);

    for (index = 0u; index < conversion_count; ++index) {
        switch (conversions[index]) {
        case 'c':
            *(char *)destinations[index] =
                parsed_values[index].character;
            break;
        case 's':
            *(const char **)destinations[index] =
                parsed_values[index].text;
            break;
        case 'd':
            *(int *)destinations[index] =
                parsed_values[index].signed_value;
            break;
        case 'u':
        case 'x':
        case 'X':
            *(unsigned int *)destinations[index] =
                parsed_values[index].unsigned_value;
            break;
        default:
            return CMDLINE_STATUS_BAD_FORMAT;
        }
    }
    return CMDLINE_STATUS_OK;
}
