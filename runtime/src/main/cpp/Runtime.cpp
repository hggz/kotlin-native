/*
 * Copyright 2010-2017 JetBrains s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "Alloc.h"
#include "Atomic.h"
#include "Exceptions.h"
#include "Memory.h"
#include "Porting.h"
#include "Runtime.h"

// For RuntimeState definition.
#include "MemoryPrivate.hpp"

typedef void (*Initializer)(int initialize);
struct InitNode {
  Initializer init;
  InitNode* next;
};

namespace {

InitNode* initHeadNode = nullptr;
InitNode* initTailNode = nullptr;

enum {
  INIT_GLOBALS = 0,
  INIT_THREAD_LOCAL_GLOBALS = 1,
  DEINIT_THREAD_LOCAL_GLOBALS = 2,
  DEINIT_GLOBALS = 3
};

enum {
  SUSPENDED = 0,
  RUNNING,
  DESTROYING
};

bool updateStatusIf(RuntimeState* state, int oldStatus, int newStatus) {
#if KONAN_NO_THREADS
    if (state->executionStatus == oldStatus) {
        state->executionStatus = newStatus;
        return true;
    }
    return false;
#else
    return __sync_bool_compare_and_swap(&state->executionStatus, oldStatus, newStatus);
#endif
}

void InitOrDeinitGlobalVariables(int initialize) {
  InitNode *currNode = initHeadNode;
  while (currNode != nullptr) {
    currNode->init(initialize);
    currNode = currNode->next;
  }
}

THREAD_LOCAL_VARIABLE RuntimeState* runtimeState = nullptr;
THREAD_LOCAL_VARIABLE int isMainThread = 0;

int aliveRuntimesCount = 0;
int32_t runtimesLock = 0;
RuntimeState* runtimeStateList = nullptr;

void threadInterruptHandler(int ignore) {
    auto* state = runtimeState;
    // TODO: an ugly hack - as TLS variables cannot be accessed from the signal handler we search
    //  in the runtime list for the current thread id.
    if (state == nullptr && runtimesLock != 0) {
        long threadId = konan::currentThread();
        RuntimeState* current = runtimeStateList;
        while (current != nullptr) {
            if (current->threadId == threadId) {
                state = current;
                break;
            }
            current = current->next;
        }
    }
    if (state != nullptr && state->handler != nullptr) {
        state->handler(state);
    }
}

RuntimeState* initRuntime() {
  SetKonanTerminateHandler();
  RuntimeState* result = konanConstructInstance<RuntimeState>();
  if (!result) return nullptr;
  result->threadId = konan::currentThread();
  result->handler = nullptr;
  result->memoryState = InitMemory();
  bool firstRuntime = atomicAdd(&aliveRuntimesCount, 1) == 1;
  Kotlin_lockRuntimes();
  result->next = runtimeStateList;
  runtimeStateList = result;
  Kotlin_unlockRuntimes();
  // Keep global variables in state as well.
  if (firstRuntime) {
    konan::setThreadInterruptHandler(threadInterruptHandler);
    isMainThread = 1;
    konan::consoleInit();
    InitOrDeinitGlobalVariables(INIT_GLOBALS);
  }
  InitOrDeinitGlobalVariables(INIT_THREAD_LOCAL_GLOBALS);
  return result;
}

void deinitRuntime(RuntimeState* state) {
  bool lastRuntime = atomicAdd(&aliveRuntimesCount, -1) == 0;
  InitOrDeinitGlobalVariables(DEINIT_THREAD_LOCAL_GLOBALS);
  if (lastRuntime)
    InitOrDeinitGlobalVariables(DEINIT_GLOBALS);
  Kotlin_lockRuntimes();
  RuntimeState* current = runtimeStateList;
  RuntimeState* previous = nullptr;
  while (current != nullptr) {
      if (current == state) {
        if (previous != nullptr)
          previous->next = current->next;
        else
          runtimeStateList = current->next;
        break;
      }
      previous = current;
      current = current->next;
  }
  Kotlin_unlockRuntimes();
  DeinitMemory(state->memoryState);
  konanDestructInstance(state);
}

}  // namespace

extern "C" {

void AppendToInitializersTail(InitNode *next) {
  // TODO: use RuntimeState.
  if (initHeadNode == nullptr) {
    initHeadNode = next;
  } else {
    initTailNode->next = next;
  }
  initTailNode = next;
}

void Kotlin_lockRuntimes() {
  spinLock(&runtimesLock);
}

void Kotlin_unlockRuntimes() {
  spinUnlock(&runtimesLock);
}

void Kotlin_iterateRuntimes(bool (*operation)(RuntimeState*, void*), void* argument) {
  RuntimeCheck(runtimesLock == 1, "Lock must be taken");
  auto* current = runtimeStateList;
  while (current != nullptr) {
      if (operation(current, argument)) break;
      current = current->next;
  }
}

void Kotlin_initRuntimeIfNeeded() {
  if (runtimeState == nullptr) {
    runtimeState = initRuntime();
    RuntimeCheck(updateStatusIf(runtimeState, SUSPENDED, RUNNING), "Cannot transition state to RUNNING for init");
    // Register runtime deinit function at thread cleanup.
    konan::onThreadExit(Kotlin_deinitRuntimeIfNeeded);
  }
}

void Kotlin_deinitRuntimeIfNeeded() {
  if (runtimeState != nullptr) {
     RuntimeCheck(updateStatusIf(runtimeState, RUNNING, DESTROYING), "Cannot transition state to DESTROYING");
     deinitRuntime(runtimeState);
     runtimeState = nullptr;
  }
}

RuntimeState* Kotlin_createRuntime() {
  return initRuntime();
}

void Kotlin_destroyRuntime(RuntimeState* state) {
 RuntimeCheck(updateStatusIf(state, SUSPENDED, DESTROYING), "Cannot transition state to DESTROYING");
 deinitRuntime(state);
}

RuntimeState* Kotlin_suspendRuntime() {
    RuntimeCheck(::runtimeState != nullptr, "Runtime must be active on the current thread");
    auto result = ::runtimeState;
    RuntimeCheck(updateStatusIf(result, RUNNING, SUSPENDED), "Cannot transition state to SUSPENDED for suspend");
    result->memoryState = SuspendMemory();
    ::runtimeState = nullptr;
    return result;
}

void Kotlin_resumeRuntime(RuntimeState* state) {
    RuntimeCheck(::runtimeState == nullptr, "Runtime must not be active on the current thread");
    RuntimeCheck(updateStatusIf(state, SUSPENDED, RUNNING), "Cannot transition state to RUNNING for resume");
    ::runtimeState = state;
    ResumeMemory(state->memoryState);
}

RuntimeState* RUNTIME_USED Kotlin_getRuntime() {
  RuntimeCheck(::runtimeState != nullptr, "Runtime must be active on the current thread");
  return ::runtimeState;
}

void CheckIsMainThread() {
  if (!isMainThread)
    ThrowIncorrectDereferenceException();
}

int Konan_Platform_canAccessUnaligned() {
#if KONAN_NO_UNALIGNED_ACCESS
  return 0;
#else
  return 1;
#endif
}

int Konan_Platform_isLittleEndian() {
#ifdef __BIG_ENDIAN__
  return 0;
#else
  return 1;
#endif
}

int Konan_Platform_getOsFamily() {
#if KONAN_MACOSX
  return 1;
#elif KONAN_IOS
  return 2;
#elif KONAN_LINUX
  return 3;
#elif KONAN_WINDOWS
  return 4;
#elif KONAN_ANDROID
  return 5;
#elif KONAN_WASM
  return 6;
#else
#warning "Unknown platform"
  return 0;
#endif
}

int Konan_Platform_getCpuArchitecture() {
#if KONAN_ARM32
  return 1;
#elif KONAN_ARM64
  return 2;
#elif KONAN_X86
  return 3;
#elif KONAN_X64
  return 4;
#elif KONAN_MIPS
  return 5;
#elif KONAN_MIPSEL
  return 6;
#elif KONAN_WASM
  return 7;
#else
#warning "Unknown CPU"
  return 0;
#endif
}

}  // extern "C"
