/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <cutils/misc.h>
#include <sys/syscall.h>
#include <sys/utsname.h>

#define LOG_TAG "ProbeModule"
#include <cutils/log.h>

#define LDM_DEFAULT_MOD_PATH "/system/lib/modules/"

extern int delete_module(const char *, unsigned int);

/* get_default_mod_path() interface to outside,
 * refer to its description in probe_module.h
 */
char *get_default_mod_path(char *def_mod_path)
{
    int len;
    struct utsname buf;
    uname(&buf);
    len = snprintf(def_mod_path, PATH_MAX, "%s", LDM_DEFAULT_MOD_PATH);
    strcpy(def_mod_path + len, buf.release);
    if (access(def_mod_path, F_OK))
        def_mod_path[len] = '\0';
    else
        strcat(def_mod_path, "/");
    return def_mod_path;
}

int insmod(const char *filename, const char *options, int flags)
{
    int fd = open(filename, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd == -1) {
        ALOGE("insmod: open(\"%s\") failed: %s", filename, strerror(errno));
        return -1;
    }
    int rc = syscall(__NR_finit_module, fd, options, flags);
    if (rc == -1) {
        if (errno == EEXIST) {
            rc = 0;
        } else {
            ALOGE("finit_module for \"%s\" failed: %s", filename, strerror(errno));
        }
    }
    close(fd);
    return rc;
}

static char *strip_path(char *str)
{
    char *ptr = strrchr(str, '/');
    return ptr ? ptr + 1 : str;
}

static void hyphen_to_underscore(char *str)
{
    while (str && *str != '\0') {
        if (*str == '-')
            *str = '_';
        str++;
    }
}

/* Compare module names, but don't differentiate '_' and '-'.
 * return: 0 when s1 is matched to s2 or size is zero.
 *         non-zero in any other cases.
 */
static int match_name(const char *s1, const char *s2, const size_t size)
{
    size_t i;

    if (!size)
        return 0;

    for (i = 0; i < size; i++, s1++, s2++) {

        if ((*s1 == '_' || *s1 == '-') && (*s2 == '_' || *s2 == '-'))
            continue;

        if (*s1 != *s2)
            return -1;

        if (*s1 == '\0')
            return 0;
    }

    return 0;
}

/* check if a line in dep file is target module's dependency.
 * return 1 when it is, otherwise 0 in any other cases.
 */
static int is_target_module(char *line, const char *target)
{
    char *token;
    char name[PATH_MAX];
    const char *delimiter = ":";
    int ret = 0;

    /* search token */
    token = strstr(line, delimiter);

    if (!token) {
        ALOGE("invalid line: no token");
        return 0;
    }

    /* only take stuff before the token */
    *token = '\0';

    /* use "module.ko" in comparision */
    strcat(strcpy(name, target), ".ko");

    ret = !match_name(strip_path(line), name, strlen(name));

    /* restore [single] token, keep line unchanged until we parse it later */
    *token = *delimiter;

    return ret;

}

/* turn a single string into an array of dependency.
 *
 * return: dependency array's address if it succeeded. Caller
 *         is responsible to free the array's memory.
 *         NULL when any error happens.
 */
static char **setup_dep(char *line)
{
    char *tmp = line;
    char *brk;
    int i;
    char **dep;

    for (i = 2; (tmp = strchr(tmp, ' ')); i++)
        tmp++;

    dep = malloc(sizeof(char *) * i);
    if (dep) {
        i = 0;
        do {
            tmp = strtok_r(i ? NULL : line, ": ", &brk);
        } while ((dep[i++] = tmp));
    }

    return dep;
}

/* install all modules in the dependency chain
 * deps    : A array of module file names, must be terminated by a NULL pointer
 * args    : The module parameters for target module.
 * strip   : Non-zero to strip out path info in the file name;
 *           0 to keep path info when loading modules.
 * base    : a prefix to module path, it will NOT be affected by strip flag.
 * return  : 0 for success or nothing to do; non-zero when any error occurs.
 */
static int insmod_s(char *dep[], const char *args, int strip, const char *base)
{
    char *name;
    int cnt;
    size_t len;
    int ret = 0;
    char path_name[PATH_MAX];
    char def_mod_path[PATH_MAX];
    const char *base_dir;

    if (base && strlen(base))
        base_dir = base;
    else
        base_dir = get_default_mod_path(def_mod_path);

    /* load modules in reversed order */
    for (cnt = 0; dep[cnt]; cnt++)
        ;

    len = strlen(strcpy(path_name, base_dir));

    while (!ret && cnt--) {

        name = strip ? strip_path(dep[cnt]) : dep[cnt];

        strcpy(path_name + len, name);

        ret = insmod(path_name, cnt ? "" : args, 0);
    }

    return ret;
}

/* remove all modules in a dependency chain
 * NOTE: We assume module name in kernel is same as the file name without .ko
 */
static int rmmod_s(char *dep[], int flags)
{
    int i;
    int ret = 0;

    for (i = 0; dep[i]; i++) {
        char *mod_name = strip_path(dep[i]);
        size_t len = strlen(mod_name);

        if (len > 3 && strstr(mod_name, ".ko") == (mod_name + len - 3)) {
            mod_name[len - 3] = '\0';

            hyphen_to_underscore(mod_name);

            ret = delete_module(mod_name, flags);

            if (ret) {
                ALOGE("%s: Failed to remove module [%s] error (%s)",
                        __FUNCTION__, mod_name, strerror(errno));
                break;

            }
        }
    }

    return ret;
}

/* look_up_dep() find and setup target module's dependency in modules.dep
 *
 * dep_file:    a pointer to module's dep file loaded in memory, its content
 *              will be CHANGED during parsing.
 *
 * return:      a pointer to an array which holds the dependency strings and
 *              terminated by a NULL pointer. Caller is responsible to free the
 *              array's memory.
 *
 *              non-zero in any other cases. Content of dep array is invalid.
 */
static char **look_up_dep(const char *module_name, void *dep_file)
{
    char *line;
    char *saved_pos;
    char *start;
    char **dep = NULL;

    if (!dep_file || !module_name || *module_name == '\0')
        return NULL;

    start = (char *)dep_file;

    /* We expect modules.dep file has a new line char before EOF. */
    while ((line = strtok_r(start, "\n", &saved_pos)) != NULL) {

        start = NULL;

        if (is_target_module(line, module_name)) {

            dep = setup_dep(line);
            /* job done */
            break;
        }
    }

    return dep;
}

/* load_dep_file() load a dep file (usually it is modules.dep)
 * into memory. Caller is responsible to free the memory.
 *
 * file_name:   dep file's name, if it is NULL or an empty string,
 *              This function will try to load a dep file in the
 *              default path defined in LDM_DEFAULT_DEP_FILE
 *
 * return:      a pointer to the allocated mem which holds all
 *              content of the depfile. a zero pointer will be
 *              returned for any errors.
 * */
static void *load_dep_file(const char *file_name)
{
    unsigned int len;
    char def_mod_path[PATH_MAX];
    if (!file_name || *file_name == '\0') {
        file_name = get_default_mod_path(def_mod_path);
        strcat(def_mod_path, "modules.dep");
    }

    return load_file(file_name, &len);
}

/* insmod_by_dep() interface to outside,
 * refer to its description in probe_module.h
 */
int insmod_by_dep(const char *module_name,
        const char *args,
        const char *dep_name,
        int strip,
        const char *base)
{
    void *dep_file;
    char **dep = NULL;
    int ret = -1;

    if (!module_name || *module_name == '\0') {
        ALOGE("need valid module name");
        return ret;
    }

    dep_file = load_dep_file(dep_name);

    if (!dep_file) {
        ALOGE("cannot load dep file : %s", dep_name);
        return ret;
    }

    dep = look_up_dep(module_name, dep_file);

    if (!dep) {
        ALOGE("%s: cannot load module: [%s]", __FUNCTION__, module_name);
        goto free_file;
    }

    ret = insmod_s(dep, args, strip, base);

    free(dep);

free_file:
    free(dep_file);

    return ret;

}

/* rmmod_by_dep() interface to outside,
 * refer to its description in probe_module.h
 */
int rmmod_by_dep(const char *module_name, const char *dep_name)
{
    void *dep_file;
    char **dep = NULL;
    int ret = -1;

    if (!module_name || *module_name == '\0') {
        ALOGE("need valid module name");
        return ret;
    }

    dep_file = load_dep_file(dep_name);

    if (!dep_file) {
        ALOGE("cannot load dep file : %s", dep_name);
        return ret;
    }

    dep = look_up_dep(module_name, dep_file);

    if (!dep) {
        ALOGE("%s: cannot remove module: [%s]", __FUNCTION__, module_name);
        goto free_file;
    }

    ret = rmmod_s(dep, O_NONBLOCK);

    free(dep);

free_file:
    free(dep_file);

    return ret;
}

/* end of file */
