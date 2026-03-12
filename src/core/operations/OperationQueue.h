#pragma once

// OperationQueue — GParted-style operation queue.
//
// Operations are queued without being applied. When the user confirms,
// all queued operations are applied sequentially. On first error,
// execution stops. Individual operations may be undone if they support it.
//
// The queue emits Qt signals for progress reporting and error notification,
// making it easy to connect to a UI progress dialog.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "Operation.h"
#include "../common/Error.h"
#include "../common/Result.h"

#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include <QObject>
#include <QString>

namespace spw
{

class OperationQueue : public QObject
{
    Q_OBJECT

public:
    explicit OperationQueue(QObject* parent = nullptr);
    ~OperationQueue() override;

    // Non-copyable
    OperationQueue(const OperationQueue&) = delete;
    OperationQueue& operator=(const OperationQueue&) = delete;

    // ----- Queue management -----

    // Add an operation to the end of the queue.
    // Takes ownership of the operation.
    void enqueue(std::unique_ptr<Operation> op);

    // Remove the last queued operation (if still pending).
    // Returns the removed operation, or nullptr if queue is empty/not pending.
    std::unique_ptr<Operation> removeLast();

    // Clear all pending operations from the queue.
    // Does not affect completed or failed operations in the history.
    void clearPending();

    // Clear everything (pending queue + history)
    void clearAll();

    // ----- Query -----

    // Number of pending (not yet executed) operations
    int pendingCount() const;

    // Number of completed operations in history
    int completedCount() const;

    // Total operations (pending + completed + failed)
    int totalCount() const;

    // Get a pending operation by index
    const Operation* pendingAt(int index) const;

    // Get all pending operations (read-only view)
    const std::deque<std::unique_ptr<Operation>>& pending() const { return m_pending; }

    // Get completed operation history (read-only view)
    const std::vector<std::unique_ptr<Operation>>& history() const { return m_history; }

    // Is the queue currently executing?
    bool isRunning() const { return m_running; }

    // Was the last apply run successful (all ops completed)?
    bool lastRunSuccessful() const { return m_lastRunSuccess; }

    // ----- Execution -----

    // Apply all queued operations sequentially.
    // Stops on first error. Returns the error from the failed operation,
    // or success if all completed.
    // This is a blocking call — run it from a worker thread if needed.
    Result<void> applyAll();

    // Undo the last completed operation (if it supports undo).
    // Returns the undo result.
    Result<void> undoLast();

    // Check if the last completed operation can be undone
    bool canUndoLast() const;

    // Request cancellation of the currently running operation.
    // The current operation will complete or fail on its own;
    // subsequent operations will not be started.
    void requestCancel();

    // Is cancellation requested?
    bool isCancelRequested() const { return m_cancelRequested; }

signals:
    // Emitted when a single operation starts
    void operationStarted(int operationIndex, int totalOperations, const QString& description);

    // Emitted when a single operation completes (success or failure)
    void operationCompleted(int operationIndex, bool success, const QString& description);

    // Emitted periodically with overall queue progress
    // overallPercent: 0-100 across all operations
    // currentOpPercent: 0-100 for current operation
    void queueProgress(int overallPercent, int currentOpPercent, const QString& status);

    // Emitted when an operation fails
    void errorOccurred(int operationIndex, const QString& description, const ErrorInfo& error);

    // Emitted when all operations are done (success or stopped on error)
    void allOperationsFinished(bool success, int completedCount, int totalCount);

private:
    // Pending operations (FIFO order)
    std::deque<std::unique_ptr<Operation>> m_pending;

    // Completed/failed operations (history, in execution order)
    std::vector<std::unique_ptr<Operation>> m_history;

    bool m_running = false;
    bool m_cancelRequested = false;
    bool m_lastRunSuccess = false;
};

} // namespace spw
