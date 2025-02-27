// Copyright 2012 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/heap/incremental-marking-job.h"

#include "src/base/platform/mutex.h"
#include "src/base/platform/time.h"
#include "src/execution/isolate.h"
#include "src/execution/vm-state-inl.h"
#include "src/flags/flags.h"
#include "src/heap/base/incremental-marking-schedule.h"
#include "src/heap/cppgc-js/cpp-heap.h"
#include "src/heap/cppgc/marker.h"
#include "src/heap/gc-tracer.h"
#include "src/heap/heap-inl.h"
#include "src/heap/heap.h"
#include "src/heap/incremental-marking.h"
#include "src/heap/minor-gc-job.h"
#include "src/init/v8.h"
#include "src/tasks/cancelable-task.h"

namespace v8::internal {

class IncrementalMarkingJob::Task final : public CancelableTask {
 public:
  Task(Isolate* isolate, IncrementalMarkingJob* job, StackState stack_state)
      : CancelableTask(isolate),
        isolate_(isolate),
        job_(job),
        stack_state_(stack_state) {}

  // CancelableTask overrides.
  void RunInternal() override;

  Isolate* isolate() const { return isolate_; }

 private:
  Isolate* const isolate_;
  IncrementalMarkingJob* const job_;
  const StackState stack_state_;
};

IncrementalMarkingJob::IncrementalMarkingJob(Heap* heap)
    : heap_(heap),
      foreground_task_runner_(V8::GetCurrentPlatform()->GetForegroundTaskRunner(
          reinterpret_cast<v8::Isolate*>(heap->isolate()))) {
  CHECK(v8_flags.incremental_marking_task);
}

void IncrementalMarkingJob::ScheduleTask(TaskType task_type) {
  base::MutexGuard guard(&mutex_);

  if (pending_task_.has_value() || heap_->IsTearingDown()) {
    return;
  }

  const bool non_nestable_tasks_enabled =
      foreground_task_runner_->NonNestableTasksEnabled();
  auto task = std::make_unique<Task>(heap_->isolate(), this,
                                     non_nestable_tasks_enabled
                                         ? StackState::kNoHeapPointers
                                         : StackState::kMayContainHeapPointers);
  if (non_nestable_tasks_enabled) {
    if (task_type == TaskType::kNormal) {
      foreground_task_runner_->PostNonNestableTask(std::move(task));
    } else {
      foreground_task_runner_->PostNonNestableDelayedTask(
          std::move(task), v8::base::TimeDelta::FromMilliseconds(
                               v8_flags.incremental_marking_task_delay_ms)
                               .InSecondsF());
    }
  } else {
    if (task_type == TaskType::kNormal) {
      foreground_task_runner_->PostTask(std::move(task));
    } else {
      foreground_task_runner_->PostDelayedTask(
          std::move(task), v8::base::TimeDelta::FromMilliseconds(
                               v8_flags.incremental_marking_task_delay_ms)
                               .InSecondsF());
    }
  }

  pending_task_.emplace(task_type);
  scheduled_time_ = v8::base::TimeTicks::Now();
  if (V8_UNLIKELY(v8_flags.trace_incremental_marking)) {
    heap_->isolate()->PrintWithTimestamp(
        "[IncrementalMarking] Job: Schedule (%s)\n", ToString(task_type));
  }
}

void IncrementalMarkingJob::Task::RunInternal() {
  VMState<GC> state(isolate());
  TRACE_EVENT_CALL_STATS_SCOPED(isolate(), "v8", "V8.Task");

  isolate()->stack_guard()->ClearStartIncrementalMarking();

  Heap* heap = isolate()->heap();

  {
    base::MutexGuard guard(&job_->mutex_);
    heap->tracer()->RecordTimeToIncrementalMarkingTask(
        (v8::base::TimeTicks::Now() - job_->scheduled_time_).InMillisecondsF());
    job_->scheduled_time_ = v8::base::TimeTicks();
  }

  EmbedderStackStateScope scope(
      heap, EmbedderStackStateScope::kImplicitThroughTask, stack_state_);

  IncrementalMarking* incremental_marking = heap->incremental_marking();
  if (incremental_marking->IsStopped()) {
    if (heap->IncrementalMarkingLimitReached() !=
        Heap::IncrementalMarkingLimit::kNoLimit) {
      heap->StartIncrementalMarking(heap->GCFlagsForIncrementalMarking(),
                                    GarbageCollectionReason::kTask,
                                    kGCCallbackScheduleIdleGarbageCollection);
    } else if (v8_flags.minor_ms && v8_flags.concurrent_minor_ms_marking) {
      heap->StartMinorMSIncrementalMarkingIfPossible();
    }
  }

  // Clear this flag after StartIncrementalMarking() call to avoid scheduling a
  // new task when starting incremental marking from a task.
  {
    base::MutexGuard guard(&job_->mutex_);
    if (V8_UNLIKELY(v8_flags.trace_incremental_marking)) {
      job_->heap_->isolate()->PrintWithTimestamp(
          "[IncrementalMarking] Job: Run (%s)\n",
          ToString(job_->pending_task_.value()));
    }
    job_->pending_task_.reset();
  }

  if (incremental_marking->IsMajorMarking()) {
    heap->incremental_marking()->AdvanceAndFinalizeIfComplete();
    if (incremental_marking->IsMajorMarking()) {
      TaskType task_type;
      if (v8_flags.incremental_marking_task_delay_ms > 0) {
        task_type = heap->incremental_marking()->IsAheadOfSchedule()
                        ? TaskType::kPending
                        : TaskType::kNormal;
      } else {
        task_type = TaskType::kNormal;
        if (V8_UNLIKELY(v8_flags.trace_incremental_marking)) {
          isolate()->PrintWithTimestamp(
              "[IncrementalMarking] Using regular task based on flags\n");
        }
      }
      job_->ScheduleTask(task_type);
    }
  }
}

base::Optional<v8::base::TimeDelta> IncrementalMarkingJob::AverageTimeToTask()
    const {
  const double recorded_time_to_task =
      heap_->tracer()->AverageTimeToIncrementalMarkingTask();
  base::Optional<double> current_time_to_task;
  if (pending_task_.has_value()) {
    const double delta_ms =
        (v8::base::TimeTicks::Now() - scheduled_time_).InMillisecondsF();
    if (pending_task_.value() == TaskType::kNormal) {
      current_time_to_task.emplace(delta_ms);
    } else {
      const double delayed_delta_ms =
          delta_ms - v8_flags.incremental_marking_task_delay_ms;
      if (delayed_delta_ms > 0) {
        current_time_to_task.emplace(delayed_delta_ms);
      }
    }
  }
  if (recorded_time_to_task == 0.0) {
    return current_time_to_task.has_value()
               ? v8::base::Optional<
                     v8::base::TimeDelta>{v8::base::TimeDelta::
                                              FromMillisecondsD(
                                                  current_time_to_task.value())}
               : v8::base::Optional<v8::base::TimeDelta>{};
  }
  return current_time_to_task.has_value()
             ? v8::base::Optional<
                   v8::base::TimeDelta>{v8::base::TimeDelta::FromMillisecondsD(
                   (current_time_to_task.value() + recorded_time_to_task) / 2)}
             : v8::base::Optional<v8::base::TimeDelta>{
                   v8::base::TimeDelta::FromMillisecondsD(
                       recorded_time_to_task)};
}

}  // namespace v8::internal
