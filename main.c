#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>

#include "types.h"
#include "image.h"
#include "utils.h"
#include "mount_helpers.h"
#include "pfs_mount.h"
#include "ufs_mount.h"
#include "exfat_mount.h"
#include "install.h"

int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallTitleDir(const char* title_id, const char* install_path, void* reserved);
int sceAppInstUtilAppInstallAll(void* reserved);

int main(void) {
    char cwd[PATH_MAX];
    char title_id[32] = {};
    char system_ex_app[PATH_MAX];
    char user_app_dir[PATH_MAX];
    char user_sce_sys[PATH_MAX];
    char src_sce_sys[PATH_MAX];
    char mount_lnk_path[PATH_MAX];
	
    notify("Dump Installer 1.06 Beta - PFS + UFS + exFAT");

    const char *dirs[] = {
        "/data/imgmnt",
        "/data/imgmnt/exfatmnt",
        "/data/imgmnt/pfsmnt",
        "/data/imgmnt/ufsmnt"
    };
    
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        if (mkdir(dirs[i], 0755) != 0) {
            if (errno != EEXIST) {
                // Try to create parent directory first
                char parent[PATH_MAX];
                strncpy(parent, dirs[i], sizeof(parent) - 1);
                parent[sizeof(parent)-1] = '\0';
                
                char *last_slash = strrchr(parent, '/');
                if (last_slash && last_slash != parent) {
                    *last_slash = '\0';
                    mkdir(parent, 0755);   // ignore error, we'll try again
                }

                if (mkdir(dirs[i], 0755) != 0 && errno != EEXIST) {
                    printf("Failed to create %s (errno %d: %s)\n", 
                           dirs[i], errno, strerror(errno));
                    return 1;
                }
            }
        }
    }
	
    if (!getcwd(cwd, sizeof(cwd))) {
        printf("Error: Unable to determine working directory\n");
        return -1;
    }

    char image_file[MAX_PATH] = {};
    bool is_ufs = false;
    bool is_pfs = false;
    bool is_exfat = false;
    bool has_image = find_image_in_dir(cwd, image_file, sizeof(image_file),
                                       &is_ufs, &is_pfs, &is_exfat);
    char mount_point[MAX_PATH] = {0};
    const char* nullfs_src = cwd;

    if (has_image) {      
        if (is_ufs) {
            notify("UFS image detected: %s", strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file);
            if (mount_ufs_image(image_file, mount_point)) {
                nullfs_src = mount_point;
                char src_sce_sys_tmp[MAX_PATH];
                snprintf(src_sce_sys_tmp, sizeof(src_sce_sys_tmp), "%s/sce_sys", mount_point);
                snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
                mkdir(src_sce_sys, 0755);
                if (copy_dir(src_sce_sys_tmp, src_sce_sys) == 0) {
                    //notify("sce_sys extracted from UFS mount");
					nullfs_src = mount_point;
                } else {
                    notify("Warning: sce_sys copy from UFS failed");
					unmount_ufs(mount_point);
                    nullfs_src = cwd;
                }
            } else {
                notify("UFS mount failed - falling back to folder mode");
            }
        } else if (is_pfs) {
            notify("PFS image detected: %s", strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file);
            if (strlen(mount_point) > 0) unmount_pfs(mount_point);

            if (mount_pfs_image(image_file, mount_point)) {
                char src_sce_sys_tmp[MAX_PATH];
                snprintf(src_sce_sys_tmp, sizeof(src_sce_sys_tmp), "%s/sce_sys", mount_point);
                snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
                mkdir(src_sce_sys, 0755);
                if (copy_dir(src_sce_sys_tmp, src_sce_sys) == 0) {
                    //notify("sce_sys copied from PFS");
                    nullfs_src = mount_point;
                } else {
                    notify("Failed to copy sce_sys from PFS - fallback to folder mode");
                    unmount_pfs(mount_point);
                    nullfs_src = cwd;
                }
            } else {
                notify("PFS mount failed - falling back to folder mode");
                nullfs_src = cwd;
            }
        } else if (is_exfat) {
            notify("ExFat image detected: %s", strrchr(image_file, '/') ? strrchr(image_file, '/') + 1 : image_file);
            if (mount_exfat_image(image_file, mount_point)) {
                nullfs_src = mount_point;
                char src_sce_sys_tmp[MAX_PATH];
                snprintf(src_sce_sys_tmp, sizeof(src_sce_sys_tmp), "%s/sce_sys", mount_point);
                snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
                mkdir(src_sce_sys, 0755);
                if (copy_dir(src_sce_sys_tmp, src_sce_sys) == 0) {
                    //notify("sce_sys extracted from ExFat mount");
					nullfs_src = mount_point;
                } else {
                    notify("Warning: sce_sys copy from ExFat failed");
					unmount_exfat(mount_point);
                    nullfs_src = cwd;
                }
            } else {
                notify("ExFat mount failed - falling back to folder mode");
            }
		}
    }

    // Safety check for sce_sys before title ID read
    char sce_sys_path[MAX_PATH];
    snprintf(sce_sys_path, sizeof(sce_sys_path), "%s/sce_sys", nullfs_src);
    if (access(sce_sys_path, F_OK) != 0) {
        notify("CRITICAL: No sce_sys folder at %s - cannot read Title ID", sce_sys_path);
        if (strlen(mount_point) > 0) {
            if (is_ufs) unmount_ufs(mount_point);
            else if (is_pfs) unmount_pfs(mount_point);
            else if (is_exfat) unmount_exfat(mount_point);
        }
        return -1;
    }

    if (get_title_id(nullfs_src, title_id, sizeof(title_id))) {
        printf("Error: Could not read Title ID from %s/sce_sys\n", nullfs_src);
        notify("Failed to read Title ID from %s/sce_sys", nullfs_src);
        if (strlen(mount_point) > 0) {
            if (is_ufs) unmount_ufs(mount_point);
            else if (is_pfs) unmount_pfs(mount_point);
            else if (is_exfat) unmount_exfat(mount_point);
        }
        return -1;
    }

    notify("Installing %s, please wait...", title_id);
    //printf("Installing %s, please wait...\n", title_id);

    snprintf(system_ex_app, sizeof(system_ex_app), "/system_ex/app/%s", title_id);
    mkdir(system_ex_app, 0755);

    if (is_mounted(system_ex_app)) {
        unmount(system_ex_app, 0);
    }

    //notify("Mounting nullfs: %s → %s", nullfs_src, system_ex_app);

    if (mount_nullfs(nullfs_src, system_ex_app)) {
        notify("nullfs mount failed → %s (errno %d)", system_ex_app, errno);
        //printf("Error: Failed to mount application (errno %d)\n", errno);
        if (strlen(mount_point) > 0) {
            if (is_ufs) unmount_ufs(mount_point);
            else if (is_pfs) unmount_pfs(mount_point);
            else if (is_exfat) unmount_exfat(mount_point);
        }
        return -1;
    }

    char test_sfo[MAX_PATH];
    snprintf(test_sfo, sizeof(test_sfo), "%s/sce_sys/param.sfo", system_ex_app);
    /*if (access(test_sfo, F_OK) == 0) {
        notify("param.sfo visible in mounted location: %s", test_sfo);
    } else {
        notify("CRITICAL: param.sfo NOT visible at %s - install may fail", test_sfo);
    }*/

    //notify("nullfs mounted OK → %s overlays %s", system_ex_app, nullfs_src);

    remount_system_ex();

    snprintf(user_app_dir, sizeof(user_app_dir), "/user/app/%s", title_id);
    mkdir(user_app_dir, 0755);

    snprintf(user_sce_sys, sizeof(user_sce_sys), "%s/sce_sys", user_app_dir);
    mkdir(user_sce_sys, 0755);

    snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
    copy_dir(src_sce_sys, user_sce_sys);

    copy_sce_sys_to_appmeta(src_sce_sys, title_id);

    /*sceAppInstUtilInitialize();
    int install_ret = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);
    if (install_ret != 0) {
        notify("sceAppInstUtilAppInstallTitleDir failed: ret=0x%08x errno=%d (%s)",
               install_ret, errno, strerror(errno));
        printf("Install failed: ret=0x%08x errno=%d (%s)\n",
               install_ret, errno, strerror(errno));
        if (strlen(mount_point) > 0) {
            if (is_ufs) unmount_ufs(mount_point);
            else if (is_pfs) unmount_pfs(mount_point);
            else if (is_exfat) unmount_exfat(mount_point);
        }
        return -1;
    }*/

    sceAppInstUtilInitialize();

    int install_ret = sceAppInstUtilAppInstallTitleDir(title_id, "/user/app/", 0);

    if (install_ret != 0) {
        notify("TitleDir failed: 0x%08x", install_ret);
        printf("TitleDir failed: 0x%08x\n", install_ret);
        printf("Falling back to AppInstallAll...\n");

        install_ret = sceAppInstUtilAppInstallAll(0);

        if (install_ret != 0) {
            notify("AppInstallAll failed: 0x%08x", install_ret);
            printf("AppInstallAll failed: 0x%08x\n", install_ret);

            if (strlen(mount_point) > 0) {
                if (is_ufs) unmount_ufs(mount_point);
                else if (is_pfs) unmount_pfs(mount_point);
                else if (is_exfat) unmount_exfat(mount_point);
            }
            return -1;
        } else {
            notify("Fallback succeeded (AppInstallAll)");
        }
    }
	
    snprintf(mount_lnk_path, sizeof(mount_lnk_path), "/user/app/%s/mount.lnk", title_id);

    FILE* f = fopen(mount_lnk_path, "w");
    if (f) {
        fprintf(f, "%s", cwd);
        fclose(f);
    }

    notify("Fixing Config, please wait...");

    update_trophy(title_id, src_sce_sys);
	
    sleep(3);
    update_snd0info(title_id);

    /*if (strlen(mount_point) > 0) {
        notify("%s kept mounted at %s for launch",
               is_ufs ? "UFS" : (is_exfat ? "exFAT" : "PFS"), mount_point);
    }*/

    if (has_image) {
        snprintf(src_sce_sys, sizeof(src_sce_sys), "%s/sce_sys", cwd);
        //notify("Cleaning up: removing %s ...", src_sce_sys);
        if (remove_dir(src_sce_sys) == 0) {
            //notify("sce_sys folder removed successfully");
        } else {
            //notify("Warning: failed to remove sce_sys folder (errno %d)", errno);
        }
    }

    notify("%s installed and ready to use!", title_id);
    //printf("%s installed and ready to use!\n", title_id);
    printf("The icon should now appear on the home screen.\n");

    return 0;
}