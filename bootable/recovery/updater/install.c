/*
 * Copyright (C) 2009 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <selinux/selinux.h>
#include <ftw.h>
#include <sys/capability.h>
#include <sys/xattr.h>
#include <linux/xattr.h>
#include <inttypes.h>

#include "bootloader.h"
#include "applypatch/applypatch.h"
#include "cutils/android_reboot.h"
#include "cutils/misc.h"
#include "cutils/properties.h"
#include "edify/expr.h"
#include "mincrypt/sha.h"
#include "minzip/DirUtil.h"
#include "mtdutils/mounts.h"
#include "mtdutils/mtdutils.h"
#include "updater.h"
#include "install.h"
#include "fs_mgr.h"
#include "common.h"

/* Need tune2fs to enable */
/* #include "tune2fs.h" */

#ifdef USE_EXT4
#include "make_ext4fs.h"
#include "wipe.h"
#endif

static int num_volumes = 0;
static Volume* device_volumes = NULL;

void uiPrint(State* state, char* buffer) {
    char* line = strtok(buffer, "\n");
    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    while (line) {
        fprintf(ui->cmd_pipe, "ui_print %s\n", line);
        line = strtok(NULL, "\n");
    }
    fprintf(ui->cmd_pipe, "ui_print\n");
}

__attribute__((__format__(printf, 2, 3))) __nonnull((2))
void uiPrintf(State* state, const char* format, ...) {
    char error_msg[1024];
    va_list ap;
    va_start(ap, format);
    vsnprintf(error_msg, sizeof(error_msg), format, ap);
    va_end(ap);
    uiPrint(state, error_msg);
}

// Take a sha-1 digest and return it as a newly-allocated hex string.
char* PrintSha1(const uint8_t* digest) {
    char* buffer = malloc(SHA_DIGEST_SIZE*2 + 1);
    int i;
    const char* alphabet = "0123456789abcdef";
    for (i = 0; i < SHA_DIGEST_SIZE; ++i) {
        buffer[i*2] = alphabet[(digest[i] >> 4) & 0xf];
        buffer[i*2+1] = alphabet[digest[i] & 0xf];
    }
    buffer[i*2] = '\0';
    return buffer;
}

int parse_fstab(FILE *logfd, char *name, int *alloc) {
    FILE* fstab;

    fstab = fopen(name, "r");
    if(!fstab){
        fprintf(logfd, "ui_print %s not found\n", name);
        return -1;
    }

    char buffer[1024];
    int i;
    while (fgets(buffer, sizeof(buffer)-1, fstab)) {
        for (i = 0; buffer[i] && isspace(buffer[i]); ++i);
        if (buffer[i] == '\0' || buffer[i] == '#') continue;

        char* original = strdup(buffer);

        char* device = strtok(buffer+i, " \t\n");
        char* mount_point = strtok(NULL, " \t\n");
        char* fs_type = strtok(NULL, " \t\n");

        if (mount_point && fs_type && device) {
            while (num_volumes >= *alloc) {
                *alloc *= 2;
                device_volumes = realloc(device_volumes, (*alloc)*sizeof(Volume));
            }
            device_volumes[num_volumes].mount_point = strdup(mount_point);
            device_volumes[num_volumes].fs_type = strdup(fs_type);
            device_volumes[num_volumes].blk_device = strdup(device);
            device_volumes[num_volumes].length = 0;
            ++num_volumes;
        } else {
            fprintf(logfd, "ui_print skipping malformed fstab (%s) line: %s\n", name, original);
        }
        free(original);
    }

    fclose(fstab);
    return 0;
}

void load_volume_table(FILE *logfd) {
    int alloc = 2;
    int i;

    if (device_volumes)
        return;

    device_volumes = malloc(alloc * sizeof(Volume));

    // Insert an entry for /tmp, which is the ramdisk and is always mounted.
    if (asprintf(&device_volumes[0].mount_point, "/tmp") == -1)
        device_volumes[0].mount_point = NULL;
    if (asprintf(&device_volumes[0].fs_type, "ramdisk") == -1)
        device_volumes[0].fs_type = NULL;
    device_volumes[0].blk_device = NULL;
    //device_volumes[0].device2 = NULL;
    device_volumes[0].length = 0;
    num_volumes = 1;

    if (parse_fstab(logfd, "/res/recovery_volume_config", &alloc) < 0) {
        fprintf(logfd, "ui_print /res/recovery_volume_config not found\n");
    }
    if (parse_fstab(logfd, "/res/recovery_volume_detected", &alloc) < 0) {
        fprintf(logfd, "ui_print /res/recovery_volume_detected not found\n");
    }
}

void free_volume_table() {
    int i;

    if (!device_volumes)
        return;

    for (i = 0; i < num_volumes; ++i) {
        Volume* v = device_volumes + i;
        if (v->mount_point)
            free(v->mount_point);
        if (v->fs_type)
            free(v->fs_type);
        if (v->blk_device)
            free(v->blk_device);
        //if (v->device2)
        //    free(v->device2);
    }
    free(device_volumes);
    device_volumes = 0;
}

Volume* volume_for_path(const char* path) {
    int i;
    for (i = 0; i < num_volumes; ++i) {
        Volume* v = device_volumes+i;
        int len = strlen(v->mount_point);
        if (strncmp(path, v->mount_point, len) == 0 &&
            (path[len] == '\0' || path[len] == '/')) {
            return v;
        }
    }
    return NULL;
}

// Execute command
int exec_command(FILE *logfd, const char *name, char *const args[]) {
    int status = -1;
    int i;
    pid_t pid;

    pid = fork();
    if (pid == -1) {
        fprintf(logfd, "ui_print exec failed at fork \'%s\'\n", name);
        goto cleanup;
    } else if (pid == 0) {
        fprintf(logfd, "ui_print executing \'%s\'", name);
        for (i = 0; i < 10; i++) {  // limit logging to reduce verbage
            if (args[i]) {
                fprintf(logfd, "ui_print %s", args[i]);
            } else {
                break;
            }
        }
        fprintf(logfd, "ui_print \n", name);
        execvp(name, args);
        fprintf(logfd, "ui_print exec failed \'%s\'\n", name);
        _exit(-1);
    } else {
        waitpid(pid, &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(logfd, "ui_print exec returned error %d \'%s\'\n",
                    WEXITSTATUS(status), name);
        }
    }

cleanup:
    return status;
}

// mount(fs_type, partition_type, location, mount_point)
//
//    fs_type="yaffs2" partition_type="MTD"     location=partition
//    fs_type="ubifs"  partition_type="UBI"     location=volume_alias
//    fs_type="ext4"   partition_type="EMMC"    location=device
Value* MountFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 4 && argc != 5) {
        return ErrorAbort(state, "%s() expects 4-5 args, got %d", name, argc);
    }
    char* fs_type;
    char* partition_type;
    char* location;
    char* mount_dir;
    char* mount_options;
    bool has_mount_options;
    char *newcontext = NULL;
    char *oldcontext = NULL;
    if (argc == 5) {
        has_mount_options = true;
        if (ReadArgs(state, argv, 5, &fs_type, &partition_type,
                 &location, &mount_dir, &mount_options) < 0) {
            return NULL;
        }
    } else {
        has_mount_options = false;
        if (ReadArgs(state, argv, 4, &fs_type, &partition_type,
                 &location, &mount_dir) < 0) {
            return NULL;
        }
    }

    if (strlen(fs_type) == 0) {
        ErrorAbort(state, "fs_type argument to %s() can't be empty", name);
        goto done;
    }
    if (strlen(partition_type) == 0) {
        ErrorAbort(state, "partition_type argument to %s() can't be empty",
                   name);
        goto done;
    }
    if (strlen(location) == 0) {
        ErrorAbort(state, "location argument to %s() can't be empty", name);
        goto done;
    }
    if (strlen(mount_dir) == 0) {
        ErrorAbort(state, "mount_dir argument to %s() can't be empty", name);
        goto done;
    }

    //stick a slash on the end to mount point
    size_t pathLen = strlen(mount_dir);
    char *mount_point = (char *)malloc(pathLen + 2);
    if (mount_point == NULL) {
        ErrorAbort(state, "Can't malloc mount_point to %s()", name);
        goto done;
    }
    memcpy(mount_point, mount_dir, pathLen);
    if(mount_point[pathLen - 1] != '/')
        mount_point[pathLen] = '/';
    else
        mount_point[pathLen] = '\0';
    mount_point[pathLen + 1] = '\0';

    char *secontext = NULL;

    if (sehandle) {
        selabel_lookup(sehandle, &secontext, mount_point, 0755);
        setfscreatecon(secontext);
    }

    mkdir(mount_point, 0755);

    if (secontext) {
        freecon(secontext);
        setfscreatecon(NULL);
    }

    if (strcmp(partition_type, "MTD") == 0) {
        mtd_scan_partitions();
        const MtdPartition* mtd;
        mtd = mtd_find_partition_by_name(location);
        if (mtd == NULL) {
            uiPrintf(state, "%s: no mtd partition named \"%s\"",
                    name, location);
            result = strdup("");
            goto done;
        }
        if (mtd_mount_partition(mtd, mount_point, fs_type, 0 /* rw */) != 0) {
            uiPrintf(state, "mtd mount of %s failed: %s\n",
                    location, strerror(errno));
            result = strdup("");
            goto done;
        }
        result = mount_point;
    } else if ((strcmp(fs_type, "ubifs") == 0)
               || (strcmp(fs_type, "ext4") == 0)) {
        Volume *v = 0;;
        if (strcmp(location, "userdata") == 0) {
            v = volume_for_path("/data");
        } else if (strcmp(location, "system") == 0) {
            v = volume_for_path("/system");
        }
        if (!v) {
            fprintf(stderr, "%s: failed to locate %s \"%s\"",
                    name, fs_type, location);
            result = strdup("");
            goto done;
        }
        if (mount(v->blk_device, mount_point, fs_type,
                  MS_NOATIME | MS_NODEV | MS_NODIRATIME, "") < 0) {
            fprintf(stderr, "%s: failed to mount %s at %s: %s\n",
                    name, location, mount_point, strerror(errno));
            result = strdup("");
        } else {
            result = mount_point;
        }
    } else {
        if (mount(location, mount_point, fs_type,
                  MS_NOATIME | MS_NODEV | MS_NODIRATIME,
                  has_mount_options ? mount_options : "") < 0) {
            uiPrintf(state, "%s: failed to mount %s at %s: %s\n",
                    name, location, mount_point, strerror(errno));
            result = strdup("");
        } else {
            result = mount_point;
        }
    }

    // After mounted, should check mouint point's selinux context. If it
    // isn't correct, modify it.
    if (sehandle) {
        if (getfilecon(mount_point, &oldcontext) < 0)
            goto done;
        if (selabel_lookup(sehandle, &newcontext, mount_point, 0755) < 0)
            goto done;
        if (strcmp(newcontext, "<<none>>") == 0)
            goto done;
        if (strcmp(oldcontext, newcontext))
            setfilecon(mount_point, newcontext);
    }

done:
    free(fs_type);
    free(partition_type);
    free(location);
    free(mount_dir);
    if (result != mount_point) free(mount_point);
    if (has_mount_options) free(mount_options);
    if (newcontext) freecon(newcontext);
    if (oldcontext) freecon(oldcontext);
    return StringValue(result);
}


// is_mounted(mount_point)
Value* IsMountedFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* mount_point;
    if (ReadArgs(state, argv, 1, &mount_point) < 0) {
        return NULL;
    }
    if (strlen(mount_point) == 0) {
        ErrorAbort(state, "mount_point argument to unmount() can't be empty");
        goto done;
    }

    scan_mounted_volumes();
    const MountedVolume* vol = find_mounted_volume_by_mount_point(mount_point);
    if (vol == NULL) {
        result = strdup("");
    } else {
        result = mount_point;
    }

done:
    if (result != mount_point) free(mount_point);
    return StringValue(result);
}


Value* UnmountFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* mount_point;
    if (ReadArgs(state, argv, 1, &mount_point) < 0) {
        return NULL;
    }
    if (strlen(mount_point) == 0) {
        ErrorAbort(state, "mount_point argument to unmount() can't be empty");
        goto done;
    }

    scan_mounted_volumes();
    const MountedVolume* vol = find_mounted_volume_by_mount_point(mount_point);
    if (vol == NULL) {
        uiPrintf(state, "unmount of %s failed; no such volume\n", mount_point);
        result = strdup("");
    } else {
        int ret = unmount_mounted_volume(vol);
        if (ret != 0) {
           uiPrintf(state, "unmount of %s failed (%d): %s\n",
                    mount_point, ret, strerror(errno));
        }
        result = mount_point;
    }

done:
    if (result != mount_point) free(mount_point);
    return StringValue(result);
}

static int exec_cmd(const char* path, char* const argv[]) {
    int status;
    pid_t child;
    if ((child = vfork()) == 0) {
        execv(path, argv);
        _exit(-1);
    }
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("%s failed with status %d\n", path, WEXITSTATUS(status));
    }
    return WEXITSTATUS(status);
}


// format(fs_type, partition_type, location, fs_size, mount_point)
//
//    fs_type="yaffs2" partition_type="MTD"     location=partition fs_size=<bytes> mount_point=<location>
//    fs_type="ubifs"  partition_type="UBI"     location=path      fs_size=(ignored)
//    fs_type="ext4"   partition_type="EMMC"    location=device    fs_size=<bytes> mount_point=<location>
//    fs_type="f2fs"   partition_type="EMMC"    location=device    fs_size=<bytes> mount_point=<location>
//    if fs_size == 0, then make fs uses the entire partition.
//    if fs_size > 0, that is the size to use
//    if fs_size < 0, then reserve that many bytes at the end of the partition (not for "f2fs")
Value* FormatFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 5) {
        return ErrorAbort(state, "%s() expects 5 args, got %d", name, argc);
    }
    char* fs_type;
    char* partition_type;
    char* location;
    char* fs_size;
    char* mount_point;

    if (ReadArgs(state, argv, 5, &fs_type, &partition_type, &location, &fs_size, &mount_point) < 0) {
        return NULL;
    }

    if (strlen(fs_type) == 0) {
        ErrorAbort(state, "fs_type argument to %s() can't be empty", name);
        goto done;
    }
    if (strlen(partition_type) == 0) {
        ErrorAbort(state, "partition_type argument to %s() can't be empty",
                   name);
        goto done;
    }
    if (strlen(location) == 0) {
        ErrorAbort(state, "location argument to %s() can't be empty", name);
        goto done;
    }

    if (strlen(mount_point) == 0) {
        ErrorAbort(state, "mount_point argument to %s() can't be empty", name);
        goto done;
    }

    if (strcmp(partition_type, "MTD") == 0) {
        mtd_scan_partitions();
        const MtdPartition* mtd = mtd_find_partition_by_name(location);
        if (mtd == NULL) {
            printf("%s: no mtd partition named \"%s\"",
                    name, location);
            result = strdup("");
            goto done;
        }
        MtdWriteContext* ctx = mtd_write_partition(mtd);
        if (ctx == NULL) {
            printf("%s: can't write \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        if (mtd_erase_blocks(ctx, -1) == -1) {
            mtd_write_close(ctx);
            printf("%s: failed to erase \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        if (mtd_write_close(ctx) != 0) {
            printf("%s: failed to close \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        result = location;
    } else if (strcmp(fs_type, "ubifs") == 0) {
        Volume *v = 0;;
        UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
        if (strcmp(location, "userdata") == 0) {
            v = volume_for_path("/data");
        } else if (strcmp(location, "system") == 0) {
            v = volume_for_path("/system");
        }
        if (!v) {
            fprintf(stderr, "%s: failed to locate ubifs volume \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        char *argv[] = {"mkfs.ubifs", "-y", v->blk_device, 0};
        if (exec_command(ui->cmd_pipe, "/usr/sbin/mkfs.ubifs", argv) != 0) {
            fprintf(stderr, "%s: failed to create ubifs volume \"%s\"", name, location);
            fprintf(ui->cmd_pipe, "ui_print %s: failed to format ubifs volume \"%s\"\n", name, location);
            result = strdup("");
            goto done;
        }
        result = location;
#ifdef USE_EXT4  /* Use linked in ext4fs generation tool */
    } else if (strcmp(fs_type, "ext4") == 0) {
        int status = make_ext4fs(location, atoll(fs_size), mount_point, sehandle);
        if (status != 0) {
            printf("%s: make_ext4fs failed (%d) on %s",
                    name, status, location);
            result = strdup("");
            goto done;
        }
        result = location;
    } else if (strcmp(fs_type, "f2fs") == 0) {
        char *num_sectors;
        if (asprintf(&num_sectors, "%lld", atoll(fs_size) / 512) <= 0) {
            printf("format_volume: failed to create %s command for %s\n", fs_type, location);
            result = strdup("");
            goto done;
        }
        const char *f2fs_path = "/sbin/mkfs.f2fs";
        const char* const f2fs_argv[] = {"mkfs.f2fs", "-t", "-d1", location, num_sectors, NULL};
        int status = exec_cmd(f2fs_path, (char* const*)f2fs_argv);
        free(num_sectors);
        if (status != 0) {
            printf("%s: mkfs.f2fs failed (%d) on %s",
                    name, status, location);
            result = strdup("");
            goto done;
        }
        result = location;
#else  /* Use e2fsprogs modules */
    } else if (strcmp(fs_type, "ext4") == 0) {
        Volume *v = 0;;
        UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
        if (strcmp(location, "userdata") == 0) {
            v = volume_for_path("/data");
        } else if (strcmp(location, "system") == 0) {
            v = volume_for_path("/system");
        }
        if (!v) {
            fprintf(stderr, "%s: failed to locate ext4 filesystem \"%s\"", name, location);
            result = strdup("");
            goto done;
        }
        char *argv[] = {"mkfs.ext4", "-b", "4096", "-O", "extent,uninit_bg,dir_index,has_journal,sparse_super", v->blk_device, 0};
        if (exec_command(ui->cmd_pipe, "/sbin/mkfs.ext4", argv) != 0) {
            fprintf(stderr, "%s: failed to create ext4 filesystem \"%s\"", name, location);
            fprintf(ui->cmd_pipe, "ui_print %s: failed to format ubifs volume \"%s\"\n", name, location);
            result = strdup("");
            goto done;
        }
        result = location;
#endif
    } else {
        printf("%s: unsupported fs_type \"%s\" partition_type \"%s\"",
                name, fs_type, partition_type);
    }

done:
    free(fs_type);
    free(partition_type);
    if (result != location) free(location);
    return StringValue(result);
}

Value* RenameFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }

    char* src_name;
    char* dst_name;

    if (ReadArgs(state, argv, 2, &src_name, &dst_name) < 0) {
        return NULL;
    }
    if (strlen(src_name) == 0) {
        ErrorAbort(state, "src_name argument to %s() can't be empty", name);
        goto done;
    }
    if (strlen(dst_name) == 0) {
        ErrorAbort(state, "dst_name argument to %s() can't be empty", name);
        goto done;
    }
    if (make_parents(dst_name) != 0) {
        ErrorAbort(state, "Creating parent of %s failed, error %s",
          dst_name, strerror(errno));
    } else if (access(dst_name, F_OK) == 0 && access(src_name, F_OK) != 0) {
        // File was already moved
        result = dst_name;
    } else if (rename(src_name, dst_name) != 0) {
        ErrorAbort(state, "Rename of %s to %s failed, error %s",
          src_name, dst_name, strerror(errno));
    } else {
        result = dst_name;
    }

done:
    free(src_name);
    if (result != dst_name) free(dst_name);
    return StringValue(result);
}

Value* DeleteFn(const char* name, State* state, int argc, Expr* argv[]) {
    char** paths = malloc(argc * sizeof(char*));
    int i;
    for (i = 0; i < argc; ++i) {
        paths[i] = Evaluate(state, argv[i]);
        if (paths[i] == NULL) {
            int j;
            for (j = 0; j < i; ++i) {
                free(paths[j]);
            }
            free(paths);
            return NULL;
        }
    }

    bool recursive = (strcmp(name, "delete_recursive") == 0);

    int success = 0;
    for (i = 0; i < argc; ++i) {
        if ((recursive ? dirUnlinkHierarchy(paths[i]) : unlink(paths[i])) == 0)
            ++success;
        free(paths[i]);
    }
    free(paths);

    char buffer[10];
    sprintf(buffer, "%d", success);
    return StringValue(strdup(buffer));
}


Value* ShowProgressFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }
    char* frac_str;
    char* sec_str;
    if (ReadArgs(state, argv, 2, &frac_str, &sec_str) < 0) {
        return NULL;
    }

    double frac = strtod(frac_str, NULL);
    int sec = strtol(sec_str, NULL, 10);

    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    fprintf(ui->cmd_pipe, "progress %f %d\n", frac, sec);

    free(sec_str);
    return StringValue(frac_str);
}

Value* SetProgressFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* frac_str;
    if (ReadArgs(state, argv, 1, &frac_str) < 0) {
        return NULL;
    }

    double frac = strtod(frac_str, NULL);

    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    fprintf(ui->cmd_pipe, "set_progress %f\n", frac);

    return StringValue(frac_str);
}

// package_extract_dir(package_path, destination_path)
Value* PackageExtractDirFn(const char* name, State* state,
                          int argc, Expr* argv[]) {
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }
    char* zip_path;
    char* dest_path;
    if (ReadArgs(state, argv, 2, &zip_path, &dest_path) < 0) return NULL;

    ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;

    // To create a consistent system image, never use the clock for timestamps.
    struct utimbuf timestamp = { 1217592000, 1217592000 };  // 8/1/2008 default

    bool success = mzExtractRecursive(za, zip_path, dest_path,
                                      &timestamp,
                                      NULL, NULL, sehandle);
    free(zip_path);
    free(dest_path);
    return StringValue(strdup(success ? "t" : ""));
}


// package_extract_file(package_path, destination_path)
//   or
// package_extract_file(package_path)
//   to return the entire contents of the file as the result of this
//   function (the char* returned is actually a FileContents*).
Value* PackageExtractFileFn(const char* name, State* state,
                           int argc, Expr* argv[]) {
    if (argc < 1 || argc > 2) {
        return ErrorAbort(state, "%s() expects 1 or 2 args, got %d",
                          name, argc);
    }
    bool success = false;

    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);

    if (argc == 2) {
        // The two-argument version extracts to a file.

        ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;

        char* zip_path;
        char* dest_path;
        if (ReadArgs(state, argv, 2, &zip_path, &dest_path) < 0) return NULL;

        const ZipEntry* entry = mzFindZipEntry(za, zip_path);
        if (entry == NULL) {
            printf("%s: no %s in package\n", name, zip_path);
            goto done2;
        }

        FILE* f = fopen(dest_path, "wb");
        if (f == NULL) {
            printf("%s: can't open %s for write: %s\n",
                    name, dest_path, strerror(errno));
            goto done2;
        }
        success = mzExtractZipEntryToFile(za, entry, fileno(f));
        fclose(f);

        printf("%s %s to %s\n",
               success ? "extracted" : "failed to extract", zip_path, dest_path);

      done2:
        free(zip_path);
        free(dest_path);
        return StringValue(strdup(success ? "t" : ""));
    } else {
        // The one-argument version returns the contents of the file
        // as the result.

        char* zip_path;
        Value* v = malloc(sizeof(Value));
        v->type = VAL_BLOB;
        v->size = -1;
        v->data = NULL;

        if (ReadArgs(state, argv, 1, &zip_path) < 0) return NULL;

        ZipArchive* za = ((UpdaterInfo*)(state->cookie))->package_zip;
        const ZipEntry* entry = mzFindZipEntry(za, zip_path);
        if (entry == NULL) {
            printf("%s: no %s in package\n", name, zip_path);
            goto done1;
        }

        v->size = mzGetZipEntryUncompLen(entry);
        v->data = malloc(v->size);
        if (v->data == NULL) {
            printf("%s: failed to allocate %ld bytes for %s\n",
                    name, (long)v->size, zip_path);
            goto done1;
        }

        success = mzExtractZipEntryToBuffer(za, entry,
                                            (unsigned char *)v->data);

      done1:
        free(zip_path);
        if (!success) {
            free(v->data);
            v->data = NULL;
            v->size = -1;
        }
        return v;
    }
}

// Create all parent directories of name, if necessary.
static int make_parents(char* name) {
    char* p;
    for (p = name + (strlen(name)-1); p > name; --p) {
        if (*p != '/') continue;
        *p = '\0';
        if (make_parents(name) < 0) return -1;
        int result = mkdir(name, 0700);
        if (result == 0) printf("created [%s]\n", name);
        *p = '/';
        if (result == 0 || errno == EEXIST) {
            // successfully created or already existed; we're done
            return 0;
        } else {
            printf("failed to mkdir %s: %s\n", name, strerror(errno));
            return -1;
        }
    }
    return 0;
}

// symlink target src1 src2 ...
//    unlinks any previously existing src1, src2, etc before creating symlinks.
Value* SymlinkFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc == 0) {
        return ErrorAbort(state, "%s() expects 1+ args, got %d", name, argc);
    }
    char* target;
    target = Evaluate(state, argv[0]);
    if (target == NULL) return NULL;

    char** srcs = ReadVarArgs(state, argc-1, argv+1);
    if (srcs == NULL) {
        free(target);
        return NULL;
    }

    int bad = 0;
    int i;
    for (i = 0; i < argc-1; ++i) {
        if (unlink(srcs[i]) < 0) {
            if (errno != ENOENT) {
                printf("%s: failed to remove %s: %s\n",
                        name, srcs[i], strerror(errno));
                ++bad;
            }
        }
        if (make_parents(srcs[i])) {
            printf("%s: failed to symlink %s to %s: making parents failed\n",
                    name, srcs[i], target);
            ++bad;
        }
        if (symlink(target, srcs[i]) < 0) {
            printf("%s: failed to symlink %s to %s: %s\n",
                    name, srcs[i], target, strerror(errno));
            ++bad;
        }
        free(srcs[i]);
    }
    free(srcs);
    if (bad) {
        return ErrorAbort(state, "%s: some symlinks failed", name);
    }
    return StringValue(strdup(""));
}


Value* SetPermFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    bool recursive = (strcmp(name, "set_perm_recursive") == 0);

    int min_args = 4 + (recursive ? 1 : 0);
    if (argc < min_args) {
        return ErrorAbort(state, "%s() expects %d+ args, got %d",
                          name, min_args, argc);
    }

    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) return NULL;

    char* end;
    int i;
    int bad = 0;

    int uid = strtoul(args[0], &end, 0);
    if (*end != '\0' || args[0][0] == 0) {
        ErrorAbort(state, "%s: \"%s\" not a valid uid", name, args[0]);
        goto done;
    }

    int gid = strtoul(args[1], &end, 0);
    if (*end != '\0' || args[1][0] == 0) {
        ErrorAbort(state, "%s: \"%s\" not a valid gid", name, args[1]);
        goto done;
    }

    if (recursive) {
        int dir_mode = strtoul(args[2], &end, 0);
        if (*end != '\0' || args[2][0] == 0) {
            ErrorAbort(state, "%s: \"%s\" not a valid dirmode", name, args[2]);
            goto done;
        }

        int file_mode = strtoul(args[3], &end, 0);
        if (*end != '\0' || args[3][0] == 0) {
            ErrorAbort(state, "%s: \"%s\" not a valid filemode",
                       name, args[3]);
            goto done;
        }

        for (i = 4; i < argc; ++i) {
            dirSetHierarchyPermissions(args[i], uid, gid, dir_mode, file_mode);
        }
    } else {
        int mode = strtoul(args[2], &end, 0);
        if (*end != '\0' || args[2][0] == 0) {
            ErrorAbort(state, "%s: \"%s\" not a valid mode", name, args[2]);
            goto done;
        }

        for (i = 3; i < argc; ++i) {
            if (chown(args[i], uid, gid) < 0) {
                printf("%s: chown of %s to %d %d failed: %s\n",
                        name, args[i], uid, gid, strerror(errno));
                ++bad;
            }
            if (chmod(args[i], mode) < 0) {
                printf("%s: chmod of %s to %o failed: %s\n",
                        name, args[i], mode, strerror(errno));
                ++bad;
            }
        }
    }
    result = strdup("");

done:
    for (i = 0; i < argc; ++i) {
        free(args[i]);
    }
    free(args);

    if (bad) {
        free(result);
        return ErrorAbort(state, "%s: some changes failed", name);
    }
    return StringValue(result);
}

struct perm_parsed_args {
    bool has_uid;
    uid_t uid;
    bool has_gid;
    gid_t gid;
    bool has_mode;
    mode_t mode;
    bool has_fmode;
    mode_t fmode;
    bool has_dmode;
    mode_t dmode;
    bool has_selabel;
    char* selabel;
    bool has_capabilities;
    uint64_t capabilities;
};

static struct perm_parsed_args ParsePermArgs(State * state, int argc, char** args) {
    int i;
    struct perm_parsed_args parsed;
    int bad = 0;
    static int max_warnings = 20;

    memset(&parsed, 0, sizeof(parsed));

    for (i = 1; i < argc; i += 2) {
        if (strcmp("uid", args[i]) == 0) {
            int64_t uid;
            if (sscanf(args[i+1], "%" SCNd64, &uid) == 1) {
                parsed.uid = uid;
                parsed.has_uid = true;
            } else {
                uiPrintf(state, "ParsePermArgs: invalid UID \"%s\"\n", args[i + 1]);
                bad++;
            }
            continue;
        }
        if (strcmp("gid", args[i]) == 0) {
            int64_t gid;
            if (sscanf(args[i+1], "%" SCNd64, &gid) == 1) {
                parsed.gid = gid;
                parsed.has_gid = true;
            } else {
                uiPrintf(state, "ParsePermArgs: invalid GID \"%s\"\n", args[i + 1]);
                bad++;
            }
            continue;
        }
        if (strcmp("mode", args[i]) == 0) {
            int32_t mode;
            if (sscanf(args[i+1], "%" SCNi32, &mode) == 1) {
                parsed.mode = mode;
                parsed.has_mode = true;
            } else {
                uiPrintf(state, "ParsePermArgs: invalid mode \"%s\"\n", args[i + 1]);
                bad++;
            }
            continue;
        }
        if (strcmp("dmode", args[i]) == 0) {
            int32_t mode;
            if (sscanf(args[i+1], "%" SCNi32, &mode) == 1) {
                parsed.dmode = mode;
                parsed.has_dmode = true;
            } else {
                uiPrintf(state, "ParsePermArgs: invalid dmode \"%s\"\n", args[i + 1]);
                bad++;
            }
            continue;
        }
        if (strcmp("fmode", args[i]) == 0) {
            int32_t mode;
            if (sscanf(args[i+1], "%" SCNi32, &mode) == 1) {
                parsed.fmode = mode;
                parsed.has_fmode = true;
            } else {
                uiPrintf(state, "ParsePermArgs: invalid fmode \"%s\"\n", args[i + 1]);
                bad++;
            }
            continue;
        }
        if (strcmp("capabilities", args[i]) == 0) {
            int64_t capabilities;
            if (sscanf(args[i+1], "%" SCNi64, &capabilities) == 1) {
                parsed.capabilities = capabilities;
                parsed.has_capabilities = true;
            } else {
                uiPrintf(state, "ParsePermArgs: invalid capabilities \"%s\"\n", args[i + 1]);
                bad++;
            }
            continue;
        }
        if (strcmp("selabel", args[i]) == 0) {
            if (args[i+1][0] != '\0') {
                parsed.selabel = args[i+1];
                parsed.has_selabel = true;
            } else {
                uiPrintf(state, "ParsePermArgs: invalid selabel \"%s\"\n", args[i + 1]);
                bad++;
            }
            continue;
        }
        if (max_warnings != 0) {
            printf("ParsedPermArgs: unknown key \"%s\", ignoring\n", args[i]);
            max_warnings--;
            if (max_warnings == 0) {
                printf("ParsedPermArgs: suppressing further warnings\n");
            }
        }
    }
    return parsed;
}

static int ApplyParsedPerms(
        State * state,
        const char* filename,
        const struct stat *statptr,
        struct perm_parsed_args parsed)
{
    int bad = 0;

    if (parsed.has_selabel) {
        if (lsetfilecon(filename, parsed.selabel) != 0) {
            uiPrintf(state, "ApplyParsedPerms: lsetfilecon of %s to %s failed: %s\n",
                    filename, parsed.selabel, strerror(errno));
            bad++;
        }
    }

    /* ignore symlinks */
    if (S_ISLNK(statptr->st_mode)) {
        return bad;
    }

    if (parsed.has_uid) {
        if (chown(filename, parsed.uid, -1) < 0) {
            uiPrintf(state, "ApplyParsedPerms: chown of %s to %d failed: %s\n",
                    filename, parsed.uid, strerror(errno));
            bad++;
        }
    }

    if (parsed.has_gid) {
        if (chown(filename, -1, parsed.gid) < 0) {
            uiPrintf(state, "ApplyParsedPerms: chgrp of %s to %d failed: %s\n",
                    filename, parsed.gid, strerror(errno));
            bad++;
        }
    }

    if (parsed.has_mode) {
        if (chmod(filename, parsed.mode) < 0) {
            uiPrintf(state, "ApplyParsedPerms: chmod of %s to %d failed: %s\n",
                    filename, parsed.mode, strerror(errno));
            bad++;
        }
    }

    if (parsed.has_dmode && S_ISDIR(statptr->st_mode)) {
        if (chmod(filename, parsed.dmode) < 0) {
            uiPrintf(state, "ApplyParsedPerms: chmod of %s to %d failed: %s\n",
                    filename, parsed.dmode, strerror(errno));
            bad++;
        }
    }

    if (parsed.has_fmode && S_ISREG(statptr->st_mode)) {
        if (chmod(filename, parsed.fmode) < 0) {
            uiPrintf(state, "ApplyParsedPerms: chmod of %s to %d failed: %s\n",
                   filename, parsed.fmode, strerror(errno));
            bad++;
        }
    }

    if (parsed.has_capabilities && S_ISREG(statptr->st_mode)) {
        if (parsed.capabilities == 0) {
            if ((removexattr(filename, XATTR_NAME_CAPS) == -1) && (errno != ENODATA)) {
                // Report failure unless it's ENODATA (attribute not set)
                uiPrintf(state, "ApplyParsedPerms: removexattr of %s to %" PRIx64 " failed: %s\n",
                       filename, parsed.capabilities, strerror(errno));
                bad++;
            }
        } else {
            struct vfs_cap_data cap_data;
            memset(&cap_data, 0, sizeof(cap_data));
            cap_data.magic_etc = VFS_CAP_REVISION | VFS_CAP_FLAGS_EFFECTIVE;
            cap_data.data[0].permitted = (uint32_t) (parsed.capabilities & 0xffffffff);
            cap_data.data[0].inheritable = 0;
            cap_data.data[1].permitted = (uint32_t) (parsed.capabilities >> 32);
            cap_data.data[1].inheritable = 0;
            if (setxattr(filename, XATTR_NAME_CAPS, &cap_data, sizeof(cap_data), 0) < 0) {
                uiPrintf(state, "ApplyParsedPerms: setcap of %s to %" PRIx64 " failed: %s\n",
                        filename, parsed.capabilities, strerror(errno));
                bad++;
            }
        }
    }

    return bad;
}

// nftw doesn't allow us to pass along context, so we need to use
// global variables.  *sigh*
static struct perm_parsed_args recursive_parsed_args;
static State* recursive_state;

static int do_SetMetadataRecursive(const char* filename, const struct stat *statptr,
        int fileflags, struct FTW *pfwt) {
    return ApplyParsedPerms(recursive_state, filename, statptr, recursive_parsed_args);
}

static Value* SetMetadataFn(const char* name, State* state, int argc, Expr* argv[]) {
    int i;
    int bad = 0;
    static int nwarnings = 0;
    struct stat sb;
    Value* result = NULL;

    bool recursive = (strcmp(name, "set_metadata_recursive") == 0);

    if ((argc % 2) != 1) {
        return ErrorAbort(state, "%s() expects an odd number of arguments, got %d",
                          name, argc);
    }

    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) return NULL;

    if (lstat(args[0], &sb) == -1) {
        result = ErrorAbort(state, "%s: Error on lstat of \"%s\": %s", name, args[0], strerror(errno));
        goto done;
    }

    struct perm_parsed_args parsed = ParsePermArgs(state, argc, args);

    if (recursive) {
        recursive_parsed_args = parsed;
        recursive_state = state;
        bad += nftw(args[0], do_SetMetadataRecursive, 30, FTW_CHDIR | FTW_DEPTH | FTW_PHYS);
        memset(&recursive_parsed_args, 0, sizeof(recursive_parsed_args));
        recursive_state = NULL;
    } else {
        bad += ApplyParsedPerms(state, args[0], &sb, parsed);
    }

done:
    for (i = 0; i < argc; ++i) {
        free(args[i]);
    }
    free(args);

    if (result != NULL) {
        return result;
    }

    if (bad > 0) {
        return ErrorAbort(state, "%s: some changes failed", name);
    }

    return StringValue(strdup(""));
}

Value* GetPropFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* key;
    key = Evaluate(state, argv[0]);
    if (key == NULL) return NULL;

    char value[PROPERTY_VALUE_MAX];
    property_get(key, value, "");
    free(key);

    return StringValue(strdup(value));
}

//Check to confirm if this is the same hardware as the one the package was
//generated on or not. 32 vs 64 bit variants are upgrade compatible but have
//names such as msmWXYZ msmWXYZ_32 vs msmWXYZ_64.Input to this
//function is the BuildProp value that gets stored in the update package
//at the time it it created.
Value* ConfirmDevVariant(const char* name, State* state, int argc, Expr* argv[])
{
    //ro.product.device that was on the build that the update package was made
    //from
    char* package_dev_variant;
    //ro.product.device on the current hardware
    char current_dev_variant[PROPERTY_VALUE_MAX];
    int comparison_len;
    int package_dev_variant_len;
    int current_dev_variant_len;
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    package_dev_variant = Evaluate(state, argv[0]);
    if (!package_dev_variant) goto error;
    property_get("ro.product.device", current_dev_variant, "n/a");
    if (!strncmp(current_dev_variant,"n/a",3)) {
        ErrorAbort(state, "Failed to get valid ro.product.device");
        goto error;
    }
    package_dev_variant_len = strlen(package_dev_variant);
    current_dev_variant_len = strlen(current_dev_variant);
    //Ensure device variant lengths are atleast 3 characters long
    if ((package_dev_variant_len < 3) || (current_dev_variant_len < 3)) {
        ErrorAbort(state, "Device Variant length is less than 3 characters");
        goto error;
    }
    //Length of the largest string - 3(for _32/64)
    comparison_len =
        (package_dev_variant_len >= current_dev_variant_len ?
        package_dev_variant_len :
        current_dev_variant_len) - 3;
    //Complete match
    if (!strncmp(current_dev_variant, package_dev_variant,
                 strlen(current_dev_variant)))
        goto success;
    //Match except for the last 3 char's of either string which are _32 or _64
    if (!strncmp(current_dev_variant, package_dev_variant, comparison_len)) {
        if (package_dev_variant_len >= current_dev_variant_len) {
            if (!strncmp(&package_dev_variant[package_dev_variant_len-3],
                         "_32", 3) ||
                !strncmp(&package_dev_variant[package_dev_variant_len-3],
                         "_64", 3))
                goto success;
        } else {
            if (!strncmp(&current_dev_variant[current_dev_variant_len-3],
                         "_32", 3) ||
                !strncmp(&current_dev_variant[current_dev_variant_len-3],
                         "_64", 3))
                goto success;
        }
        ErrorAbort(state, "Invalid target for update package");
        goto error;
    }
success:
    free(package_dev_variant);
    return StringValue(strdup("OK"));
error:
    if (package_dev_variant) {
        free(package_dev_variant);
    }
    return StringValue(strdup("ERROR"));
}

// file_getprop(file, key)
//
//   interprets 'file' as a getprop-style file (key=value pairs, one
//   per line. # comment lines,blank lines, lines without '=' ignored),
//   and returns the value for 'key' (or "" if it isn't defined).
Value* FileGetPropFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;
    char* buffer = NULL;
    char* filename;
    char* key;
    if (ReadArgs(state, argv, 2, &filename, &key) < 0) {
        return NULL;
    }

    struct stat st;
    if (stat(filename, &st) < 0) {
        ErrorAbort(state, "%s: failed to stat \"%s\": %s",
                   name, filename, strerror(errno));
        goto done;
    }

#define MAX_FILE_GETPROP_SIZE    65536

    if (st.st_size > MAX_FILE_GETPROP_SIZE) {
        ErrorAbort(state, "%s too large for %s (max %d)",
                   filename, name, MAX_FILE_GETPROP_SIZE);
        goto done;
    }

    buffer = malloc(st.st_size+1);
    if (buffer == NULL) {
        ErrorAbort(state, "%s: failed to alloc %lld bytes", name, (long long)st.st_size+1);
        goto done;
    }

    FILE* f = fopen(filename, "rb");
    if (f == NULL) {
        ErrorAbort(state, "%s: failed to open %s: %s",
                   name, filename, strerror(errno));
        goto done;
    }

    if (fread(buffer, 1, st.st_size, f) != st.st_size) {
        ErrorAbort(state, "%s: failed to read %lld bytes from %s",
                   name, (long long)st.st_size+1, filename);
        fclose(f);
        goto done;
    }
    buffer[st.st_size] = '\0';

    fclose(f);

    char* line = strtok(buffer, "\n");
    do {
        // skip whitespace at start of line
        while (*line && isspace(*line)) ++line;

        // comment or blank line: skip to next line
        if (*line == '\0' || *line == '#') continue;

        char* equal = strchr(line, '=');
        if (equal == NULL) {
            continue;
        }

        // trim whitespace between key and '='
        char* key_end = equal-1;
        while (key_end > line && isspace(*key_end)) --key_end;
        key_end[1] = '\0';

        // not the key we're looking for
        if (strcmp(key, line) != 0) continue;

        // skip whitespace after the '=' to the start of the value
        char* val_start = equal+1;
        while(*val_start && isspace(*val_start)) ++val_start;

        // trim trailing whitespace
        char* val_end = val_start + strlen(val_start)-1;
        while (val_end > val_start && isspace(*val_end)) --val_end;
        val_end[1] = '\0';

        result = strdup(val_start);
        break;

    } while ((line = strtok(NULL, "\n")));

    if (result == NULL) result = strdup("");

  done:
    free(filename);
    free(key);
    free(buffer);
    return StringValue(result);
}


static bool write_raw_image_cb(const unsigned char* data,
                               int data_len, void* ctx) {
    int r = mtd_write_data((MtdWriteContext*)ctx, (const char *)data, data_len);
    if (r == data_len) return true;
    printf("%s\n", strerror(errno));
    return false;
}

// write_raw_image(filename_or_blob, partition)
Value* WriteRawImageFn(const char* name, State* state, int argc, Expr* argv[]) {
    char* result = NULL;

    Value* partition_value;
    Value* contents;
    if (ReadValueArgs(state, argv, 2, &contents, &partition_value) < 0) {
        return NULL;
    }

    char* partition = NULL;
    if (partition_value->type != VAL_STRING) {
        ErrorAbort(state, "partition argument to %s must be string", name);
        goto done;
    }
    partition = partition_value->data;
    if (strlen(partition) == 0) {
        ErrorAbort(state, "partition argument to %s can't be empty", name);
        goto done;
    }
    if (contents->type == VAL_STRING && strlen((char*) contents->data) == 0) {
        ErrorAbort(state, "file argument to %s can't be empty", name);
        goto done;
    }

    mtd_scan_partitions();
    const MtdPartition* mtd = mtd_find_partition_by_name(partition);
    if (mtd == NULL) {
        printf("%s: no mtd partition named \"%s\"\n", name, partition);
        result = strdup("");
        goto done;
    }

    MtdWriteContext* ctx = mtd_write_partition(mtd);
    if (ctx == NULL) {
        printf("%s: can't write mtd partition \"%s\"\n",
                name, partition);
        result = strdup("");
        goto done;
    }

    bool success;

    if (contents->type == VAL_STRING) {
        // we're given a filename as the contents
        char* filename = contents->data;
        FILE* f = fopen(filename, "rb");
        if (f == NULL) {
            printf("%s: can't open %s: %s\n",
                    name, filename, strerror(errno));
            result = strdup("");
            goto done;
        }

        success = true;
        char* buffer = malloc(BUFSIZ);
        int read;
        while (success && (read = fread(buffer, 1, BUFSIZ, f)) > 0) {
            int wrote = mtd_write_data(ctx, buffer, read);
            success = success && (wrote == read);
        }
        free(buffer);
        fclose(f);
    } else {
        // we're given a blob as the contents
        ssize_t wrote = mtd_write_data(ctx, contents->data, contents->size);
        success = (wrote == contents->size);
    }
    if (!success) {
        printf("mtd_write_data to %s failed: %s\n",
                partition, strerror(errno));
    }

    if (mtd_erase_blocks(ctx, -1) == -1) {
        printf("%s: error erasing blocks of %s\n", name, partition);
    }
    if (mtd_write_close(ctx) != 0) {
        printf("%s: error closing write of %s\n", name, partition);
    }

    printf("%s %s partition\n",
           success ? "wrote" : "failed to write", partition);

    result = success ? partition : strdup("");

done:
    if (result != partition) FreeValue(partition_value);
    FreeValue(contents);
    return StringValue(result);
}

// apply_patch_space(bytes)
Value* ApplyPatchSpaceFn(const char* name, State* state,
                         int argc, Expr* argv[]) {
    char* bytes_str;
    if (ReadArgs(state, argv, 1, &bytes_str) < 0) {
        return NULL;
    }

    char* endptr;
    size_t bytes = strtol(bytes_str, &endptr, 10);
    if (bytes == 0 && endptr == bytes_str) {
        ErrorAbort(state, "%s(): can't parse \"%s\" as byte count\n\n",
                   name, bytes_str);
        free(bytes_str);
        return NULL;
    }

    return StringValue(strdup(CacheSizeCheck(bytes) ? "" : "t"));
}

// apply_patch(file, size, init_sha1, tgt_sha1, patch)

Value* ApplyPatchFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc < 6 || (argc % 2) == 1) {
        return ErrorAbort(state, "%s(): expected at least 6 args and an "
                                 "even number, got %d",
                          name, argc);
    }

    char* source_filename;
    char* target_filename;
    char* target_sha1;
    char* target_size_str;
    if (ReadArgs(state, argv, 4, &source_filename, &target_filename,
                 &target_sha1, &target_size_str) < 0) {
        return NULL;
    }

    char* endptr;
    size_t target_size = strtol(target_size_str, &endptr, 10);
    if (target_size == 0 && endptr == target_size_str) {
        ErrorAbort(state, "%s(): can't parse \"%s\" as byte count",
                   name, target_size_str);
        free(source_filename);
        free(target_filename);
        free(target_sha1);
        free(target_size_str);
        return NULL;
    }

    int patchcount = (argc-4) / 2;
    Value** patches = ReadValueVarArgs(state, argc-4, argv+4);

    int i;
    for (i = 0; i < patchcount; ++i) {
        if (patches[i*2]->type != VAL_STRING) {
            ErrorAbort(state, "%s(): sha-1 #%d is not string", name, i);
            break;
        }
        if (patches[i*2+1]->type != VAL_BLOB) {
            ErrorAbort(state, "%s(): patch #%d is not blob", name, i);
            break;
        }
    }
    if (i != patchcount) {
        for (i = 0; i < patchcount*2; ++i) {
            FreeValue(patches[i]);
        }
        free(patches);
        return NULL;
    }

    char** patch_sha_str = malloc(patchcount * sizeof(char*));
    for (i = 0; i < patchcount; ++i) {
        patch_sha_str[i] = patches[i*2]->data;
        patches[i*2]->data = NULL;
        FreeValue(patches[i*2]);
        patches[i] = patches[i*2+1];
    }

    int result = applypatch(source_filename, target_filename,
                            target_sha1, target_size,
                            patchcount, patch_sha_str, patches, NULL);

    for (i = 0; i < patchcount; ++i) {
        FreeValue(patches[i]);
    }
    free(patch_sha_str);
    free(patches);

    return StringValue(strdup(result == 0 ? "t" : ""));
}

// apply_patch_check(file, [sha1_1, ...])
Value* ApplyPatchCheckFn(const char* name, State* state,
                         int argc, Expr* argv[]) {
    if (argc < 1) {
        return ErrorAbort(state, "%s(): expected at least 1 arg, got %d",
                          name, argc);
    }

    char* filename;
    if (ReadArgs(state, argv, 1, &filename) < 0) {
        return NULL;
    }

    int patchcount = argc-1;
    char** sha1s = ReadVarArgs(state, argc-1, argv+1);

    int result = applypatch_check(filename, patchcount, sha1s);

    int i;
    for (i = 0; i < patchcount; ++i) {
        free(sha1s[i]);
    }
    free(sha1s);

    return StringValue(strdup(result == 0 ? "t" : ""));
}

Value* UIPrintFn(const char* name, State* state, int argc, Expr* argv[]) {
    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) {
        return NULL;
    }

    int size = 0;
    int i;
    for (i = 0; i < argc; ++i) {
        size += strlen(args[i]);
    }
    char* buffer = malloc(size+1);
    size = 0;
    for (i = 0; i < argc; ++i) {
        strcpy(buffer+size, args[i]);
        size += strlen(args[i]);
        free(args[i]);
    }
    free(args);
    buffer[size] = '\0';
    uiPrint(state, buffer);
    return StringValue(buffer);
}

Value* WipeCacheFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 0) {
        return ErrorAbort(state, "%s() expects no args, got %d", name, argc);
    }
    fprintf(((UpdaterInfo*)(state->cookie))->cmd_pipe, "wipe_cache\n");
    return StringValue(strdup("t"));
}

Value* RunProgramFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc < 1) {
        return ErrorAbort(state, "%s() expects at least 1 arg", name);
    }
    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) {
        return NULL;
    }

    char** args2 = malloc(sizeof(char*) * (argc+1));
    memcpy(args2, args, sizeof(char*) * argc);
    args2[argc] = NULL;

    printf("about to run program [%s] with %d args\n", args2[0], argc);

    pid_t child = fork();
    if (child == 0) {
        execv(args2[0], args2);
        printf("run_program: execv failed: %s\n", strerror(errno));
        _exit(1);
    }
    int status;
    waitpid(child, &status, 0);
    if (WIFEXITED(status)) {
        if (WEXITSTATUS(status) != 0) {
            printf("run_program: child exited with status %d\n",
                    WEXITSTATUS(status));
        }
    } else if (WIFSIGNALED(status)) {
        printf("run_program: child terminated by signal %d\n",
                WTERMSIG(status));
    }

    int i;
    for (i = 0; i < argc; ++i) {
        free(args[i]);
    }
    free(args);
    free(args2);

    char buffer[20];
    sprintf(buffer, "%d", status);

    return StringValue(strdup(buffer));
}

// sha1_check(data)
//    to return the sha1 of the data (given in the format returned by
//    read_file).
//
// sha1_check(data, sha1_hex, [sha1_hex, ...])
//    returns the sha1 of the file if it matches any of the hex
//    strings passed, or "" if it does not equal any of them.
//
Value* Sha1CheckFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc < 1) {
        return ErrorAbort(state, "%s() expects at least 1 arg", name);
    }

    Value** args = ReadValueVarArgs(state, argc, argv);
    if (args == NULL) {
        return NULL;
    }

    if (args[0]->size < 0) {
        return StringValue(strdup(""));
    }
    uint8_t digest[SHA_DIGEST_SIZE];
    SHA_hash(args[0]->data, args[0]->size, digest);
    FreeValue(args[0]);

    if (argc == 1) {
        return StringValue(PrintSha1(digest));
    }

    int i;
    uint8_t* arg_digest = malloc(SHA_DIGEST_SIZE);
    for (i = 1; i < argc; ++i) {
        if (args[i]->type != VAL_STRING) {
            printf("%s(): arg %d is not a string; skipping",
                    name, i);
        } else if (ParseSha1(args[i]->data, arg_digest) != 0) {
            // Warn about bad args and skip them.
            printf("%s(): error parsing \"%s\" as sha-1; skipping",
                   name, args[i]->data);
        } else if (memcmp(digest, arg_digest, SHA_DIGEST_SIZE) == 0) {
            break;
        }
        FreeValue(args[i]);
    }
    if (i >= argc) {
        // Didn't match any of the hex strings; return false.
        return StringValue(strdup(""));
    }
    // Found a match; free all the remaining arguments and return the
    // matched one.
    int j;
    for (j = i+1; j < argc; ++j) {
        FreeValue(args[j]);
    }
    return args[i];
}

// Read a local file and return its contents (the Value* returned
// is actually a FileContents*).
Value* ReadFileFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }
    char* filename;
    if (ReadArgs(state, argv, 1, &filename) < 0) return NULL;

    Value* v = malloc(sizeof(Value));
    v->type = VAL_BLOB;

    FileContents fc;
    if (LoadFileContents(filename, &fc) != 0) {
        free(filename);
        v->size = -1;
        v->data = NULL;
        free(fc.data);
        return v;
    }

    v->size = fc.size;
    v->data = (char*)fc.data;

    free(filename);
    return v;
}

// Immediately reboot the device.  Recovery is not finished normally,
// so if you reboot into recovery it will re-start applying the
// current package (because nothing has cleared the copy of the
// arguments stored in the BCB).
//
// The argument is the partition name passed to the android reboot
// property.  It can be "recovery" to boot from the recovery
// partition, or "" (empty string) to boot from the regular boot
// partition.
Value* RebootNowFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }

    char* filename;
    char* property;
    if (ReadArgs(state, argv, 2, &filename, &property) < 0) return NULL;

    char buffer[80];

    // zero out the 'command' field of the bootloader message.
    memset(buffer, 0, sizeof(((struct bootloader_message*)0)->command));
    FILE* f = fopen(filename, "r+b");
    fseek(f, offsetof(struct bootloader_message, command), SEEK_SET);
    fwrite(buffer, sizeof(((struct bootloader_message*)0)->command), 1, f);
    fclose(f);
    free(filename);

    strcpy(buffer, "reboot,");
    if (property != NULL) {
        strncat(buffer, property, sizeof(buffer)-10);
    }

    property_set(ANDROID_RB_PROPERTY, buffer);

    sleep(5);
    free(property);
    ErrorAbort(state, "%s() failed to reboot", name);
    return NULL;
}

// Store a string value somewhere that future invocations of recovery
// can access it.  This value is called the "stage" and can be used to
// drive packages that need to do reboots in the middle of
// installation and keep track of where they are in the multi-stage
// install.
//
// The first argument is the block device for the misc partition
// ("/misc" in the fstab), which is where this value is stored.  The
// second argument is the string to store; it should not exceed 31
// bytes.
Value* SetStageFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }

    char* filename;
    char* stagestr;
    if (ReadArgs(state, argv, 2, &filename, &stagestr) < 0) return NULL;

    // Store this value in the misc partition, immediately after the
    // bootloader message that the main recovery uses to save its
    // arguments in case of the device restarting midway through
    // package installation.
    FILE* f = fopen(filename, "r+b");
    fseek(f, offsetof(struct bootloader_message, stage), SEEK_SET);
    int to_write = strlen(stagestr)+1;
    int max_size = sizeof(((struct bootloader_message*)0)->stage);
    if (to_write > max_size) {
        to_write = max_size;
        stagestr[max_size-1] = 0;
    }
    fwrite(stagestr, to_write, 1, f);
    fclose(f);

    free(stagestr);
    return StringValue(filename);
}

// Return the value most recently saved with SetStageFn.  The argument
// is the block device for the misc partition.
Value* GetStageFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 1) {
        return ErrorAbort(state, "%s() expects 1 arg, got %d", name, argc);
    }

    char* filename;
    if (ReadArgs(state, argv, 1, &filename) < 0) return NULL;

    char buffer[sizeof(((struct bootloader_message*)0)->stage)];
    FILE* f = fopen(filename, "rb");
    fseek(f, offsetof(struct bootloader_message, stage), SEEK_SET);
    fread(buffer, sizeof(buffer), 1, f);
    fclose(f);
    buffer[sizeof(buffer)-1] = '\0';

    return StringValue(strdup(buffer));
}

Value* WipeBlockDeviceFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 2) {
        return ErrorAbort(state, "%s() expects 2 args, got %d", name, argc);
    }

    char* filename;
    char* len_str;
    if (ReadArgs(state, argv, 2, &filename, &len_str) < 0) return NULL;

    size_t len = strtoull(len_str, NULL, 0);
    int fd = open(filename, O_WRONLY, 0644);
    int success = wipe_block_device(fd, len);

    free(filename);
    free(len_str);

    close(fd);

    return StringValue(strdup(success ? "t" : ""));
}

Value* EnableRebootFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc != 0) {
        return ErrorAbort(state, "%s() expects no args, got %d", name, argc);
    }
    UpdaterInfo* ui = (UpdaterInfo*)(state->cookie);
    fprintf(ui->cmd_pipe, "enable_reboot\n");
    return StringValue(strdup("t"));
}

/* Need tune2fs to enable */
/*
Value* Tune2FsFn(const char* name, State* state, int argc, Expr* argv[]) {
    if (argc == 0) {
        return ErrorAbort(state, "%s() expects args, got %d", name, argc);
    }

    char** args = ReadVarArgs(state, argc, argv);
    if (args == NULL) {
        return ErrorAbort(state, "%s() could not read args", name);
    }

    int i;
    char** args2 = malloc(sizeof(char*) * (argc+1));
    // Tune2fs expects the program name as its args[0]
    args2[0] = strdup(name);
    for (i = 0; i < argc; ++i) {
       args2[i + 1] = args[i];
    }
    int result = tune2fs_main(argc + 1, args2);
    for (i = 0; i < argc; ++i) {
        free(args[i]);
    }
    free(args);

    free(args2[0]);
    free(args2);
    if (result != 0) {
        return ErrorAbort(state, "%s() returned error code %d", name, result);
    }
    return StringValue(strdup("t"));
}
*/

void RegisterInstallFunctions() {
    RegisterFunction("mount", MountFn);
    RegisterFunction("is_mounted", IsMountedFn);
    RegisterFunction("unmount", UnmountFn);
    RegisterFunction("format", FormatFn);
    RegisterFunction("show_progress", ShowProgressFn);
    RegisterFunction("set_progress", SetProgressFn);
    RegisterFunction("delete", DeleteFn);
    RegisterFunction("delete_recursive", DeleteFn);
    RegisterFunction("package_extract_dir", PackageExtractDirFn);
    RegisterFunction("package_extract_file", PackageExtractFileFn);
    RegisterFunction("symlink", SymlinkFn);

    // Maybe, at some future point, we can delete these functions? They have been
    // replaced by perm_set and perm_set_recursive.
    RegisterFunction("set_perm", SetPermFn);
    RegisterFunction("set_perm_recursive", SetPermFn);

    // Usage:
    //   set_metadata("filename", "key1", "value1", "key2", "value2", ...)
    // Example:
    //   set_metadata("/system/bin/netcfg", "uid", 0, "gid", 3003, "mode", 02750, "selabel", "u:object_r:system_file:s0", "capabilities", 0x0);
    RegisterFunction("set_metadata", SetMetadataFn);

    // Usage:
    //   set_metadata_recursive("dirname", "key1", "value1", "key2", "value2", ...)
    // Example:
    //   set_metadata_recursive("/system", "uid", 0, "gid", 0, "fmode", 0644, "dmode", 0755, "selabel", "u:object_r:system_file:s0", "capabilities", 0x0);
    RegisterFunction("set_metadata_recursive", SetMetadataFn);

    RegisterFunction("getprop", GetPropFn);
    RegisterFunction("file_getprop", FileGetPropFn);
    RegisterFunction("write_raw_image", WriteRawImageFn);

    RegisterFunction("apply_patch", ApplyPatchFn);
    RegisterFunction("apply_patch_check", ApplyPatchCheckFn);
    RegisterFunction("apply_patch_space", ApplyPatchSpaceFn);

    RegisterFunction("wipe_block_device", WipeBlockDeviceFn);

    RegisterFunction("read_file", ReadFileFn);
    RegisterFunction("sha1_check", Sha1CheckFn);
    RegisterFunction("rename", RenameFn);

    RegisterFunction("wipe_cache", WipeCacheFn);

    RegisterFunction("ui_print", UIPrintFn);

    RegisterFunction("run_program", RunProgramFn);

    RegisterFunction("reboot_now", RebootNowFn);
    RegisterFunction("get_stage", GetStageFn);
    RegisterFunction("set_stage", SetStageFn);

    RegisterFunction("enable_reboot", EnableRebootFn);
    /* Need tune2fs to enable */
    /*RegisterFunction("tune2fs", Tune2FsFn); */
    RegisterFunction("get_device_compatible", ConfirmDevVariant);
}
