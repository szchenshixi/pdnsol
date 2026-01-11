
/******************************************************************************/
/*
  Author  - Ming-Lun "Allen" Chou
  Web     - http://AllenChou.net
  Twitter - @TheAllenChou
 */
/******************************************************************************/

#ifndef LOCK_HPP_
#define LOCK_HPP_

#include <condition_variable>
#include <mutex>

namespace pdnsol {

class RwLock {
    friend class ReadAutoLock;
    friend class WriteAutoLock;

  public:
    RwLock()
        : m_counter(0) {}

  private:
    void AcquireReadLock() {
        std::unique_lock<std::mutex> lock(m_counterMutex);
        m_cv.wait(lock, [&]() {
            return !(m_counter & kWriterBit) && m_counter < kMaxReaders;
        });
        ++m_counter;
    }

    void ReleaseReadLock() {
        std::unique_lock<std::mutex> lock(m_counterMutex);
        --m_counter;
        m_cv.notify_one();
    }

    void AcquireWriteLock() {
        std::unique_lock<std::mutex> lock(m_counterMutex);
        m_cv.wait(lock, [&]() { return m_counter == 0; });
        m_counter |= kWriterBit;
    }

    void ReleaseWriteLock() {
        std::unique_lock<std::mutex> lock(m_counterMutex);
        m_counter &= ~kWriterBit;
        m_cv.notify_all();
    }

    static const size_t     kWriterBit = size_t(1) << (sizeof(size_t) * 8 - 1);
    static const size_t     kMaxReaders = kWriterBit - 1;
    std::mutex              m_counterMutex;
    std::condition_variable m_cv;
    size_t m_counter; // MSB, i.e. writer bit, acts as a "mutex"
};

class ReadAutoLock {
  public:
    ReadAutoLock(RwLock& lock)
        : mLock(lock) {
        mLock.AcquireReadLock();
    }
    ~ReadAutoLock() { mLock.ReleaseReadLock(); }

  private:
    RwLock& mLock;
};

class WriteAutoLock {
  public:
    WriteAutoLock(RwLock& lock)
        : mLock(lock) {
        mLock.AcquireWriteLock();
    }
    ~WriteAutoLock() { mLock.ReleaseWriteLock(); }

  private:
    RwLock& mLock;
};
} // namespace pdnsol
#endif // LOCK_HPP_