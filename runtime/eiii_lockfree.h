// Lock-free building blocks shared by the engine, the CLAP plugin and the GUI (header-only, no allocation after
// construction, wait-free on both sides).
#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

namespace eiii {

// Single-producer / single-consumer queue of trivially copyable records; capacity rounded up to a power of two.
template <class T> class spsc_queue
{
	static_assert(std::is_trivially_copyable_v<T>);
public:
	explicit spsc_queue(size_t capacity)
	{
		size_t c = 2; while (c < capacity) c <<= 1;
		m_mask = c - 1; m_buf = std::make_unique<T[]>(c);
	}
	bool push(T const &v)                 // producer
	{
		uint64_t const w = m_w.load(std::memory_order_relaxed);
		if (w - m_r.load(std::memory_order_acquire) > m_mask) return false;
		m_buf[w & m_mask] = v;
		m_w.store(w + 1, std::memory_order_release);
		return true;
	}
	bool pop(T &v)                        // consumer
	{
		uint64_t const r = m_r.load(std::memory_order_relaxed);
		if (r == m_w.load(std::memory_order_acquire)) return false;
		v = m_buf[r & m_mask];
		m_r.store(r + 1, std::memory_order_release);
		return true;
	}
	bool peek(T &v) const                 // consumer
	{
		uint64_t const r = m_r.load(std::memory_order_relaxed);
		if (r == m_w.load(std::memory_order_acquire)) return false;
		v = m_buf[r & m_mask];
		return true;
	}
	size_t size() const { return size_t(m_w.load(std::memory_order_acquire) - m_r.load(std::memory_order_acquire)); }
private:
	std::unique_ptr<T[]> m_buf;
	size_t m_mask = 0;
	alignas(64) std::atomic<uint64_t> m_w{0};
	alignas(64) std::atomic<uint64_t> m_r{0};
};

// Single-producer / single-consumer ring of stereo float frames with absolute 64-bit frame indices.
class audio_ring
{
public:
	explicit audio_ring(size_t frames)
	{
		size_t c = 2; while (c < frames) c <<= 1;
		m_mask = c - 1; m_buf = std::make_unique<float[]>(c * 2);
	}
	// Only call while producer and consumer are stopped (a new engine activation).
	void reset() { m_w.store(0); m_r.store(0); }
	size_t capacity() const { return m_mask + 1; }
	uint64_t write_index() const { return m_w.load(std::memory_order_acquire); }
	uint64_t read_index() const { return m_r.load(std::memory_order_acquire); }
	size_t fill() const { return size_t(write_index() - read_index()); }
	size_t space() const { return capacity() - fill(); }
	// producer: interleaved L/R
	size_t write(float const *lr, size_t frames)
	{
		uint64_t const w = m_w.load(std::memory_order_relaxed);
		size_t const n = std::min(frames, capacity() - size_t(w - m_r.load(std::memory_order_acquire)));
		for (size_t i = 0; i < n; ++i) { size_t const p = size_t((w + i) & m_mask) * 2; m_buf[p] = lr[2 * i]; m_buf[p + 1] = lr[2 * i + 1]; }
		m_w.store(w + n, std::memory_order_release);
		return n;
	}
	// consumer: planar output; returns frames read
	size_t read(float *l, float *r, size_t frames)
	{
		uint64_t const rd = m_r.load(std::memory_order_relaxed);
		size_t const n = std::min(frames, size_t(m_w.load(std::memory_order_acquire) - rd));
		for (size_t i = 0; i < n; ++i) { size_t const p = size_t((rd + i) & m_mask) * 2; l[i] = m_buf[p]; r[i] = m_buf[p + 1]; }
		m_r.store(rd + n, std::memory_order_release);
		return n;
	}
	// consumer side, only while the producer is known to be idle (activation handshake)
	void drain() { m_r.store(m_w.load(std::memory_order_acquire), std::memory_order_release); }
private:
	std::unique_ptr<float[]> m_buf;
	size_t m_mask = 0;
	alignas(64) std::atomic<uint64_t> m_w{0};
	alignas(64) std::atomic<uint64_t> m_r{0};
};

// Triple buffer: one writer publishes whole values, one reader always gets the latest complete one.
template <class T> class triple_buffer
{
	static_assert(std::is_trivially_copyable_v<T>);
public:
	T &back() { return m_buf[m_back]; }                    // writer: fill, then publish()
	void publish()
	{
		uint8_t const prev = m_mid.exchange(uint8_t(m_back | 4), std::memory_order_acq_rel);
		m_back = prev & 3;
	}
	bool read(T &out)                                      // reader: true if a new value was published since the last read
	{
		bool fresh = false;
		if (m_mid.load(std::memory_order_acquire) & 4)
		{
			uint8_t const prev = m_mid.exchange(m_front, std::memory_order_acq_rel);
			m_front = prev & 3; fresh = true;
		}
		out = m_buf[m_front];
		return fresh;
	}
private:
	T m_buf[3]{};
	uint8_t m_back = 0, m_front = 1;
	std::atomic<uint8_t> m_mid{2};
};

} // namespace eiii
