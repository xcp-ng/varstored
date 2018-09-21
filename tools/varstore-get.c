/*
 * Copyright (C) Citrix Systems, Inc
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <backend.h>
#include <debug.h>
#include <serialize.h>

#include "tool-lib.h"

struct backend *db = &xapidb_cmdline;
enum log_level log_level = LOG_LVL_INFO;

static void
usage(const char *progname)
{
    printf("usage: %s [-h] [-a] <vm-uuid> <guid> <name>\n", progname);
}

#define print_attr(x) do { \
    if (attr & x) \
        printf(#x "\n"); \
    } while (0)

static bool
do_get(const char *guid_str, const char *name, bool show_attr)
{
    uint8_t buf[SHMEM_SIZE];
    uint8_t *ptr;
    uint8_t variable_name[NAME_LIMIT];
    EFI_GUID guid;
    EFI_STATUS status;
    UINT32 attr;
    size_t name_size;

    name_size = parse_name(name, variable_name);

    if (!parse_guid(&guid, guid_str)) {
        ERR("Failed to parse GUID\n");
        return false;
    }

    ptr = buf;
    serialize_uint32(&ptr, 1); /* version */
    serialize_uint32(&ptr, COMMAND_GET_VARIABLE);
    serialize_data(&ptr, variable_name, name_size);
    serialize_guid(&ptr, &guid);
    serialize_uintn(&ptr, DATA_LIMIT);
    *ptr = 0;

    dispatch_command(buf);

    ptr = buf;
    status = unserialize_uintn(&ptr);
    if (status != EFI_SUCCESS) {
        print_efi_error(status);
        return false;
    }

    attr = unserialize_uint32(&ptr);

    if (show_attr) {
        printf("Attributes = 0x%08x (%u)\n", attr, attr);

        print_attr(EFI_VARIABLE_NON_VOLATILE);
        print_attr(EFI_VARIABLE_BOOTSERVICE_ACCESS);
        print_attr(EFI_VARIABLE_RUNTIME_ACCESS);
        print_attr(EFI_VARIABLE_HARDWARE_ERROR_RECORD);
        print_attr(EFI_VARIABLE_TIME_BASED_AUTHENTICATED_WRITE_ACCESS);
        print_attr(EFI_VARIABLE_APPEND_WRITE);
        print_attr(EFI_VARIABLE_ENHANCED_AUTHENTICATED_ACCESS);
        print_attr(EFI_VARIABLE_AUTHENTICATED_WRITE_ACCESS);
    } else {
        uint8_t *data;
        UINTN data_len;

        data = unserialize_data(&ptr, &data_len, DATA_LIMIT);
        if (fwrite(data, 1, data_len, stdout) != data_len) {
            ERR("Failed to write out data\n");
            free(data);
            return false;
        }
        free(data);
    }

    return true;
}

int main(int argc, char **argv)
{
    bool show_attr = false;

    for (;;) {
        int c = getopt(argc, argv, "ah");

        if (c == -1)
            break;

        switch (c) {
        case 'a':
            show_attr = true;
            break;
        case 'h':
            usage(argv[0]);
            exit(0);
        default:
            usage(argv[0]);
            exit(1);
        }
    }

    if (argc - optind != 3) {
        usage(argv[0]);
        exit(1);
    }

    db->parse_arg("uuid", argv[optind]);

    if (!tool_init())
        exit(1);

    return !do_get(argv[optind + 1], argv[optind + 2], show_attr);
}
