#include "SingleInstance.h"

namespace spw
{

SingleInstance::SingleInstance(const QString& mutexName)
{
#ifdef _WIN32
    m_mutex = CreateMutexW(nullptr, FALSE, mutexName.toStdWString().c_str());
    if (m_mutex != nullptr)
    {
        m_isUnique = (GetLastError() != ERROR_ALREADY_EXISTS);
    }
#else
    Q_UNUSED(mutexName);
    m_isUnique = true; // Non-Windows: always unique (placeholder)
#endif
}

SingleInstance::~SingleInstance()
{
#ifdef _WIN32
    if (m_mutex != nullptr)
    {
        ReleaseMutex(m_mutex);
        CloseHandle(m_mutex);
    }
#endif
}

bool SingleInstance::isUnique() const
{
    return m_isUnique;
}

} // namespace spw
