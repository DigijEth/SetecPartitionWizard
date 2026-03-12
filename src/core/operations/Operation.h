#pragma once

// Operation — Abstract base class for all disk operations.
//
// Operations follow a GParted-style pattern: they are queued first, then
// applied sequentially. Each operation knows how to execute itself and
// (where possible) undo itself.
//
// DISCLAIMER: This code is for authorized disk utility software only.

#include "../common/Error.h"
#include "../common/Result.h"
#include "../common/Types.h"

#include <functional>
#include <memory>
#include <string>

#include <QString>

namespace spw
{

// Progress callback for individual operations: (percent 0-100, status message)
using ProgressCallback = std::function<void(int percent, const QString& status)>;

class Operation
{
public:
    // All supported operation types
    enum class Type
    {
        CreatePartition,
        DeletePartition,
        ResizePartition,
        MovePartition,
        FormatPartition,
        SetLabel,
        SetFlags,
        Clone,
        CreateImage,
        RestoreImage,
        FlashImage,
        SecureErase,
        RepairBoot,
        CheckFilesystem,
    };

    // Execution state tracking
    enum class State
    {
        Pending,     // Queued, not yet executed
        Running,     // Currently executing
        Completed,   // Finished successfully
        Failed,      // Finished with error
        Undone,      // Successfully undone
    };

    virtual ~Operation() = default;

    // What kind of operation is this?
    virtual Type type() const = 0;

    // Human-readable description for the UI
    virtual QString description() const = 0;

    // Execute the operation. Progress is reported via the callback.
    // Returns Result<void> — success or an error.
    virtual Result<void> execute(ProgressCallback progress) = 0;

    // Attempt to undo the operation. Not all operations are undoable.
    // Default implementation returns NotImplemented.
    virtual Result<void> undo()
    {
        return ErrorInfo::fromCode(ErrorCode::NotImplemented,
            "Undo is not supported for this operation");
    }

    // Returns true if this operation can be undone after execution.
    virtual bool canUndo() const { return false; }

    // Current state
    State state() const { return m_state; }

    // Error info if state == Failed
    const ErrorInfo& lastError() const { return m_lastError; }

    // Target disk for this operation (if applicable)
    DiskId targetDiskId() const { return m_targetDiskId; }
    void setTargetDiskId(DiskId id) { m_targetDiskId = id; }

    // Target partition index (if applicable)
    PartitionId targetPartitionId() const { return m_targetPartitionId; }
    void setTargetPartitionId(PartitionId id) { m_targetPartitionId = id; }

    // Returns the operation type as a string
    static QString typeToString(Type t)
    {
        switch (t)
        {
        case Type::CreatePartition:  return QStringLiteral("Create Partition");
        case Type::DeletePartition:  return QStringLiteral("Delete Partition");
        case Type::ResizePartition:  return QStringLiteral("Resize Partition");
        case Type::MovePartition:    return QStringLiteral("Move Partition");
        case Type::FormatPartition:  return QStringLiteral("Format Partition");
        case Type::SetLabel:         return QStringLiteral("Set Label");
        case Type::SetFlags:         return QStringLiteral("Set Flags");
        case Type::Clone:            return QStringLiteral("Clone");
        case Type::CreateImage:      return QStringLiteral("Create Image");
        case Type::RestoreImage:     return QStringLiteral("Restore Image");
        case Type::FlashImage:       return QStringLiteral("Flash Image");
        case Type::SecureErase:      return QStringLiteral("Secure Erase");
        case Type::RepairBoot:       return QStringLiteral("Repair Boot");
        case Type::CheckFilesystem:  return QStringLiteral("Check Filesystem");
        }
        return QStringLiteral("Unknown");
    }

    friend class OperationQueue;

protected:
    State m_state = State::Pending;
    ErrorInfo m_lastError;
    DiskId m_targetDiskId = -1;
    PartitionId m_targetPartitionId = -1;
};

} // namespace spw
