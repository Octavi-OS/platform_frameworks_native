/*
 * Copyright 2020 The Android Open Source Project
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

#undef LOG_TAG
#define LOG_TAG "GpuMemTracer"
#define ATRACE_TAG ATRACE_TAG_GRAPHICS

#include "tracing/GpuMemTracer.h"

#include <gpumem/GpuMem.h>
#include <perfetto/trace/android/gpu_mem_event.pbzero.h>
#include <unistd.h>
#include <utils/Timers.h>

#include <algorithm>
#include <thread>

PERFETTO_DEFINE_DATA_SOURCE_STATIC_MEMBERS(android::GpuMemTracer::GpuMemDataSource);

namespace android {

std::mutex GpuMemTracer::sTraceMutex;
std::condition_variable GpuMemTracer::sCondition;
bool GpuMemTracer::sTraceStarted;

void GpuMemTracer::initialize(std::shared_ptr<GpuMem> gpuMem) {
    if (!gpuMem->isInitialized()) {
        ALOGE("Cannot initialize GpuMemTracer before GpuMem");
        return;
    }
    mGpuMem = gpuMem;
    perfetto::TracingInitArgs args;
    args.backends = perfetto::kSystemBackend;
    perfetto::Tracing::Initialize(args);
    registerDataSource();
    std::thread tracerThread(&GpuMemTracer::threadLoop, this);
    pthread_setname_np(tracerThread.native_handle(), "GpuMemTracerThread");
    tracerThread.detach();
}

void GpuMemTracer::registerDataSource() {
    perfetto::DataSourceDescriptor dsd;
    dsd.set_name(kGpuMemDataSource);
    GpuMemDataSource::Register(dsd);
}

void GpuMemTracer::threadLoop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(GpuMemTracer::sTraceMutex);
            while (!sTraceStarted) {
                sCondition.wait(lock);
            }
        }
        traceInitialCounters();
        {
            std::lock_guard<std::mutex> lock(GpuMemTracer::sTraceMutex);
            sTraceStarted = false;
        }
    }
}

void GpuMemTracer::traceInitialCounters() {
    if (!mGpuMem->isInitialized()) {
        // This should never happen.
        ALOGE("Cannot trace without GpuMem initialization");
        return;
    }
    mGpuMem->traceGpuMemTotals([](uint32_t gpuId, uint32_t pid, uint64_t size) {
        GpuMemDataSource::Trace([&](GpuMemDataSource::TraceContext ctx) {
            auto packet = ctx.NewTracePacket();
            packet->set_timestamp(systemTime());
            auto* event = packet->set_gpu_mem_total_event();
            event->set_gpu_id(gpuId);
            event->set_pid(pid);
            event->set_size(size);
        });
    });
    // Flush the TraceContext. The last packet in the above loop will go
    // missing without this flush.
    GpuMemDataSource::Trace([](GpuMemDataSource::TraceContext ctx) { ctx.Flush(); });
}

} // namespace android