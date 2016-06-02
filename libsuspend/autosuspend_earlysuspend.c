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
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "libsuspend"
#include <cutils/log.h>

#include "autosuspend_ops.h"

#define EARLYSUSPEND_WAIT_FOR_FB_SLEEP "/sys/power/wait_for_fb_sleep"
#define EARLYSUSPEND_WAIT_FOR_FB_WAKE "/sys/power/wait_for_fb_wake"

static int sPowerStatefd;
static const char *pwr_state_on = "on";
static pthread_t earlysuspend_thread;
static pthread_mutex_t earlysuspend_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t earlysuspend_cond = PTHREAD_COND_INITIALIZER;
static bool wait_for_earlysuspend;
static enum {
    EARLYSUSPEND_ON,
    EARLYSUSPEND_MEM,
} earlysuspend_state = EARLYSUSPEND_ON;

static void log_err(const char *fmt, ...)
{
    char err[80];
    char buf[512];

    strerror_r(errno, err, sizeof(err));

    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ALOGE("Error %s: %s", buf, err);
}

static int open_file(const char *file, char *buf, size_t sz, int *fd)
{
    int err;
    int f = TEMP_FAILURE_RETRY(open(file, fd ? O_RDWR : O_RDONLY, 0));
    // if the file doesn't exist, the error will be caught in read() below
    err = TEMP_FAILURE_RETRY(read(f, buf, sz));

    if (err < 0) {
        log_err("opening %s", file);
    } else {
        err = 0;
    }

    if (fd == NULL) {
        close(f);
    } else {
        *fd = f;
    }
    return err;
}

static int wait_for_fb_wake(void)
{
    char buf;
    return open_file(EARLYSUSPEND_WAIT_FOR_FB_WAKE, &buf, 1, NULL);
}

static int wait_for_fb_sleep(void)
{
    char buf;
    return open_file(EARLYSUSPEND_WAIT_FOR_FB_SLEEP, &buf, 1, NULL);
}

static void *earlysuspend_thread_func(void __unused *arg)
{
    while (1) {
        if (wait_for_fb_sleep()) {
            ALOGE("Failed reading wait_for_fb_sleep, exiting earlysuspend thread");
            return NULL;
        }
        pthread_mutex_lock(&earlysuspend_mutex);
        earlysuspend_state = EARLYSUSPEND_MEM;
        pthread_cond_signal(&earlysuspend_cond);
        pthread_mutex_unlock(&earlysuspend_mutex);

        if (wait_for_fb_wake()) {
            ALOGE("Failed reading wait_for_fb_wake, exiting earlysuspend thread");
            return NULL;
        }
        pthread_mutex_lock(&earlysuspend_mutex);
        earlysuspend_state = EARLYSUSPEND_ON;
        pthread_cond_signal(&earlysuspend_cond);
        pthread_mutex_unlock(&earlysuspend_mutex);
    }
}
static int autosuspend_earlysuspend_enable(void)
{
    int ret;
    const char *sleep_state = get_sleep_state();

    ALOGI("autosuspend_earlysuspend_enable");

    ret = TEMP_FAILURE_RETRY(write(sPowerStatefd, sleep_state, strlen(sleep_state)));
    if (ret < 0) {
        log_err("writing %s to %s", sleep_state, SYS_POWER_STATE);
        return ret;
    }

    if (wait_for_earlysuspend) {
        pthread_mutex_lock(&earlysuspend_mutex);
        while (earlysuspend_state != EARLYSUSPEND_MEM) {
            pthread_cond_wait(&earlysuspend_cond, &earlysuspend_mutex);
        }
        pthread_mutex_unlock(&earlysuspend_mutex);
    }

    ALOGD("autosuspend_earlysuspend_enable done");

    return 0;
}

static int autosuspend_earlysuspend_disable(void)
{
    int ret;

    ALOGI("autosuspend_earlysuspend_disable");

    ret = TEMP_FAILURE_RETRY(write(sPowerStatefd, pwr_state_on, strlen(pwr_state_on)));
    if (ret < 0) {
#if DEBUG
        log_err("writing %s to %s", pwr_state_on, SYS_POWER_STATE);
#endif
    }

    if (wait_for_earlysuspend) {
        pthread_mutex_lock(&earlysuspend_mutex);
        while (earlysuspend_state != EARLYSUSPEND_ON) {
            pthread_cond_wait(&earlysuspend_cond, &earlysuspend_mutex);
        }
        pthread_mutex_unlock(&earlysuspend_mutex);
    }

    ALOGD("autosuspend_earlysuspend_disable done");

    return 0;
}

struct autosuspend_ops autosuspend_earlysuspend_ops = {
        .enable = autosuspend_earlysuspend_enable,
        .disable = autosuspend_earlysuspend_disable,
};

void start_earlysuspend_thread(void)
{
    int ret;

    ret = access(EARLYSUSPEND_WAIT_FOR_FB_SLEEP, F_OK);
    if (ret < 0) {
        log_err("accessing %s", EARLYSUSPEND_WAIT_FOR_FB_SLEEP);
        return;
    }

    ret = access(EARLYSUSPEND_WAIT_FOR_FB_WAKE, F_OK);
    if (ret < 0) {
        log_err("accessing %s", EARLYSUSPEND_WAIT_FOR_FB_WAKE);
        return;
    }

    wait_for_fb_wake();

    ALOGI("Starting early suspend unblocker thread");
    ret = pthread_create(&earlysuspend_thread, NULL, earlysuspend_thread_func, NULL);
    if (ret) {
        log_err("creating thread");
        return;
    }

    wait_for_earlysuspend = true;
}

struct autosuspend_ops *autosuspend_earlysuspend_init(void)
{
    int ret;
    char pwr_st[128];

    ret = open_file(SYS_POWER_STATE, pwr_st, sizeof(pwr_st), &sPowerStatefd);
    if (ret < 0) {
        close(sPowerStatefd);
        return NULL;
    }

    ALOGI("Selected early suspend");

    start_earlysuspend_thread();

    return &autosuspend_earlysuspend_ops;
}
