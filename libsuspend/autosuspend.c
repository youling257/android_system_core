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
#include <stdbool.h>
#include <string.h>

#define LOG_TAG "libsuspend"
#include <cutils/log.h>
#include <cutils/properties.h>

#include <suspend/autosuspend.h>

#include "autosuspend_ops.h"

static const char *default_sleep_state = "mem";
static const char *fallback_sleep_state = "freeze";

static struct autosuspend_ops *autosuspend_ops;
static bool autosuspend_enabled;
static bool autosuspend_inited;

static int autosuspend_init(void)
{
    char buf[PROPERTY_VALUE_MAX];

    if (autosuspend_inited) {
        return 0;
    }

    property_get("sleep.earlysuspend", buf, "1");
    if (buf[0] == '1') {
        autosuspend_ops = autosuspend_earlysuspend_init();
    }
    if (autosuspend_ops) {
        goto out;
    }

/* Remove autosleep so userspace can manager suspend/resume and keep stats */
#if 0
    autosuspend_ops = autosuspend_autosleep_init();
    if (autosuspend_ops) {
        goto out;
    }
#endif

    autosuspend_ops = autosuspend_wakeup_count_init();
    if (autosuspend_ops) {
        goto out;
    }

    if (!autosuspend_ops) {
        ALOGE("failed to initialize autosuspend\n");
        return -1;
    }

out:
    autosuspend_inited = true;

    ALOGV("autosuspend initialized\n");
    return 0;
}

int autosuspend_enable(void)
{
    int ret;

    ret = autosuspend_init();
    if (ret) {
        return ret;
    }

    ALOGV("autosuspend_enable\n");

    if (autosuspend_enabled) {
        return 0;
    }

    ret = autosuspend_ops->enable();
    if (ret) {
        return ret;
    }

    autosuspend_enabled = true;
    return 0;
}

int autosuspend_disable(void)
{
    int ret;

    ret = autosuspend_init();
    if (ret) {
        return ret;
    }

    ALOGV("autosuspend_disable\n");

    if (!autosuspend_enabled) {
        return 0;
    }

    ret = autosuspend_ops->disable();
    if (ret) {
        return ret;
    }

    autosuspend_enabled = false;
    return 0;
}

static bool sleep_state_available(const char *state)
{
    char buf[64];
    int fd = TEMP_FAILURE_RETRY(open(SYS_POWER_STATE, O_RDONLY));
    if (fd < 0) {
        ALOGE("Error reading power state: %s", SYS_POWER_STATE);
        return false;
    }
    TEMP_FAILURE_RETRY(read(fd, buf, 64));
    close(fd);
    return !!strstr(buf, state);
}

const char *get_sleep_state()
{
    static char sleep_state[PROPERTY_VALUE_MAX] = "";

    if (!sleep_state[0]) {
        if (property_get("sleep.state", sleep_state, NULL) > 0) {
            ALOGD("autosuspend using sleep.state property (%s)", sleep_state);
        } else if (sleep_state_available(default_sleep_state)) {
            ALOGD("autosuspend using default sleep_state (%s)", default_sleep_state);
            strncpy(sleep_state, default_sleep_state, PROPERTY_VALUE_MAX);
        } else {
            ALOGW("autosuspend \"%s\" unavailable, using fallback sleep.state (%s)", default_sleep_state, fallback_sleep_state);
            strncpy(sleep_state, fallback_sleep_state, PROPERTY_VALUE_MAX);
        }
    }
    return sleep_state;
}
