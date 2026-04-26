#include "exfat_mount.h"
#include "mount_helpers.h"
#include "utils.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <zlib.h>

#define ZLB_MAGIC "ZLB0"
#define TMP_DIR   "/data/tmp"

static int decompress_zlb0(const char *src, char *out_path)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;

    char magic[4];
    uint32_t orig_size;

    fread(magic, 1, 4, in);
    fread(&orig_size, 1, 4, in);

    if (memcmp(magic, ZLB_MAGIC, 4) != 0) {
        fclose(in);
        return -1;
    }

    snprintf(out_path, MAX_PATH, TMP_DIR "/%08x_exfat.img", rand());

    FILE *out = fopen(out_path, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    inflateInit(&zs);

    uint8_t inbuf[1024 * 1024];
    uint8_t outbuf[1024 * 1024];

    int ret;
    do {
        zs.avail_in = fread(inbuf, 1, sizeof(inbuf), in);
        if (!zs.avail_in) break;
        zs.next_in = inbuf;

        do {
            zs.avail_out = sizeof(outbuf);
            zs.next_out = outbuf;
            ret = inflate(&zs, Z_NO_FLUSH);
            if (ret < 0) goto fail;
            fwrite(outbuf, 1, sizeof(outbuf) - zs.avail_out, out);
        } while (zs.avail_out == 0);

    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    fclose(in);
    fclose(out);
    return 0;

fail:
    inflateEnd(&zs);
    fclose(in);
    fclose(out);
    unlink(out_path);
    return -1;
}

bool mount_exfat_image(const char *file_path, char *out_mount_point)
{
    struct stat st;
    if (stat(file_path, &st) != 0) {
        notify("stat failed: %s", strerror(errno));
        return false;
    }

    char actual_path[MAX_PATH];
    strncpy(actual_path, file_path, sizeof(actual_path)-1);

    int fd = open(file_path, O_RDONLY);
    if (fd >= 0) {
        char magic[4];
        read(fd, magic, 4);
        close(fd);

        if (!memcmp(magic, ZLB_MAGIC, 4)) {
            notify("Compressed image detected — decompressing...");
            if (decompress_zlb0(file_path, actual_path) != 0) {
                notify("Decompression failed");
                return false;
            }
        }
    }

    const char *filename = strrchr(actual_path, '/') ?
                            strrchr(actual_path, '/') + 1 : actual_path;

    char mount_name[256];
    strncpy(mount_name, filename, sizeof(mount_name)-1);
    char *dot = strrchr(mount_name, '.');
    if (dot) *dot = '\0';

    snprintf(out_mount_point, MAX_PATH,
             "/data/imgmnt/exfatmnt/%s", mount_name);

    if (mkdir(out_mount_point, 0777) != 0 && errno != EEXIST) {
        notify("mkdir failed: %s", strerror(errno));
        return false;
    }

    uint64_t offset = 0;

    fd = open(actual_path, O_RDONLY);
    if (fd >= 0) {
        uint8_t sector[512];
        pread(fd, sector, 512, 0);
        if (!memcmp(sector+3, "EXFAT   ", 8)) offset = 0;
        close(fd);
    }

    int md_fd = open("/dev/mdctl", O_RDWR);
    if (md_fd >= 0) {
        struct md_ioctl mdio = {0};
        mdio.md_version = MDIOVERSION;
        mdio.md_type = MD_VNODE;
        mdio.md_file = actual_path;
        mdio.md_mediasize = st.st_size - offset;
        mdio.md_sectorsize = 512;
        mdio.md_options = MD_AUTOUNIT | MD_READONLY;

        if (ioctl(md_fd, (unsigned long)MDIOCATTACH, &mdio) == 0) {
            close(md_fd);
            char devname[64];
            snprintf(devname, sizeof(devname), "/dev/md%u", mdio.md_unit);

            struct iovec iov[] = {
                IOVEC_ENTRY("fstype"),    IOVEC_ENTRY("exfatfs"),
                IOVEC_ENTRY("from"),      IOVEC_ENTRY(devname),
                IOVEC_ENTRY("fspath"),    IOVEC_ENTRY(out_mount_point),
                IOVEC_ENTRY("large"),     IOVEC_ENTRY("yes"),
                IOVEC_ENTRY("timezone"),  IOVEC_ENTRY("static"),
                IOVEC_ENTRY("async"),     IOVEC_ENTRY(NULL),
                IOVEC_ENTRY("noatime"),   IOVEC_ENTRY(NULL),
                IOVEC_ENTRY("ignoreacl"), IOVEC_ENTRY(NULL),
            };

            if (nmount(iov, IOVEC_SIZE(iov), MNT_RDONLY) == 0)
                return true;
        }
        close(md_fd);
    }

    notify("exFAT mount failed completely");
    return false;
}

void unmount_exfat(const char *mount_point)
{
    if (!mount_point || !*mount_point) return;

    struct statfs sfs;
    if (statfs(mount_point, &sfs) == 0 &&
        strcmp(sfs.f_fstypename, "exfatfs") == 0) {
        unmount(mount_point, MNT_FORCE);
    }

    rmdir(mount_point);
}