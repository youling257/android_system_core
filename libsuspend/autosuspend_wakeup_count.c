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

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define LOG_TAG "libsuspend"
//#define LOG_NDEBUG 0
#include <cutils/log.h>

#include <cutils/properties.h>
#include <linux/uinput.h>
#include <dirent.h>
#include <poll.h>

#include "autosuspend_ops.h"

#define SYS_POWER_WAKEUP_COUNT "/sys/power/wakeup_count"
#define MAX_POWERBTNS 3

static int uinput_fd = -1;
static int state_fd;
static int wakeup_count_fd;
static pthread_t suspend_thread;
static sem_t suspend_lockout;
static void (*wakeup_func)(bool success) = NULL;

static void emit_key(int ufd, int key_code, int val)
{
    struct input_event iev;
    iev.type = EV_KEY;
    iev.code = key_code;
    iev.value = val;
    iev.time.tv_sec = 0;
    iev.time.tv_usec = 0;
    write(ufd, &iev, sizeof(iev));
    iev.type = EV_SYN;
    iev.code = SYN_REPORT;
    iev.value = 0;
    write(ufd, &iev, sizeof(iev));
    ALOGD("send key %d (%d) on fd %d", key_code, val, ufd);
}

static void send_key_wakeup(int ufd)
{
    emit_key(ufd, KEY_WAKEUP, 1);
    emit_key(ufd, KEY_WAKEUP, 0);
}

static void send_key_power(int ufd, bool longpress)
{
    emit_key(ufd, KEY_POWER, 1);
    if (longpress) sleep(2);
    emit_key(ufd, KEY_POWER, 0);
}

static int openfds(struct pollfd pfds[])
{
    int cnt = 0;
    const char *dirname = "/dev/input";
    struct dirent *de;
    DIR *dir;

    if ((dir = opendir(dirname))) {
        while ((cnt < MAX_POWERBTNS) && (de = readdir(dir))) {
            int fd;
            char name[PATH_MAX];
            if (de->d_name[0] != 'e') /* eventX */
                continue;
            snprintf(name, PATH_MAX, "%s/%s", dirname, de->d_name);
            fd = open(name, O_RDWR | O_NONBLOCK);
            if (fd < 0) {
                ALOGE("could not open %s, %s", name, strerror(errno));
                continue;
            }
            name[sizeof(name) - 1] = '\0';
            if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), &name) < 1) {
                ALOGE("could not get device name for %s, %s", name, strerror(errno));
                name[0] = '\0';
            }
            // TODO: parse /etc/excluded-input-devices.xml
            if (strcmp(name, "Power Button")) {
                close(fd);
                continue;
            }

            ALOGI("open %s(%s) ok fd=%d", de->d_name, name, fd);
            pfds[cnt].events = POLLIN;
            pfds[cnt++].fd = fd;
        }
        closedir(dir);
    }

    return cnt;
}

static void *powerbtnd_thread_func(void *arg __attribute__((unused)))
{
    int cnt, timeout, pollres;
    bool longpress = true;
    bool doubleclick = property_get_bool("poweroff.doubleclick", 0);
    struct pollfd pfds[MAX_POWERBTNS];

    timeout = -1;
    cnt = openfds(pfds);

    while (cnt > 0 && (pollres = poll(pfds, cnt, timeout)) >= 0) {
        ALOGV("pollres=%d %d\n", pollres, timeout);
        if (pollres == 0) {
            ALOGI("timeout, send one power key");
            send_key_power(uinput_fd, 0);
            timeout = -1;
            longpress = true;
            continue;
        }
        for (int i = 0; i < cnt; ++i) {
            if (pfds[i].revents & POLLIN) {
                struct input_event iev;
                size_t res = read(pfds[i].fd, &iev, sizeof(iev));
                if (res < sizeof(iev)) {
                    ALOGW("insufficient input data(%zd)? fd=%d", res, pfds[i].fd);
                    continue;
                }
                ALOGD("type=%d code=%d value=%d from fd=%d", iev.type, iev.code, iev.value, pfds[i].fd);
                if (iev.type == EV_KEY && iev.code == KEY_POWER && !iev.value) {
                    if (!doubleclick || timeout > 0) {
                        send_key_power(uinput_fd, longpress);
                        timeout = -1;
                    } else {
                        timeout = 1000; // one second
                    }
                } else if (iev.type == EV_SYN && iev.code == SYN_REPORT && iev.value) {
                    ALOGI("got a resuming event");
                    longpress = false;
                    timeout = 1000; // one second
                }
            }
        }
    }

    ALOGE_IF(cnt, "poll error: %s", strerror(errno));
    return NULL;
}

static void init_android_power_button()
{
    static pthread_t powerbtnd_thread;
    struct uinput_user_dev ud;

    if (uinput_fd >= 0) return;

    uinput_fd = open("/dev/uinput", O_WRONLY | O_NDELAY);
    if (uinput_fd < 0) {
        ALOGE("could not open uinput device: %s", strerror(errno));
        return;
    }

    memset(&ud, 0, sizeof(ud));
    strcpy(ud.name, "Android Power Button");
    write(uinput_fd, &ud, sizeof(ud));
    ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY);
    ioctl(uinput_fd, UI_SET_KEYBIT, KEY_POWER);
    ioctl(uinput_fd, UI_SET_KEYBIT, KEY_WAKEUP);
    ioctl(uinput_fd, UI_DEV_CREATE, 0);

    pthread_create(&powerbtnd_thread, NULL, powerbtnd_thread_func, NULL);
    pthread_setname_np(powerbtnd_thread, "powerbtnd");
}

static void *suspend_thread_func(void *arg __attribute__((unused)))
{
    char buf[80];
    char wakeup_count[20];
    int wakeup_count_len;
    int ret;
    bool success;

    while (1) {
        usleep(100000);
        ALOGV("%s: read wakeup_count\n", __func__);
        lseek(wakeup_count_fd, 0, SEEK_SET);
        wakeup_count_len = TEMP_FAILURE_RETRY(read(wakeup_count_fd, wakeup_count,
                sizeof(wakeup_count)));
        if (wakeup_count_len < 0) {
            strerror_r(errno, buf, sizeof(buf));
            ALOGE("Error reading from %s: %s\n", SYS_POWER_WAKEUP_COUNT, buf);
            wakeup_count_len = 0;
            continue;
        }
        if (!wakeup_count_len) {
            ALOGE("Empty wakeup count\n");
            continue;
        }

        ALOGV("%s: wait\n", __func__);
        ret = sem_wait(&suspend_lockout);
        if (ret < 0) {
            strerror_r(errno, buf, sizeof(buf));
            ALOGE("Error waiting on semaphore: %s\n", buf);
            continue;
        }

        success = true;
        ALOGV("%s: write %*s to wakeup_count\n", __func__, wakeup_count_len, wakeup_count);
        ret = TEMP_FAILURE_RETRY(write(wakeup_count_fd, wakeup_count, wakeup_count_len));
        if (ret < 0) {
            strerror_r(errno, buf, sizeof(buf));
            ALOGE("Error writing to %s: %s\n", SYS_POWER_WAKEUP_COUNT, buf);
        } else {
            const char *sleep_state = get_sleep_state();
            ALOGV("%s: write %s to %s\n", __func__, sleep_state, SYS_POWER_STATE);
            ret = TEMP_FAILURE_RETRY(write(state_fd, sleep_state, strlen(sleep_state)));
            if (ret < 0) {
                success = false;
            } else {
                send_key_wakeup(uinput_fd);
            }
            void (*func)(bool success) = wakeup_func;
            if (func != NULL) {
                (*func)(success);
            }
        }

        ALOGV("%s: release sem\n", __func__);
        ret = sem_post(&suspend_lockout);
        if (ret < 0) {
            strerror_r(errno, buf, sizeof(buf));
            ALOGE("Error releasing semaphore: %s\n", buf);
        }
    }
    return NULL;
}

static int autosuspend_wakeup_count_enable(void)
{
    char buf[80];
    int ret;

    ALOGV("autosuspend_wakeup_count_enable\n");

    ret = sem_post(&suspend_lockout);

    if (ret < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error changing semaphore: %s\n", buf);
    }

    ALOGV("autosuspend_wakeup_count_enable done\n");

    return ret;
}

static int autosuspend_wakeup_count_disable(void)
{
    char buf[80];
    int ret;

    ALOGV("autosuspend_wakeup_count_disable\n");

    ret = sem_wait(&suspend_lockout);

    if (ret < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error changing semaphore: %s\n", buf);
    }

    ALOGV("autosuspend_wakeup_count_disable done\n");

    return ret;
}

void set_wakeup_callback(void (*func)(bool success))
{
    if (wakeup_func != NULL) {
        ALOGE("Duplicate wakeup callback applied, keeping original");
        return;
    }
    wakeup_func = func;
}

struct autosuspend_ops autosuspend_wakeup_count_ops = {
        .enable = autosuspend_wakeup_count_enable,
        .disable = autosuspend_wakeup_count_disable,
};

struct autosuspend_ops *autosuspend_wakeup_count_init(void)
{
    int ret;
    char buf[80];

    init_android_power_button();

    state_fd = TEMP_FAILURE_RETRY(open(SYS_POWER_STATE, O_RDWR));
    if (state_fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", SYS_POWER_STATE, buf);
        goto err_open_state;
    }

    wakeup_count_fd = TEMP_FAILURE_RETRY(open(SYS_POWER_WAKEUP_COUNT, O_RDWR));
    if (wakeup_count_fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", SYS_POWER_WAKEUP_COUNT, buf);
        goto err_open_wakeup_count;
    }

    ret = sem_init(&suspend_lockout, 0, 0);
    if (ret < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error creating semaphore: %s\n", buf);
        goto err_sem_init;
    }
    ret = pthread_create(&suspend_thread, NULL, suspend_thread_func, NULL);
    if (ret) {
        strerror_r(ret, buf, sizeof(buf));
        ALOGE("Error creating thread: %s\n", buf);
        goto err_pthread_create;
    }

    ALOGI("Selected wakeup count\n");
    return &autosuspend_wakeup_count_ops;

err_pthread_create:
    sem_destroy(&suspend_lockout);
err_sem_init:
    close(wakeup_count_fd);
err_open_wakeup_count:
    close(state_fd);
err_open_state:
    return NULL;
}
