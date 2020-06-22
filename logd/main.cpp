/*
 * Copyright (C) 2012-2013 The Android Open Source Project
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

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <poll.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/klog.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>

#include <memory>

#include <android-base/macros.h>
#include <cutils/android_get_control_file.h>
#include <cutils/properties.h>
#include <cutils/sockets.h>
#include <log/event_tag_map.h>
#include <packagelistparser/packagelistparser.h>
#include <private/android_filesystem_config.h>
#include <private/android_logger.h>
#include <processgroup/sched_policy.h>
#include <utils/threads.h>

#include "CommandListener.h"
#include "LogAudit.h"
#include "LogBuffer.h"
#include "LogKlog.h"
#include "LogListener.h"
#include "LogUtils.h"

#define KMSG_PRIORITY(PRI)                                 \
    '<', '0' + LOG_MAKEPRI(LOG_DAEMON, LOG_PRI(PRI)) / 10, \
        '0' + LOG_MAKEPRI(LOG_DAEMON, LOG_PRI(PRI)) % 10, '>'

// The service is designed to be run by init, it does not respond well to starting up manually. Init
// has a 'sigstop' feature that sends SIGSTOP to a service immediately before calling exec().  This
// allows debuggers, etc to be attached to logd at the very beginning, while still having init
// handle the user, groups, capabilities, files, etc setup.
static int DropPrivs(bool klogd, bool auditd) {
    if (set_sched_policy(0, SP_BACKGROUND) < 0) {
        android::prdebug("failed to set background scheduling policy");
        return -1;
    }

    sched_param param = {};
    if (sched_setscheduler((pid_t)0, SCHED_BATCH, &param) < 0) {
        android::prdebug("failed to set batch scheduler");
        return -1;
    }

    if (!__android_logger_property_get_bool("ro.debuggable",
                                            BOOL_DEFAULT_FALSE) &&
        prctl(PR_SET_DUMPABLE, 0) == -1) {
        android::prdebug("failed to clear PR_SET_DUMPABLE");
        return -1;
    }

    std::unique_ptr<struct _cap_struct, int (*)(void*)> caps(cap_init(), cap_free);
    if (cap_clear(caps.get()) < 0) {
        android::prdebug("cap_clear() failed");
        return -1;
    }
    if (klogd) {
        cap_value_t cap_syslog = CAP_SYSLOG;
        if (cap_set_flag(caps.get(), CAP_PERMITTED, 1, &cap_syslog, CAP_SET) < 0 ||
            cap_set_flag(caps.get(), CAP_EFFECTIVE, 1, &cap_syslog, CAP_SET) < 0) {
            android::prdebug("Failed to set CAP_SYSLOG");
            return -1;
        }
    }
    if (auditd) {
        cap_value_t cap_audit_control = CAP_AUDIT_CONTROL;
        if (cap_set_flag(caps.get(), CAP_PERMITTED, 1, &cap_audit_control, CAP_SET) < 0 ||
            cap_set_flag(caps.get(), CAP_EFFECTIVE, 1, &cap_audit_control, CAP_SET) < 0) {
            android::prdebug("Failed to set CAP_AUDIT_CONTROL");
            return -1;
        }
    }
    if (cap_set_proc(caps.get()) < 0) {
        android::prdebug("failed to set CAP_SYSLOG or CAP_AUDIT_CONTROL (%d)", errno);
        return -1;
    }

    return 0;
}

// Property helper
static bool check_flag(const char* prop, const char* flag) {
    const char* cp = strcasestr(prop, flag);
    if (!cp) {
        return false;
    }
    // We only will document comma (,)
    static const char sep[] = ",:;|+ \t\f";
    if ((cp != prop) && !strchr(sep, cp[-1])) {
        return false;
    }
    cp += strlen(flag);
    return !*cp || !!strchr(sep, *cp);
}

static int fdDmesg = -1;
void android::prdebug(const char* fmt, ...) {
    if (fdDmesg < 0) {
        return;
    }

    static const char message[] = {
        KMSG_PRIORITY(LOG_DEBUG), 'l', 'o', 'g', 'd', ':', ' '
    };
    char buffer[256];
    memcpy(buffer, message, sizeof(message));

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buffer + sizeof(message),
                      sizeof(buffer) - sizeof(message), fmt, ap);
    va_end(ap);
    if (n > 0) {
        buffer[sizeof(buffer) - 1] = '\0';
        if (!strchr(buffer, '\n')) {
            buffer[sizeof(buffer) - 2] = '\0';
            strlcat(buffer, "\n", sizeof(buffer));
        }
        write(fdDmesg, buffer, strlen(buffer));
    }
}

static sem_t reinit;
static bool reinit_running = false;
static LogBuffer* logBuf = nullptr;

static void* reinit_thread_start(void* /*obj*/) {
    prctl(PR_SET_NAME, "logd.daemon");

    while (reinit_running && !sem_wait(&reinit) && reinit_running) {
        if (fdDmesg >= 0) {
            static const char reinit_message[] = { KMSG_PRIORITY(LOG_INFO),
                                                   'l',
                                                   'o',
                                                   'g',
                                                   'd',
                                                   '.',
                                                   'd',
                                                   'a',
                                                   'e',
                                                   'm',
                                                   'o',
                                                   'n',
                                                   ':',
                                                   ' ',
                                                   'r',
                                                   'e',
                                                   'i',
                                                   'n',
                                                   'i',
                                                   't',
                                                   '\n' };
            write(fdDmesg, reinit_message, sizeof(reinit_message));
        }

        // Anything that reads persist.<property>
        if (logBuf) {
            logBuf->init();
            logBuf->initPrune(nullptr);
        }
        android::ReReadEventLogTags();
    }

    return nullptr;
}

char* android::uidToName(uid_t u) {
    struct Userdata {
        uid_t uid;
        char* name;
    } userdata = {
            .uid = u,
            .name = nullptr,
    };

    packagelist_parse(
            [](pkg_info* info, void* callback_parameter) {
                auto userdata = reinterpret_cast<Userdata*>(callback_parameter);
                bool result = true;
                if (info->uid == userdata->uid) {
                    userdata->name = strdup(info->name);
                    // false to stop processing
                    result = false;
                }
                packagelist_free(info);
                return result;
            },
            &userdata);

    return userdata.name;
}

// Serves as a global method to trigger reinitialization
// and as a function that can be provided to signal().
void reinit_signal_handler(int /*signal*/) {
    sem_post(&reinit);
}

static void readDmesg(LogAudit* al, LogKlog* kl) {
    if (!al && !kl) {
        return;
    }

    int rc = klogctl(KLOG_SIZE_BUFFER, nullptr, 0);
    if (rc <= 0) {
        return;
    }

    // Margin for additional input race or trailing nul
    ssize_t len = rc + 1024;
    std::unique_ptr<char[]> buf(new char[len]);

    rc = klogctl(KLOG_READ_ALL, buf.get(), len);
    if (rc <= 0) {
        return;
    }

    if (rc < len) {
        len = rc + 1;
    }
    buf[--len] = '\0';

    if (kl && kl->isMonotonic()) {
        kl->synchronize(buf.get(), len);
    }

    ssize_t sublen;
    for (char *ptr = nullptr, *tok = buf.get();
         (rc >= 0) && !!(tok = android::log_strntok_r(tok, len, ptr, sublen));
         tok = nullptr) {
        if ((sublen <= 0) || !*tok) continue;
        if (al) {
            rc = al->log(tok, sublen);
        }
        if (kl) {
            rc = kl->log(tok, sublen);
        }
    }
}

static int issueReinit() {
    int sock = TEMP_FAILURE_RETRY(socket_local_client(
        "logd", ANDROID_SOCKET_NAMESPACE_RESERVED, SOCK_STREAM));
    if (sock < 0) return -errno;

    static const char reinitStr[] = "reinit";
    ssize_t ret = TEMP_FAILURE_RETRY(write(sock, reinitStr, sizeof(reinitStr)));
    if (ret < 0) return -errno;

    struct pollfd p;
    memset(&p, 0, sizeof(p));
    p.fd = sock;
    p.events = POLLIN;
    ret = TEMP_FAILURE_RETRY(poll(&p, 1, 1000));
    if (ret < 0) return -errno;
    if ((ret == 0) || !(p.revents & POLLIN)) return -ETIME;

    static const char success[] = "success";
    char buffer[sizeof(success) - 1];
    memset(buffer, 0, sizeof(buffer));
    ret = TEMP_FAILURE_RETRY(read(sock, buffer, sizeof(buffer)));
    if (ret < 0) return -errno;

    return strncmp(buffer, success, sizeof(success) - 1) != 0;
}

// Foreground waits for exit of the main persistent threads
// that are started here. The threads are created to manage
// UNIX domain client sockets for writing, reading and
// controlling the user space logger, and for any additional
// logging plugins like auditd and restart control. Additional
// transitory per-client threads are created for each reader.
int main(int argc, char* argv[]) {
    // logd is written under the assumption that the timezone is UTC.
    // If TZ is not set, persist.sys.timezone is looked up in some time utility
    // libc functions, including mktime. It confuses the logd time handling,
    // so here explicitly set TZ to UTC, which overrides the property.
    setenv("TZ", "UTC", 1);
    // issue reinit command. KISS argument parsing.
    if ((argc > 1) && argv[1] && !strcmp(argv[1], "--reinit")) {
        return issueReinit();
    }

    static const char dev_kmsg[] = "/dev/kmsg";
    fdDmesg = android_get_control_file(dev_kmsg);
    if (fdDmesg < 0) {
        fdDmesg = TEMP_FAILURE_RETRY(open(dev_kmsg, O_WRONLY | O_CLOEXEC));
    }

    int fdPmesg = -1;
    bool klogd = __android_logger_property_get_bool(
        "ro.logd.kernel",
        BOOL_DEFAULT_TRUE | BOOL_DEFAULT_FLAG_ENG | BOOL_DEFAULT_FLAG_SVELTE);
    if (klogd) {
        static const char proc_kmsg[] = "/proc/kmsg";
        fdPmesg = android_get_control_file(proc_kmsg);
        if (fdPmesg < 0) {
            fdPmesg = TEMP_FAILURE_RETRY(
                open(proc_kmsg, O_RDONLY | O_NDELAY | O_CLOEXEC));
        }
        if (fdPmesg < 0) android::prdebug("Failed to open %s\n", proc_kmsg);
    }

    bool auditd = __android_logger_property_get_bool("ro.logd.auditd", BOOL_DEFAULT_TRUE);
    if (DropPrivs(klogd, auditd) != 0) {
        return EXIT_FAILURE;
    }

    // Reinit Thread
    sem_init(&reinit, 0, 0);
    pthread_attr_t attr;
    if (!pthread_attr_init(&attr)) {
        struct sched_param param;

        memset(&param, 0, sizeof(param));
        pthread_attr_setschedparam(&attr, &param);
        pthread_attr_setschedpolicy(&attr, SCHED_BATCH);
        if (!pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED)) {
            pthread_t thread;
            reinit_running = true;
            if (pthread_create(&thread, &attr, reinit_thread_start, nullptr)) {
                reinit_running = false;
            }
        }
        pthread_attr_destroy(&attr);
    }

    // Serves the purpose of managing the last logs times read on a
    // socket connection, and as a reader lock on a range of log
    // entries.

    LastLogTimes* times = new LastLogTimes();

    // LogBuffer is the object which is responsible for holding all
    // log entries.

    logBuf = new LogBuffer(times);

    signal(SIGHUP, reinit_signal_handler);

    if (__android_logger_property_get_bool(
            "logd.statistics", BOOL_DEFAULT_TRUE | BOOL_DEFAULT_FLAG_PERSIST |
                                   BOOL_DEFAULT_FLAG_ENG |
                                   BOOL_DEFAULT_FLAG_SVELTE)) {
        logBuf->enableStatistics();
    }

    // LogReader listens on /dev/socket/logdr. When a client
    // connects, log entries in the LogBuffer are written to the client.

    LogReader* reader = new LogReader(logBuf);
    if (reader->startListener()) {
        return EXIT_FAILURE;
    }

    // LogListener listens on /dev/socket/logdw for client
    // initiated log messages. New log entries are added to LogBuffer
    // and LogReader is notified to send updates to connected clients.

    LogListener* swl = new LogListener(logBuf, reader);
    // Backlog and /proc/sys/net/unix/max_dgram_qlen set to large value
    if (swl->startListener(600)) {
        return EXIT_FAILURE;
    }

    // Command listener listens on /dev/socket/logd for incoming logd
    // administrative commands.

    CommandListener* cl = new CommandListener(logBuf, reader, swl);
    if (cl->startListener()) {
        return EXIT_FAILURE;
    }

    // LogAudit listens on NETLINK_AUDIT socket for selinux
    // initiated log messages. New log entries are added to LogBuffer
    // and LogReader is notified to send updates to connected clients.

    LogAudit* al = nullptr;
    if (auditd) {
        al = new LogAudit(logBuf, reader,
                          __android_logger_property_get_bool(
                              "ro.logd.auditd.dmesg", BOOL_DEFAULT_TRUE)
                              ? fdDmesg
                              : -1);
    }

    LogKlog* kl = nullptr;
    if (klogd) {
        kl = new LogKlog(logBuf, reader, fdDmesg, fdPmesg, al != nullptr);
    }

    readDmesg(al, kl);

    // failure is an option ... messages are in dmesg (required by standard)

    if (kl && kl->startListener()) {
        delete kl;
    }

    if (al && al->startListener()) {
        delete al;
    }

    TEMP_FAILURE_RETRY(pause());

    return EXIT_SUCCESS;
}
