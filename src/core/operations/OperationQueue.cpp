// OperationQueue.cpp — GParted-style operation queue implementation.
//
// Operations are queued, then applied sequentially. Execution stops on
// first error. Progress is reported via Qt signals for UI integration.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "OperationQueue.h"

#include <algorithm>

namespace spw
{

OperationQueue::OperationQueue(QObject* parent)
    : QObject(parent)
{
}

OperationQueue::~OperationQueue() = default;

// ============================================================================
// Queue management
// ============================================================================

void OperationQueue::enqueue(std::unique_ptr<Operation> op)
{
    if (op)
    {
        m_pending.push_back(std::move(op));
    }
}

std::unique_ptr<Operation> OperationQueue::removeLast()
{
    if (m_pending.empty())
        return nullptr;

    // Only remove if the last operation is still pending
    if (m_pending.back()->state() != Operation::State::Pending)
        return nullptr;

    auto op = std::move(m_pending.back());
    m_pending.pop_back();
    return op;
}

void OperationQueue::clearPending()
{
    // Remove only pending operations (from the back, since some at the front
    // might have been partially processed if we stopped mid-run)
    auto it = std::remove_if(m_pending.begin(), m_pending.end(),
        [](const std::unique_ptr<Operation>& op)
        {
            return op->state() == Operation::State::Pending;
        });
    m_pending.erase(it, m_pending.end());
}

void OperationQueue::clearAll()
{
    m_pending.clear();
    m_history.clear();
    m_lastRunSuccess = false;
}

// ============================================================================
// Query
// ============================================================================

int OperationQueue::pendingCount() const
{
    return static_cast<int>(m_pending.size());
}

int OperationQueue::completedCount() const
{
    return static_cast<int>(m_history.size());
}

int OperationQueue::totalCount() const
{
    return pendingCount() + completedCount();
}

const Operation* OperationQueue::pendingAt(int index) const
{
    if (index < 0 || index >= static_cast<int>(m_pending.size()))
        return nullptr;
    return m_pending[static_cast<size_t>(index)].get();
}

// ============================================================================
// Execution
// ============================================================================

Result<void> OperationQueue::applyAll()
{
    if (m_pending.empty())
    {
        m_lastRunSuccess = true;
        emit allOperationsFinished(true, 0, 0);
        return Result<void>::ok();
    }

    m_running = true;
    m_cancelRequested = false;
    m_lastRunSuccess = false;

    const int totalOps = static_cast<int>(m_pending.size());
    int opIndex = 0;
    ErrorInfo lastError;

    while (!m_pending.empty())
    {
        if (m_cancelRequested)
        {
            lastError = ErrorInfo::fromCode(ErrorCode::OperationCanceled,
                "Operation queue canceled by user");
            break;
        }

        // Take the front operation
        auto op = std::move(m_pending.front());
        m_pending.pop_front();

        QString desc = op->description();
        emit operationStarted(opIndex, totalOps, desc);

        // Build a progress callback that maps per-operation progress to overall progress
        auto progressCb = [this, opIndex, totalOps](int opPercent, const QString& status)
        {
            // Overall percent: evenly divided among operations
            int overallPercent = (opIndex * 100 + opPercent) / totalOps;
            overallPercent = std::clamp(overallPercent, 0, 100);

            emit queueProgress(overallPercent, opPercent, status);
        };

        // Execute the operation
        op->m_state = Operation::State::Running;
        auto result = op->execute(progressCb);

        if (result.isOk())
        {
            op->m_state = Operation::State::Completed;
            emit operationCompleted(opIndex, true, desc);
        }
        else
        {
            op->m_state = Operation::State::Failed;
            op->m_lastError = result.error();
            lastError = result.error();

            emit operationCompleted(opIndex, false, desc);
            emit errorOccurred(opIndex, desc, result.error());

            // Move to history and stop
            m_history.push_back(std::move(op));
            break;
        }

        m_history.push_back(std::move(op));
        ++opIndex;
    }

    // Check if all completed successfully
    bool success = m_pending.empty() && !lastError.isError();
    m_lastRunSuccess = success;
    m_running = false;

    emit allOperationsFinished(success, opIndex, totalOps);

    if (lastError.isError())
        return lastError;

    return Result<void>::ok();
}

Result<void> OperationQueue::undoLast()
{
    if (m_history.empty())
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "No operations to undo");
    }

    auto& lastOp = m_history.back();

    if (!lastOp->canUndo())
    {
        return ErrorInfo::fromCode(ErrorCode::NotImplemented,
            "Last operation does not support undo: " + lastOp->description().toStdString());
    }

    if (lastOp->state() != Operation::State::Completed)
    {
        return ErrorInfo::fromCode(ErrorCode::InvalidArgument,
            "Can only undo completed operations");
    }

    auto result = lastOp->undo();
    if (result.isOk())
    {
        lastOp->m_state = Operation::State::Undone;
    }

    return result;
}

bool OperationQueue::canUndoLast() const
{
    if (m_history.empty())
        return false;

    const auto& lastOp = m_history.back();
    return lastOp->canUndo() && lastOp->state() == Operation::State::Completed;
}

void OperationQueue::requestCancel()
{
    m_cancelRequested = true;
}

} // namespace spw
