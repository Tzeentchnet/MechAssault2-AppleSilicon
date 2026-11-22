#include "kernel.h"

#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define PARTITION_TABLE_OFFSET 0x800
#define PARTITION_COUNT 6

const char *xbox_LookupSymbolicLink(const char *link)
{
    (void)link;
    return NULL;
}

void xbox_log(int level, const char *subsystem, const char *fmt, ...)
{
    (void)level;
    (void)subsystem;
    (void)fmt;
}

static uint32_t load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void expect_translation(const char *xbox_path, const char *expected)
{
    char host_path[MAX_PATH];
    assert(xbox_translate_path(xbox_path, host_path, sizeof(host_path)));
    assert(strcmp(host_path, expected) == 0);
}

static void partition_path(char *out, size_t out_size,
                           const char *save_dir, int partition)
{
    int length = snprintf(out, out_size, "%s/Partition%d.img",
                          save_dir, partition);
    assert(length > 0 && (size_t)length < out_size);
}

static void verify_partition_table(const char *path)
{
    static const char names[][16] = {
        "XBOX_PART_X", "XBOX_PART_Y", "XBOX_PART_Z",
        "XBOX_PART_C", "XBOX_PART_E",
    };
    static const uint32_t starts[] = {
        0x00000400, 0x00177400, 0x002EE400, 0x00465400, 0x0055F400,
    };
    static const uint32_t sizes[] = {
        0x00177000, 0x00177000, 0x00177000, 0x000FA000, 0x00465400,
    };
    unsigned char table[512];
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    assert(fd >= 0);
    assert(pread(fd, table, sizeof(table), PARTITION_TABLE_OFFSET) ==
           (ssize_t)sizeof(table));
    assert(close(fd) == 0);
    assert(memcmp(table, "****PARTINFO****", 16) == 0);

    for (size_t i = 0; i < sizeof(starts) / sizeof(starts[0]); i++) {
        const unsigned char *entry = table + 48 + i * 32;
        assert(memcmp(entry, names[i], strlen(names[i])) == 0);
        assert(load_le32(entry + 16) == 0x80000000u);
        assert(load_le32(entry + 20) == starts[i]);
        assert(load_le32(entry + 24) == sizes[i]);
        assert(load_le32(entry + 28) == 0);
    }
}

static void verify_partition_volume_open(const char *xbox_path)
{
    XBOX_ANSI_STRING name = {
        (USHORT)strlen(xbox_path),
        (USHORT)(strlen(xbox_path) + 1),
        (PCHAR)xbox_path,
    };
    XBOX_OBJECT_ATTRIBUTES attributes = {0, &name, 0};
    XBOX_IO_STATUS_BLOCK io = {0};
    XBOX_FILE_FS_SIZE_INFORMATION size = {0};
    HANDLE handle = NULL;

    NTSTATUS status = xbox_NtOpenFile(
        &handle, XBOX_GENERIC_READ, &attributes, &io, 0,
        XBOX_FILE_DIRECTORY_FILE);
    assert(status == STATUS_SUCCESS);
    assert(io.Status == STATUS_SUCCESS);
    assert(handle != NULL);
    assert(xbox_NtQueryVolumeInformationFile(
               handle, &io, &size, sizeof(size), XboxFileFsSizeInformation) ==
           STATUS_SUCCESS);
    assert(size.BytesPerSector == 512);
    assert(size.SectorsPerAllocationUnit > 0);
    assert(CloseHandle(handle));
}

static void verify_reopened_directory_query(const char *game_dir)
{
    static char directory_path[] = "D:\\";
    static char file_name[] = "Common.mgf";
    static const char contents[] = "directory-query-lifecycle";
    XBOX_ANSI_STRING directory = {
        (USHORT)strlen(directory_path),
        (USHORT)(strlen(directory_path) + 1),
        directory_path,
    };
    XBOX_ANSI_STRING pattern = {
        (USHORT)strlen(file_name),
        (USHORT)(strlen(file_name) + 1),
        file_name,
    };
    XBOX_OBJECT_ATTRIBUTES attributes = {0, &directory, 0};
    char host_path[MAX_PATH];
    int fd;

    assert(snprintf(host_path, sizeof(host_path), "%s/%s",
                    game_dir, file_name) > 0);
    fd = open(host_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
    assert(fd >= 0);
    assert(write(fd, contents, sizeof(contents) - 1) ==
           (ssize_t)(sizeof(contents) - 1));
    assert(close(fd) == 0);

    for (int attempt = 0; attempt < 2; attempt++) {
        unsigned char info_buffer[512];
        PXBOX_FILE_DIRECTORY_INFORMATION info =
            (PXBOX_FILE_DIRECTORY_INFORMATION)info_buffer;
        XBOX_IO_STATUS_BLOCK io = {0};
        HANDLE handle = NULL;

        assert(xbox_NtOpenFile(
                   &handle, XBOX_GENERIC_READ, &attributes, &io, 3,
                   XBOX_FILE_DIRECTORY_FILE | XBOX_FILE_SYNCHRONOUS_IO_NONALERT) ==
               STATUS_SUCCESS);
        assert(xbox_NtQueryDirectoryFile(
                   handle, NULL, NULL, NULL, &io, info, sizeof(info_buffer),
                   XboxFileDirectoryInformation, &pattern, FALSE) ==
               STATUS_SUCCESS);
        assert(info->EndOfFile.QuadPart == (LONGLONG)(sizeof(contents) - 1));
        assert(xbox_NtClose(handle) == STATUS_SUCCESS);
    }

    assert(unlink(host_path) == 0);
}

static void verify_file_allocation(const char *game_dir)
{
    static const off_t allocation_size = 1024 * 1024;
    XBOX_FILE_END_OF_FILE_INFORMATION allocation = {0};
    XBOX_IO_STATUS_BLOCK io = {0};
    char host_path[MAX_PATH];
    struct stat status;
    HANDLE handle;
    int fd;

    assert(snprintf(host_path, sizeof(host_path), "%s/allocation.bin",
                    game_dir) > 0);
    fd = open(host_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    handle = w32_open_handle(fd, host_path);
    assert(handle != NULL);

    allocation.EndOfFile.QuadPart = allocation_size;
    assert(xbox_NtSetInformationFile(
               handle, &io, &allocation, sizeof(allocation),
               XboxFileAllocationInformation) == STATUS_SUCCESS);
    assert(io.Status == STATUS_SUCCESS);
    assert(fstat(fd, &status) == 0);
    assert(status.st_size == 0);
    assert(pwrite(fd, "x", 1, allocation_size - 1) == 1);
    assert(fstat(fd, &status) == 0);
    assert(status.st_size == allocation_size);
    assert(CloseHandle(handle));
    assert(unlink(host_path) == 0);
}

static void verify_file_rename(const char *game_dir)
{
    static char destination_name[] = "D:\\renamed.bin";
    XBOX_FILE_RENAME_INFORMATION rename_info = {
        TRUE,
        NULL,
        {
            (USHORT)(sizeof(destination_name) - 1),
            (USHORT)sizeof(destination_name),
            destination_name,
        },
    };
    XBOX_IO_STATUS_BLOCK io = {0};
    char source_path[MAX_PATH];
    char destination_path[MAX_PATH];
    HANDLE handle;
    int fd;

    assert(snprintf(source_path, sizeof(source_path), "%s/filecopy.tmp",
                    game_dir) > 0);
    assert(snprintf(destination_path, sizeof(destination_path),
                    "%s/renamed.bin", game_dir) > 0);
    fd = open(source_path, O_CREAT | O_EXCL | O_RDWR, 0600);
    assert(fd >= 0);
    assert(write(fd, "cache", 5) == 5);
    handle = w32_open_handle(fd, source_path);
    assert(handle != NULL);

    assert(xbox_NtSetInformationFile(
               handle, &io, &rename_info, sizeof(rename_info),
               XboxFileRenameInformation) == STATUS_SUCCESS);
    assert(io.Status == STATUS_SUCCESS);
    assert(access(source_path, F_OK) != 0);
    assert(access(destination_path, F_OK) == 0);
    assert(CloseHandle(handle));
    assert(unlink(destination_path) == 0);
}

int main(void)
{
    static const uint64_t sectors[PARTITION_COUNT] = {
        0, 0x00465400ull, 0x000FA000ull,
        0x00177000ull, 0x00177000ull, 0x00177000ull,
    };
    char root[] = "/tmp/ma2-partition-XXXXXX";
    char game_dir[MAX_PATH];
    char save_dir[MAX_PATH];
    char expected[MAX_PATH];
    char path[MAX_PATH];

    assert(mkdtemp(root) != NULL);
    assert(snprintf(game_dir, sizeof(game_dir), "%s/game", root) > 0);
    assert(snprintf(save_dir, sizeof(save_dir), "%s/save", root) > 0);
    assert(mkdir(game_dir, 0700) == 0);

    xbox_path_init(game_dir, save_dir);
    verify_reopened_directory_query(game_dir);
    verify_file_allocation(game_dir);
    verify_file_rename(game_dir);

    partition_path(expected, sizeof(expected), save_dir, 0);
    expect_translation("\\Device\\Harddisk0\\Partition0", expected);
    expect_translation("\\device\\harddisk0\\partition0", expected);
    expect_translation("\\Device\\Harddisk0\\Partition0\\", expected);
    expect_translation("\\Device\\Harddisk0\\Partition0/", expected);
    assert(xbox_path_is_partition_device("\\Device\\Harddisk0\\Partition0"));
    assert(xbox_path_is_partition_device("\\device\\harddisk0\\partition0\\"));
    assert(!xbox_path_is_partition_device(
        "\\Device\\Harddisk0\\Partition0\\child"));
    verify_partition_volume_open("\\device\\harddisk0\\partition0");

    partition_path(expected, sizeof(expected), save_dir, 1);
    expect_translation("\\Device\\Harddisk0\\Partition1", expected);

    assert(snprintf(expected, sizeof(expected), "%s/TitleData/slot.bin",
                    save_dir) > 0);
    expect_translation("\\Device\\Harddisk0\\Partition1\\TDATA\\slot.bin",
                       expected);

    assert(snprintf(expected, sizeof(expected), "%s/default.xip",
                    game_dir) > 0);
    expect_translation("\\Device\\Harddisk0\\Partition2\\default.xip",
                       expected);

    expect_translation("\\Device\\Harddisk0\\Partition0\\child",
                       "/Device/Harddisk0/Partition0/child");

    for (int partition = 0; partition < PARTITION_COUNT; partition++) {
        struct stat status;
        partition_path(path, sizeof(path), save_dir, partition);
        assert(lstat(path, &status) == 0);
        assert(S_ISREG(status.st_mode));
        assert(status.st_uid == geteuid());
        assert(status.st_nlink == 1);
        assert((status.st_mode & 0777) == 0600);
        if (partition > 0) {
            uint64_t size = sectors[partition] * 512ull;
            assert((uint64_t)status.st_size == size);
            assert((uint64_t)status.st_blocks * 512ull < size);
        }
    }

    partition_path(path, sizeof(path), save_dir, 0);
    verify_partition_table(path);
    assert(chmod(path, 0644) == 0);
    xbox_path_init(game_dir, save_dir);
    struct stat status;
    assert(stat(path, &status) == 0);
    assert((status.st_mode & 0777) == 0600);
    verify_partition_table(path);

    assert(unlink(path) == 0);
    for (int partition = 1; partition < PARTITION_COUNT; partition++) {
        partition_path(path, sizeof(path), save_dir, partition);
        assert(unlink(path) == 0);
    }
    assert(snprintf(path, sizeof(path), "%s/TitleData", save_dir) > 0);
    assert(rmdir(path) == 0);
    assert(rmdir(save_dir) == 0);
    assert(rmdir(game_dir) == 0);
    assert(rmdir(root) == 0);

    puts("POSIX partition device paths and images passed");
    return 0;
}
