set(core_file "${SRT_SOURCE_DIR}/srtcore/core.cpp")
file(READ "${core_file}" core_source)

set(local_lock_class "    class SendApiLock\n    {\n    public:\n        SendApiLock(Mutex& mutex, bool blocking)\n            : mutex_(mutex), locked_(blocking ? (mutex_.lock(), true) : mutex_.try_lock())\n        {\n        }\n        ~SendApiLock()\n        {\n            if (locked_)\n                mutex_.unlock();\n        }\n        bool locked() const { return locked_; }\n\n    private:\n        Mutex& mutex_;\n        bool locked_;\n    };\n\n")
string(REPLACE "${local_lock_class}" "" core_source "${core_source}")

set(guard_class "class NonblockingApiLockGuard\n{\npublic:\n    NonblockingApiLockGuard(Mutex& mutex, bool blocking)\n        : mutex_(mutex), locked_(blocking ? (mutex_.lock(), true) : mutex_.try_lock())\n    {\n    }\n    ~NonblockingApiLockGuard()\n    {\n        if (locked_)\n            mutex_.unlock();\n    }\n    bool locked() const { return locked_; }\n\nprivate:\n    Mutex& mutex_;\n    bool locked_;\n};\n\n")
string(REPLACE "${guard_class}" "" core_source "${core_source}")
set(namespace_marker "using namespace srt_logging;\n\n")
string(FIND "${core_source}" "${namespace_marker}" namespace_location)
if(namespace_location EQUAL -1)
    message(FATAL_ERROR "Unable to locate SRT namespace declarations")
endif()
string(REPLACE "${namespace_marker}" "${namespace_marker}${guard_class}" core_source "${core_source}")

set(lock_original "    UniqueLock sendguard(m_SendLock);")
set(lock_old_patch "    UniqueLock sendguard(m_SendLock, std::defer_lock);\n    if (m_config.bSynSending)\n    {\n        sendguard.lock();\n    }\n    else if (!sendguard.try_lock())\n    {\n        throw CUDTException(MJ_AGAIN, MN_WRAVAIL, 0);\n    }")
set(lock_replacement "    NonblockingApiLockGuard sendguard(m_SendLock, m_config.bSynSending);\n    if (!sendguard.locked())\n        throw CUDTException(MJ_AGAIN, MN_WRAVAIL, 0);")
string(REPLACE "${lock_old_patch}" "${lock_replacement}" core_source "${core_source}")
string(REPLACE "${lock_original}" "${lock_replacement}" core_source "${core_source}")
string(REPLACE "    SendApiLock sendguard" "    NonblockingApiLockGuard sendguard" core_source "${core_source}")

set(initial_ack_original "        ScopedLock ack_lock(m_RecvAckLock);\n        m_tsLastRspAckTime = steady_clock::now();")
set(initial_ack_replacement "        NonblockingApiLockGuard ack_lock(m_RecvAckLock, m_config.bSynSending);\n        if (!ack_lock.locked())\n            throw CUDTException(MJ_AGAIN, MN_WRAVAIL, 0);\n        m_tsLastRspAckTime = steady_clock::now();")
string(REPLACE "${initial_ack_original}" "${initial_ack_replacement}" core_source "${core_source}")
string(REPLACE "        SendApiLock ack_lock" "        NonblockingApiLockGuard ack_lock" core_source "${core_source}")

set(drop_original "    const int iPktsTLDropped SRT_ATR_UNUSED = sndDropTooLate();")
set(drop_replacement "    const int iPktsTLDropped SRT_ATR_UNUSED =\n        m_config.bSynSending ? sndDropTooLate() : 0;")
string(REPLACE "${drop_original}" "${drop_replacement}" core_source "${core_source}")

set(insert_ack_original "        ScopedLock recvAckLock(m_RecvAckLock);\n        // insert the user buffer into the sending list")
set(insert_ack_replacement "        NonblockingApiLockGuard recvAckLock(m_RecvAckLock, m_config.bSynSending);\n        if (!recvAckLock.locked())\n            throw CUDTException(MJ_AGAIN, MN_WRAVAIL, 0);\n        // insert the user buffer into the sending list")
string(REPLACE "${insert_ack_original}" "${insert_ack_replacement}" core_source "${core_source}")
string(REPLACE "        SendApiLock recvAckLock" "        NonblockingApiLockGuard recvAckLock" core_source "${core_source}")

set(close_locks_original "    ScopedLock sendguard(m_SendLock);\n    ScopedLock recvguard(m_RecvLock);")
set(close_locks_replacement "    NonblockingApiLockGuard sendguard(\n        m_SendLock, m_config.bSynSending || m_config.bSynRecving);\n    NonblockingApiLockGuard recvguard(\n        m_RecvLock, m_config.bSynSending || m_config.bSynRecving);")
string(REPLACE "${close_locks_original}" "${close_locks_replacement}" core_source "${core_source}")

file(WRITE "${core_file}" "${core_source}")
message(STATUS "Applied SRT nonblocking API-lock patch")
