#include "main/lsp/LSPTypecheckerCoordinator.h"

#include "absl/synchronization/notification.h"

namespace sorbet::realmain::lsp {
using namespace std;

LSPTypecheckerCoordinator::Task::Task(function<void()> &&lambda) : lambda(move(lambda)) {}

void LSPTypecheckerCoordinator::Task::run() {
    lambda();
}

LSPTypecheckerCoordinator::LSPTypecheckerCoordinator(const shared_ptr<const LSPConfiguration> &config,
                                                     WorkerPool &workers)
    : shouldTerminate(false), typechecker(config), config(config), hasDedicatedThread(false), workers(workers) {}

void LSPTypecheckerCoordinator::asyncRunInternal(std::shared_ptr<Task> task) {
    if (hasDedicatedThread) {
        tasks.push(move(task), 1);
    } else {
        task->run();
    }
}

void LSPTypecheckerCoordinator::typecheckAsync(shared_ptr<LSPFileUpdates> updates) {
    // Just to be memory-safe, let's make this a shared ptr so the lambda can share ownership.
    auto notification = make_shared<absl::Notification>();
    asyncRunInternal(make_shared<Task>(
        [&typechecker = this->typechecker, &workers = this->workers, notification, updates]() -> void {
            // Slow path (non-blocking so we can cancel it). Tell globalstate that we're starting a change
            // that can be canceled before passing off the lambda.
            typechecker.startCommitEpoch(updates->epoch);
            notification->Notify();
            const u4 merged = updates->editCount - 1;
            // Only report stats if the edit was committed.
            if (!typechecker.typecheck(move(*updates), workers)) {
                prodCategoryCounterInc("lsp.messages.processed", "sorbet/workspaceEdit");
                prodCategoryCounterAdd("lsp.messages.processed", "sorbet/mergedEdits", merged);
            }
        }));
    // To avoid race conditions, wait until the commit epoch begins.
    notification->WaitForNotification();
}

void LSPTypecheckerCoordinator::syncRunPreempt(function<void(LSPTypechecker &)> &&lambda,
                                               core::GlobalState &initialGS) {
    absl::Notification notification;
    CounterState typecheckerCounters;
    // Note: Capturing notification by reference is safe here. We wait for the notification to happen prior to
    // returning.
    auto task = make_shared<Task>([&typechecker = this->typechecker, lambda, &notification, &typecheckerCounters,
                                   hasDedicatedThread = this->hasDedicatedThread]() -> void {
        lambda(typechecker);
        if (hasDedicatedThread) {
            typecheckerCounters = getAndClearThreadCounters();
        }
        notification.Notify();
    });

    // There's a lot going on in these three lines. If preemption succeeds, the currently running slow path is
    // guaranteed to run it. If it fails, we need to schedule it ourselves.
    if (!initialGS.tryPreempt(task)) {
        asyncRunInternal(move(task));
    }

    notification.WaitForNotification();

    // If typechecker is running on a dedicated thread, then we need to merge its metrics w/
    // coordinator thread's so we report them.
    if (hasDedicatedThread) {
        counterConsume(move(typecheckerCounters));
    }
}

void LSPTypecheckerCoordinator::syncRun(function<void(LSPTypechecker &, WorkerPool &)> &&lambda) {
    absl::Notification notification;
    CounterState typecheckerCounters;
    // If typechecker is running on a dedicated thread, then we need to merge its metrics w/ coordinator thread's so we
    // report them.
    // Note: Capturing notification by reference is safe here, we we wait for the notification to happen prior to
    // returning.
    asyncRunInternal(
        make_shared<Task>([&typechecker = this->typechecker, &workers = this->workers, lambda, &notification,
                           &typecheckerCounters, hasDedicatedThread = this->hasDedicatedThread]() -> void {
            lambda(typechecker, workers);
            if (hasDedicatedThread) {
                typecheckerCounters = getAndClearThreadCounters();
            }
            notification.Notify();
        }));
    notification.WaitForNotification();
    if (hasDedicatedThread) {
        counterConsume(move(typecheckerCounters));
    }
}

unique_ptr<core::GlobalState> LSPTypecheckerCoordinator::shutdown() {
    unique_ptr<core::GlobalState> gs;
    syncRun([&](auto &typechecker, auto &workers) -> void {
        shouldTerminate = true;
        gs = typechecker.destroy();
    });
    return gs;
}

unique_ptr<Joinable> LSPTypecheckerCoordinator::startTypecheckerThread() {
    if (hasDedicatedThread) {
        Exception::raise("Typechecker already started on a dedicated thread.");
    }

    hasDedicatedThread = true;
    return runInAThread("Typechecker", [&]() -> void {
        typechecker.changeThread();

        while (!shouldTerminate) {
            shared_ptr<Task> task;
            // Note: Pass in 'true' for silent to avoid spamming log with wait_pop_timed entries.
            auto result = tasks.wait_pop_timed(task, WorkerPool::BLOCK_INTERVAL(), *config->logger, true);
            if (result.gotItem()) {
                task->run();
            }
        }
    });
}

}; // namespace sorbet::realmain::lsp