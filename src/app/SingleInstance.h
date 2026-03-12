#pragma once

#include <QString>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace spw
{

// Prevents multiple instances of the application using a named kernel mutex.
class SingleInstance
{
public:
    explicit SingleInstance(const QString& mutexName);
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // Returns true if this is the only running instance.
    bool isUnique() const;

private:
#ifdef _WIN32
    HANDLE m_mutex = nullptr;
#endif
    bool m_isUnique = false;
};

} // namespace spw
