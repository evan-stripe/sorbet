#include "core/lsp/PreemptionTaskManager.h"
#include "core/lsp/Task.h"
#include "core/lsp/TypecheckEpochManager.h"

using namespace std;

namespace sorbet::core::lsp {

PreemptionTaskManager::PreemptionTaskManager(shared_ptr<TypecheckEpochManager> epochManager)
    : epochManager(move(epochManager)) {}

bool PreemptionTaskManager::trySchedulePreemptionTask(std::shared_ptr<Task> task) {
    TypecheckEpochManager::assertConsistentThread(
        messageProcessingThreadId, "PreemptionTaskManager::trySchedulePreemptionTask", "message processing thread");
    bool success = false;
    // Need to grab epoch lock so we have accurate information w.r.t. if typechecking is happening / if typechecking was
    // canceled. Avoids races with typechecking thread.
    this->epochManager->withEpochLock(
        [&preemptTask = this->preemptTask, &task, &success](TypecheckEpochManager::TypecheckingStatus status) -> void {
            // The code should only ever set one preempt function.
            auto existingTask = atomic_load(&preemptTask);
            ENFORCE(existingTask == nullptr);
            if (!status.slowPathRunning || status.slowPathWasCanceled || existingTask != nullptr) {
                // No slow path running, typechecking was canceled so we can't preempt the canceled slow path, or a task
                // is already scheduled. The latter should _never_ occur, as the scheduled task should _block_ the
                // thread that scheduled it.
                return;
            }
            success = atomic_compare_exchange_strong(&preemptTask, &existingTask, move(task));
        });

    return success;
}

bool PreemptionTaskManager::tryRunScheduledPreemptionTask() {
    TypecheckEpochManager::assertConsistentThread(
        typecheckingThreadId, "PreemptionTaskManager::tryRunScheduledPreemptionTask", "typechecking thread");
    auto preemptTask = atomic_load(&this->preemptTask);
    if (preemptTask != nullptr) {
        // Capture with write lock before running task. Ensures that all worker threads park before we proceed.
        absl::MutexLock lock(&typecheckMutex);
        // Invariant: Typechecking _cannot_ be canceled before or during a preemption task.
        ENFORCE(!epochManager->wasTypecheckingCanceled());
        preemptTask->run();
        atomic_store(&this->preemptTask, shared_ptr<lsp::Task>(nullptr));
        ENFORCE(!epochManager->wasTypecheckingCanceled());
        return true;
    }
    return false;
}

unique_ptr<absl::ReaderMutexLock> PreemptionTaskManager::lockPreemption() const {
    return make_unique<absl::ReaderMutexLock>(&typecheckMutex);
}

void PreemptionTaskManager::assertTypecheckMutexHeld() {
    typecheckMutex.AssertHeld();
}

} // namespace sorbet::core::lsp