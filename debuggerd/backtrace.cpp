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

#define LOG_TAG "DEBUG"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/ptrace.h>

#include <memory>
#include <string>

#include <backtrace/Backtrace.h>

#include <log/log.h>

#include "backtrace.h"

#include "utility.h"

#define MAX_FRAMES_FROM_TOP_TO_CHECK 7
#define LIB_ART_NAME "libart"

static void dump_process_header(log_t* log, pid_t pid) {
  char path[PATH_MAX];
  char procnamebuf[1024];
  char* procname = NULL;
  FILE* fp;

  snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
  if ((fp = fopen(path, "r"))) {
    procname = fgets(procnamebuf, sizeof(procnamebuf), fp);
    fclose(fp);
  }

  time_t t = time(NULL);
  struct tm tm;
  localtime_r(&t, &tm);
  char timestr[64];
  strftime(timestr, sizeof(timestr), "%F %T", &tm);
  _LOG(log, logtype::BACKTRACE, "\n\n----- pid %d at %s -----\n", pid, timestr);

  if (procname) {
    _LOG(log, logtype::BACKTRACE, "Cmd line: %s\n", procname);
  }
  _LOG(log, logtype::BACKTRACE, "ABI: '%s'\n", ABI_STRING);
}

static void dump_process_footer(log_t* log, pid_t pid) {
  _LOG(log, logtype::BACKTRACE, "\n----- end %d -----\n", pid);
}

static void dump_thread(log_t* log, BacktraceMap* map, pid_t pid, pid_t tid) {
  char path[PATH_MAX];
  char threadnamebuf[1024];
  char* threadname = NULL;
  FILE* fp;

  snprintf(path, sizeof(path), "/proc/%d/comm", tid);
  if ((fp = fopen(path, "r"))) {
    threadname = fgets(threadnamebuf, sizeof(threadnamebuf), fp);
    fclose(fp);
    if (threadname) {
      size_t len = strlen(threadname);
      if (len && threadname[len - 1] == '\n') {
          threadname[len - 1] = '\0';
      }
    }
  }

  _LOG(log, logtype::BACKTRACE, "\n\"%s\" sysTid=%d\n", threadname ? threadname : "<unknown>", tid);

  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(pid, tid, map));
  if (backtrace->Unwind(0)) {
    dump_backtrace_to_log(backtrace.get(), log, "  ");
  } else {
    ALOGE("Unwind failed: tid = %d: %s", tid,
          backtrace->GetErrorString(backtrace->GetError()).c_str());
  }
}

void dump_backtrace(int fd, BacktraceMap* map, pid_t pid, pid_t tid,
                    const std::set<pid_t>& siblings, std::string* amfd_data) {
  log_t log;
  log.tfd = fd;
  log.amfd_data = amfd_data;

  dump_process_header(&log, pid);
  dump_thread(&log, map, pid, tid);

  for (pid_t sibling : siblings) {
    dump_thread(&log, map, pid, sibling);
  }

  dump_process_footer(&log, pid);
}

void check_coredump_criteria (std::string log, int frame_num) {
  ALOGI("coredump_criteria: checking backtrace-frame %d in victim thread, signal = %d",
      frame_num, coredump_signal);
  size_t len = log.length();
  for (size_t i = 0; i < len; i++ ) {
    log[i] = tolower(log[i]);
  }

  size_t found = log.find(LIB_ART_NAME);

  // Criterion 2: (Check tombstone.cpp for the fisrt one)
  // Of course, if numero uno frame has libart in it, dump core without much ado
  if ((found != std::string::npos) && (frame_num == 0)) {
    ALOGI("coredump_criteria: found %s in first frame, coredump-criteria met", LIB_ART_NAME);
    force_coredump_generation = true;
    return;
  }

  // Criterion 3:
  // If a frame(less than MAX_FRAMES_FROM_TOP_TO_CHECK) has libart in it,
  // and the accompanying signal is a SIGABRT, dump the core
  if ((found != std::string::npos) && (coredump_signal == SIGABRT)) {
    ALOGI("coredump_criteria: found %s and signal=SIGABRT, coredump-criteria met", LIB_ART_NAME);
    force_coredump_generation = true;
    return;
  }
  /* Append more lame rules above this line */
}

void dump_backtrace_to_log(Backtrace* backtrace, log_t* log, const char* prefix) {
  for (size_t i = 0; i < backtrace->NumFrames(); i++) {
    // Check for coredump-criteria only in userdebug/eng builds(if enabled), if:
    // 1) the current thread was the thread that originally got the fatal signal
    // 2) If no coredump_criteria has been met until now
    // 3) the current frame number is less than a defined(and customizable) upper-limit
    if (coredump_enabled && victim_thread &&
       (!force_coredump_generation) && (i < MAX_FRAMES_FROM_TOP_TO_CHECK)) {
      check_coredump_criteria(backtrace->FormatFrameData(i), i);
    }
    _LOG(log, logtype::BACKTRACE, "%s%s\n", prefix, backtrace->FormatFrameData(i).c_str());
  }
}
