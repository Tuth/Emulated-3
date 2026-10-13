// Minimal MAME device-framework shim for compiling selected, UNMODIFIED MAME device sources
// (NS32016 CPU, NS32081 FPU) outside MAME.  Not MAME code; provides only what those files use.
// Utility headers (strformat, coretmpl, disasmintf, osdcomm) are MAME's own, used as-is.
#pragma once
#ifndef __EMU_H__
#define __EMU_H__

#include "osdcomm.h"
#include "coretmpl.h"
#include "strformat.h"
#include "disasmintf.h"

#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#include <system_error>

using osd::u8; using osd::u16; using osd::u32; using osd::u64;
using osd::s8; using osd::s16; using osd::s32; using osd::s64;
using offs_t = u32;
using util::BIT;
using util::string_format;
using util::bitswap;

// eminline.h equivalents used by the CPU core
inline u64 mulu_32x32(u32 a, u32 b) { return u64(a) * u64(b); }
inline s64 mul_32x32(s32 a, s32 b) { return s64(a) * s64(b); }
using util::make_bitmask;

#define NAME(x) x, #x
#define FUNC(x) &x, #x
#define TIMER_CALLBACK_MEMBER(name) void name(s32 param)
#define DEVICE_SELF ""
#define DEVICE_SELF_OWNER "^"
#define DECLARE_READ_LINE_MEMBER(name) int name()
#define DECLARE_WRITE_LINE_MEMBER(name) void name(int state)
#define DEFINE_DEVICE_TYPE_PRIVATE(Type, Base, Class, ShortName, FullName) device_type_impl const Type{ ShortName, FullName };
#define ALLOW_SAVE_TYPE(t)
#define STRUCT_MEMBER(s, m) s, &std::remove_reference_t<decltype(s[0])>::m, #s "." #m

enum endianness_t { ENDIANNESS_LITTLE, ENDIANNESS_BIG };
enum line_state { CLEAR_LINE = 0, ASSERT_LINE = 1 };
enum { INPUT_LINE_IRQ0 = 0, INPUT_LINE_NMI = 32 };
enum { STATE_GENPC = -1, STATE_GENPCBASE = -2, STATE_GENFLAGS = -3, STATE_GENSP = -4 };
enum { AS_PROGRAM = 0, AS_DATA = 1, AS_IO = 2, AS_OPCODES = 3 };
enum { TR_READ, TR_WRITE, TR_FETCH };

[[noreturn]] void shim_fatal(std::string const &msg);
template <typename... Params> [[noreturn]] void fatalerror(char const *fmt, Params &&... args)
{ shim_fatal(util::string_format(fmt, std::forward<Params>(args)...)); }

class machine_config { };
template <typename... A> void osd_printf_verbose(A &&...) { }
template <typename... A> void osd_printf_error(A &&...) { }

class emu_fatalerror : public std::runtime_error
{
public:
	template <typename... Params> emu_fatalerror(char const *fmt, Params &&... args)
		: std::runtime_error(util::string_format(fmt, std::forward<Params>(args)...)) { }
};

// OSD file/directory API: only used by the Apple CD-ROM "toolbox" vendor extension in bus/nscsi/cd.cpp,
// which the EIII never invokes.  Every call fails cleanly.
#define PATH_SEPARATOR "/"
enum { OPEN_FLAG_READ = 1, OPEN_FLAG_WRITE = 2, OPEN_FLAG_CREATE = 4 };
class osd_file
{
public:
	using ptr = std::unique_ptr<osd_file>;
	static std::error_condition open(std::string const &, u32, ptr &, u64 &) { return std::make_error_condition(std::errc::operation_not_supported); }
	std::error_condition read(void *, u64, u32, u32 &actual) { actual = 0; return std::make_error_condition(std::errc::operation_not_supported); }
	std::error_condition write(void const *, u64, u32, u32 &actual) { actual = 0; return std::make_error_condition(std::errc::operation_not_supported); }
};
namespace osd {
class directory
{
public:
	struct entry { enum class entry_type { NONE, FILE, DIR, OTHER }; char const *name = ""; entry_type type = entry_type::NONE; u64 size = 0; };
	using ptr = std::unique_ptr<directory>;
	static ptr open(std::string const &) { return nullptr; }
	entry const *read() { return nullptr; }
};
}

// time and timers: the standalone host schedules in CPU cycles; these exist so that unmodified device code compiles.
class attotime
{
public:
	using T = long double;
	constexpr attotime() = default;
	constexpr explicit attotime(T sec) : m_seconds(sec) { }
	static attotime from_ticks(u64 ticks, u32 freq) { return attotime(freq ? T(ticks) / T(freq) : T(0)); }
	static attotime from_hz(double hz) { return attotime(hz > 0 ? T(1) / T(hz) : T(1e30)); }
	static attotime from_seconds(s64 s) { return attotime(T(s)); }
	static attotime from_msec(s64 ms) { return attotime(T(ms) * T(1e-3)); }
	static attotime from_usec(s64 us) { return attotime(T(us) * T(1e-6)); }
	static attotime from_nsec(s64 ns) { return attotime(T(ns) * T(1e-9)); }
	static attotime from_double(double s) { return attotime(T(s)); }
	static const attotime zero;
	static const attotime never;
	s64 seconds() const { return s64(std::floor(m_seconds)); }
	double as_double() const { return double(m_seconds); }
	u64 as_ticks(u32 freq) const { return u64(m_seconds * T(freq) + T(1e-9)); }
	bool is_never() const { return m_seconds >= T(1e29); }
	std::string to_string() const { return util::string_format("%.9f", double(m_seconds)); }
	bool is_zero() const { return m_seconds == 0; }
	attotime operator+(attotime const &o) const { return (is_never() || o.is_never()) ? never : attotime(m_seconds + o.m_seconds); }
	attotime operator-(attotime const &o) const { return attotime(m_seconds - o.m_seconds); }
	attotime &operator+=(attotime const &o) { *this = *this + o; return *this; }
	attotime operator*(u32 f) const { return attotime(m_seconds * f); }
	attotime operator/(u32 f) const { return attotime(m_seconds / f); }
	bool operator<(attotime const &o) const { return m_seconds < o.m_seconds; }
	bool operator<=(attotime const &o) const { return m_seconds <= o.m_seconds; }
	bool operator>(attotime const &o) const { return m_seconds > o.m_seconds; }
	bool operator>=(attotime const &o) const { return m_seconds >= o.m_seconds; }
	bool operator==(attotime const &o) const { return m_seconds == o.m_seconds; }
	bool operator!=(attotime const &o) const { return m_seconds != o.m_seconds; }
	T m_seconds = 0;
};
inline const attotime attotime::zero{};
inline const attotime attotime::never{attotime::T(1e30)};

class emu_timer;
// timer_expired_delegate(FUNC(class::member), object) — member signature void(s32)
struct timer_expired_delegate
{
	template <class C, class O> timer_expired_delegate(void (C::*fn)(s32), char const *, O *obj)
	{ C *const t = static_cast<C *>(obj); m_f = [t, fn](s32 p) { (t->*fn)(p); }; }
	std::function<void(s32)> m_f;
};
// Scheduler with MAME's semantics: one CPU; timers fire at their expiry; while the CPU executes, machine time is the CPU's
// local time (start of slice + cycles executed); inside timer callbacks it is the expiry time.
class shim_scheduler
{
public:
	attotime time() const;
	attotime m_basetime;
	std::vector<emu_timer *> m_timers;
	class cpu_device *m_cpu = nullptr;
	bool m_cpu_running = false;
	attotime m_cpu_slice_start;
	attotime next_expiry() const;
	void fire_due(attotime upto);
	void abort_timeslice();
	emu_timer *timer_alloc(timer_expired_delegate d);
	std::vector<std::unique_ptr<emu_timer>> m_owned;
};
// The scheduler and the other shim globals are per thread: a runtime instance binds its own context to the thread that
// builds and runs its machine (shim_bind_thread), so several instances can run in one process.  Threads that never
// bind use the process-default context (the standalone's single thread).
extern thread_local shim_scheduler *g_shim_sched;
inline shim_scheduler &shim_sched() { return *g_shim_sched; }

class emu_timer
{
public:
	emu_timer(std::function<void(s32)> cb) : m_cb(std::move(cb)), m_sched(&shim_sched()) { m_sched->m_timers.push_back(this); }
	~emu_timer();
	emu_timer(emu_timer const &) = delete;
	emu_timer &operator=(emu_timer const &) = delete;
	void adjust(attotime start_delay, s32 param = 0, attotime period = attotime::never)
	{
		m_param = param; m_enabled = true; m_period = period;
		if (start_delay < attotime::zero) start_delay = attotime::zero;
		m_expire = m_sched->time() + start_delay; m_start = m_sched->time();
		m_sched->abort_timeslice();
	}
	void reset(attotime duration = attotime::never) { adjust(duration, m_param, m_period); }
	bool enable(bool en = true) { bool const old = m_enabled; m_enabled = en; return old; }
	bool enabled() const { return m_enabled; }
	s32 param() const { return m_param; }
	void set_param(s32 p) { m_param = p; }
	attotime expire() const { return m_enabled ? m_expire : attotime::never; }
	attotime remaining() const { attotime const n = m_sched->time(); return (!m_enabled || m_expire.is_never()) ? attotime::never : (m_expire <= n ? attotime::zero : m_expire - n); }
	attotime elapsed() const { return m_sched->time() - m_start; }
	attotime period() const { return m_period; }
	std::function<void(s32)> m_cb;
	attotime m_expire = attotime::never, m_start, m_period = attotime::never;
	s32 m_param = 0;
	bool m_enabled = false;
	shim_scheduler *m_sched;
};

class device_t;
struct device_type_impl
{
	char const *shortname; char const *fullname;
	// MAME lets configuration code call device types like functions (TYPE(config, tag, ...)); the standalone host
	// builds devices directly, so these calls exist only to keep unmodified device_add_mconfig bodies compiling.
	template <typename... A> device_t &operator()(A &&...) const { throw std::logic_error("device_type call not supported in the standalone host"); }
};
inline bool operator==(device_type_impl const &a, device_type_impl const &b) { return &a == &b; }
using device_type = device_type_impl const &;
#define DECLARE_DEVICE_TYPE(Type, Class) extern device_type_impl const Type;
#define DEFINE_DEVICE_TYPE(Type, Class, ShortName, FullName) device_type_impl const Type{ ShortName, FullName };

struct emu_options_stub { char const *share_directory() const { return "."; } };
class running_machine
{
public:
	bool debug_enabled() const { return false; }
	void debug_break() { }
	std::string describe_context() const { return std::string(); }
	bool side_effects_disabled() const { return false; }
	attotime time() const { return shim_sched().time(); }
	shim_scheduler &scheduler() const { return shim_sched(); }
	emu_options_stub const &options() const { static emu_options_stub o; return o; }
};

// ---- host bus: the standalone machine implements this ----
class address_space
{
public:
	virtual ~address_space() = default;
	// 16-bit bus semantics as in MAME: offset in bytes, mask selects byte lanes of the 16-bit word at (address & ~1)
	virtual u16 read_word(offs_t address, u16 mask) = 0;
	virtual void write_word(offs_t address, u16 data, u16 mask) = 0;
	template <typename T> void specific(T &accessor) { accessor.m_space = this; }
};

// address_map declarations in device sources compile but are never instantiated (the host bus decodes addresses)
struct address_map_entry
{
	template <typename... A> address_map_entry &r(A &&...) { return *this; }
	template <typename... A> address_map_entry &w(A &&...) { return *this; }
	template <typename... A> address_map_entry &rw(A &&...) { return *this; }
	template <typename... A> address_map_entry &lr8(A &&...) { return *this; }
	template <typename... A> address_map_entry &lw8(A &&...) { return *this; }
	template <typename... A> address_map_entry &lrw8(A &&...) { return *this; }
	template <typename... A> address_map_entry &umask16(A &&...) { return *this; }
	template <typename... A> address_map_entry &umask32(A &&...) { return *this; }
	template <typename... A> address_map_entry &mirror(A &&...) { return *this; }
	address_map_entry &nopr() { return *this; } address_map_entry &nopw() { return *this; } address_map_entry &noprw() { return *this; }
	address_map_entry &ram() { return *this; } address_map_entry &rom() { return *this; }
};
class address_map { public: address_map_entry &operator()(offs_t, offs_t) { return m_e; } address_map_entry m_e; };

class device_t;
class device_interface
{
public:
	device_interface(device_t &device, char const *type);
	virtual ~device_interface() = default;
	device_t &device() { return m_device; }
	device_t const &device() const { return m_device; }
	virtual void interface_pre_start() { }
	virtual void interface_post_start() { }
	virtual void interface_pre_reset() { }
	virtual void interface_post_reset() { }
private:
	device_t &m_device;
};

class device_t
{
public:
	device_t(machine_config const &, device_type type, char const *tag, device_t *owner, u32 clock)
		: m_type(type), m_tag(tag ? tag : ""), m_owner(owner), m_clock(clock)
	{
		if (owner) owner->m_children.push_back(this);
	}
	virtual ~device_t() = default;
	device_type type() const { return m_type; }
	u32 clock() const { return m_clock; }
	attotime clocks_to_attotime(u64 clocks) const { return attotime::from_ticks(clocks,m_clock); }
	char const *tag() const { return m_tag.c_str(); }
	char const *basetag() const { return m_tag.c_str(); }
	std::string subtag(std::string const &t) const { return m_tag + ":" + t; }
	device_t *owner() const { return m_owner; }
	device_t *subdevice(std::string const &t) const { for (device_t *c : m_children) if (c->m_tag == t) return c; return nullptr; }
	template <class T> T *subdevice(std::string const &t) const { return dynamic_cast<T *>(subdevice(t)); }
	running_machine &machine() const { return s_machine; }
	struct device_debug_stub { void exception_hook(int) { } void instruction_hook(offs_t) { } void privilege_hook() { } void wait_hook() { } template <typename... A> void interrupt_hook(A &&...) { } };
	device_debug_stub *debug() const { static device_debug_stub s; return &s; }
	template <typename Format, typename... Params> void logerror(Format &&fmt, Params &&... args) const
	{
		if (s_log) fputs(util::string_format(std::forward<Format>(fmt), std::forward<Params>(args)...).c_str(), s_log);
	}
	template <typename... T> void save_item(T &&...) { }
	template <typename... T> void save_pointer(T &&...) { }
	// timer_alloc(FUNC(class::member), this) — member signature void(s32)
	template <class C, class O> emu_timer *timer_alloc(void (C::*fn)(s32), char const *, O *obj)
	{
		C *const target = static_cast<C *>(obj);
		m_timers.emplace_back(new emu_timer([target, fn](s32 p) { (target->*fn)(p); }));
		return m_timers.back().get();
	}
	// host lifecycle (MAME order: resolve objects, pre_start interfaces, device_start, post_start)
	void resolve() { device_resolve_objects(); }
	void start() { for (auto *i : m_interfaces) i->interface_pre_start(); device_start(); for (auto *i : m_interfaces) i->interface_post_start(); }
	void reset() { for (auto *i : m_interfaces) i->interface_pre_reset(); device_reset(); for (auto *i : m_interfaces) i->interface_post_reset(); }
	void reset_after_children() { device_reset_after_children(); }
	device_t *siblingdevice(std::string const &t) const { return m_owner ? m_owner->subdevice(t) : nullptr; }
	template <class T> bool interface(T *&p) { p = dynamic_cast<T *>(this); return p != nullptr; }
	static thread_local FILE *s_log;
	std::vector<device_interface *> m_interfaces;
	std::vector<device_t *> m_children;
protected:
	virtual void device_resolve_objects() { }
	virtual void device_start() { }
	virtual void device_reset() { }
	virtual void device_reset_after_children() { }
	virtual void device_add_mconfig(machine_config &) { }
private:
	device_type m_type;
	std::string m_tag;
	device_t *m_owner;
	u32 m_clock;
	std::vector<std::unique_ptr<emu_timer>> m_timers;
	static running_machine s_machine;
};
inline device_interface::device_interface(device_t &device, char const *) : m_device(device) { device.m_interfaces.push_back(this); }

template <class Dest, class Source> inline Dest downcast(Source *src) { return static_cast<Dest>(src); }
template <class Dest, class Source> inline Dest downcast(Source &src) { return static_cast<Dest>(src); }

struct finder_base { static constexpr char const *DUMMY_TAG = "finder_dummy_tag"; };

// device finders: the host assigns targets explicitly with set() (or set_tag(pointer)).
template <class T> class device_finder_shim
{
public:
	device_finder_shim() = default;
	template <typename U> device_finder_shim(U &&base, char const *tag) : m_tag(tag ? tag : "")
	{
		using B = std::remove_cv_t<std::remove_reference_t<U>>;
		if constexpr (std::is_base_of_v<device_t, B>) m_base = &base;
		else if constexpr (std::is_base_of_v<device_interface, B>) m_base = &base.device();
	}
	template <typename U> void set_tag(U &&tag)
	{
		if constexpr (std::is_convertible_v<U, T *>) m_p = tag;
		else if constexpr (std::is_convertible_v<U, char const *> || std::is_convertible_v<U, std::string>) m_tag = std::string(tag);
	}
	void set(T *p) { m_p = p; }
	// MAME resolves finders by tag relative to the owning device; do the same lazily.
	T *target() const
	{
		if (!m_p && m_base && !m_tag.empty() && m_tag != finder_base::DUMMY_TAG)
			m_p = dynamic_cast<T *>(m_base->subdevice(m_tag));
		return m_p;
	}
	explicit operator bool() const { return target() != nullptr; }
	operator T *() const { return target(); }
	T *operator->() const { return target(); }
	T &operator*() const { return *target(); }
	char const *finder_tag() const { return m_tag.c_str(); }
private:
	mutable T *m_p = nullptr;
	device_t *m_base = nullptr;
	std::string m_tag;
};
template <class T> using optional_device = device_finder_shim<T>;
template <class T> using required_device = device_finder_shim<T>;
template <class T, unsigned N> class device_array_finder_shim
{
public:
	template <typename U, typename... A> device_array_finder_shim(U &&, A &&...) { }
	device_finder_shim<T> &operator[](unsigned i) { return m_e[i]; }
	device_finder_shim<T> const &operator[](unsigned i) const { return m_e[i]; }
private:
	std::array<device_finder_shim<T>, N> m_e;
};
template <class T, unsigned N> using optional_device_array = device_array_finder_shim<T, N>;
template <class T, unsigned N> using required_device_array = device_array_finder_shim<T, N>;

// slots: the host plugs cards directly
class device_slot_interface : public device_interface
{
public:
	device_slot_interface(machine_config const &, device_t &d) : device_interface(d, "slot") { }
	template <typename... A> void set_options(A &&...) { }
	template <typename... A> device_slot_interface &option_add(A &&...) { return *this; }
};
template <class Card> class device_single_card_slot_interface : public device_slot_interface
{
public:
	device_single_card_slot_interface(class machine_config const &mc, device_t &d) : device_slot_interface(mc, d) { }
	Card *get_card_device() const { return m_card; }
	void plug(Card *c) { m_card = c; }
private:
	Card *m_card = nullptr;
};

class devcb_write_line
{
public:
	devcb_write_line() = default;
	template <typename U> explicit devcb_write_line(U &&) { }
	template <unsigned N> class array
	{
	public:
		template <typename U> explicit array(U &&) { }
		devcb_write_line &operator[](unsigned i) { return m_e[i]; }
		devcb_write_line const &operator[](unsigned i) const { return m_e[i]; }
	private:
		std::array<devcb_write_line, N> m_e;
	};
	// bind() must hand back something that writes through (callers use `auto x() { return m_cb.bind(); }`)
	struct binder { devcb_write_line *p; void set(std::function<void(int)> f) { p->m_f = std::move(f); } };
	binder bind() { return binder{ this }; }
	void set(std::function<void(int)> f) { m_f = std::move(f); }
	void operator()(int state) { if (m_f) m_f(state); }
	void resolve_safe(int) { }
	bool isunset() const { return !m_f; }
private:
	std::function<void(int)> m_f;
};
class devcb_read_line {
public:
 template<class U> devcb_read_line(U &,int d=0):value(d){}
 bool isunset() const { return !fn; }
 int operator()() const { return fn?fn():value; }
 struct binder { devcb_read_line *p; void set(std::function<int()> f){p->fn=std::move(f);} };
 binder bind(){return {this};}
private: int value;std::function<int()> fn;
};
class devcb_write8
{
public:
	template <typename U> explicit devcb_write8(U &&) { }
	struct binder { devcb_write8 *p; void set(std::function<void(offs_t, u8)> f) { p->m_f = std::move(f); } };
	binder bind() { return binder{ this }; }
	void set(std::function<void(offs_t, u8)> f) { m_f = std::move(f); }
	void operator()(offs_t o, u8 d) { if (m_f) m_f(o, d); }
	void operator()(u8 d) { if (m_f) m_f(0, d); }
	bool isunset() const { return !m_f; }
private:
	std::function<void(offs_t, u8)> m_f;
};

class address_space_config
{
public:
	address_space_config(char const *name, endianness_t endian, u8 datawidth, u8 addrwidth, s8 addrshift = 0)
		: m_name(name), m_endian(endian), m_data_width(datawidth), m_addr_width(addrwidth), m_addr_shift(addrshift) { }
	char const *m_name; endianness_t m_endian; u8 m_data_width; u8 m_addr_width; s8 m_addr_shift;
};

// MAME memory_access<...>::specific, reduced to a 16-bit little-endian bus (NS32016).  Wider accesses are split into
// ascending word cycles; unaligned words into two byte-lane cycles — the same handler-visible behaviour as MAME.
template <int HighBits, int Width, int AddrShift, endianness_t Endian> struct memory_access
{
	class specific
	{
	public:
		address_space &space() const { return *m_space; }
		u8 read_byte(offs_t a) { return u8(m_space->read_word(a & ~1, (a & 1) ? 0xff00 : 0x00ff) >> ((a & 1) * 8)); }
		u16 read_word(offs_t a) { return (a & 1) ? read_word_unaligned(a) : m_space->read_word(a, 0xffff); }
		u16 read_word(offs_t a, u16 mask) { return m_space->read_word(a, mask); }
		u32 read_dword(offs_t a) { return u32(read_word(a)) | (u32(read_word(a + 2)) << 16); }
		u32 read_dword(offs_t a, u32 mask)
		{
			u32 r = 0;
			if (mask & 0xffff) r |= m_space->read_word(a, u16(mask));
			if (mask >> 16) r |= u32(m_space->read_word(a + 2, u16(mask >> 16))) << 16;
			return r;
		}
		u64 read_qword(offs_t a) { return u64(read_dword(a)) | (u64(read_dword(a + 4)) << 32); }
		u64 read_qword(offs_t a, u64 mask) { return u64(read_dword(a, u32(mask))) | (u64(read_dword(a + 4, u32(mask >> 32))) << 32); }
		u16 read_word_unaligned(offs_t a) { return (a & 1) ? u16(read_byte(a) | (read_byte(a + 1) << 8)) : read_word(a); }
		u32 read_dword_unaligned(offs_t a) { return u32(read_word_unaligned(a)) | (u32(read_word_unaligned(a + 2)) << 16); }
		u64 read_qword_unaligned(offs_t a) { return u64(read_dword_unaligned(a)) | (u64(read_dword_unaligned(a + 4)) << 32); }

		void write_byte(offs_t a, u8 d) { m_space->write_word(a & ~1, u16(d) << ((a & 1) * 8), (a & 1) ? 0xff00 : 0x00ff); }
		void write_word(offs_t a, u16 d) { if (a & 1) write_word_unaligned(a, d); else m_space->write_word(a, d, 0xffff); }
		void write_word(offs_t a, u16 d, u16 mask) { m_space->write_word(a, d, mask); }
		void write_dword(offs_t a, u32 d) { write_word(a, u16(d)); write_word(a + 2, u16(d >> 16)); }
		void write_dword(offs_t a, u32 d, u32 mask)
		{
			if (mask & 0xffff) m_space->write_word(a, u16(d), u16(mask));
			if (mask >> 16) m_space->write_word(a + 2, u16(d >> 16), u16(mask >> 16));
		}
		void write_qword(offs_t a, u64 d) { write_dword(a, u32(d)); write_dword(a + 4, u32(d >> 32)); }
		void write_qword(offs_t a, u64 d, u64 mask) { write_dword(a, u32(d), u32(mask)); write_dword(a + 4, u32(d >> 32), u32(mask >> 32)); }
		void write_word_unaligned(offs_t a, u16 d) { if (a & 1) { write_byte(a, u8(d)); write_byte(a + 1, u8(d >> 8)); } else write_word(a, d); }
		void write_dword_unaligned(offs_t a, u32 d) { write_word_unaligned(a, u16(d)); write_word_unaligned(a + 2, u16(d >> 16)); }
		void write_qword_unaligned(offs_t a, u64 d) { write_dword_unaligned(a, u32(d)); write_dword_unaligned(a + 4, u32(d >> 32)); }

		address_space *m_space = nullptr;
	};
};

// ---- device_state_interface ----
class device_state_entry
{
public:
	device_state_entry &noshow() { return *this; }
	device_state_entry &mask(u64) { return *this; }
	device_state_entry &formatstr(char const *) { return *this; }
	device_state_entry &callimport() { return *this; }
	device_state_entry &callexport() { return *this; }
	int index() const { return m_index; }
	int m_index = 0;
	std::string m_name;
	std::function<u64()> m_get;  // debug read-back (host --probe)
};

class device_state_interface
{
public:
	virtual ~device_state_interface() = default;
	template <typename T> device_state_entry &state_add(int index, std::string name, T &v)
	{
		std::function<u64()> g;
		if constexpr (std::is_arithmetic_v<T>) g = [&v] { return u64(v); };
		m_entries.push_back({index, name, g});
		return m_entries.back();
	}
	u64 state_value(std::string const &name) const { for (auto const &e : m_entries) if (e.m_name == name && e.m_get) return e.m_get(); return 0; }
	template <typename T> device_state_entry &state_add(int index, char const *name, T &v) { return state_add(index, std::string(name), v); }
	virtual void state_string_export(device_state_entry const &, std::string &) const { }
protected:
	std::vector<device_state_entry> m_entries;
};

class device_memory_interface
{
public:
	using space_config_vector = std::vector<std::pair<int, address_space_config const *>>;
	virtual ~device_memory_interface() = default;
	virtual space_config_vector memory_space_config() const = 0;
	virtual bool memory_translate(int, int, offs_t &, address_space *&) { return true; }
	bool has_configured_map(int) const { return false; }
	address_space &space(int = 0) const { return *s_host_space; }
	static thread_local address_space *s_host_space;
};

extern thread_local void (*g_insn_hook)(offs_t);
class cpu_device : public device_t, public device_memory_interface, public device_state_interface
{
public:
	cpu_device(machine_config const &mconfig, device_type type, char const *tag, device_t *owner, u32 clock)
		: device_t(mconfig, type, tag, owner, clock), m_insn_hook(g_insn_hook) { }
	// run for (at least) 'cycles' cycles; MAME semantics: stops at the first instruction boundary at or after the budget
	void run(int cycles)
	{
		*m_icountptr = cycles; m_slice = cycles;
		shim_sched().m_cpu_running = true;
		execute_run();
		shim_sched().m_cpu_running = false;
		m_total += m_slice - *m_icountptr; m_slice = *m_icountptr;
	}
	// MAME device_execute_interface::abort_timeslice: finish the current instruction, keep the cycle count exact
	void abort_timeslice()
	{
		if (!m_icountptr) return;
		int const delta = *m_icountptr;
		if (delta > 0) { m_slice -= delta; *m_icountptr = 0; }
	}
	void set_input_line(int line, int state) { execute_set_input(line, state); }
	u64 total_cycles() const { return m_total + (m_slice - *m_icountptr); }
	attotime local_time() const { return attotime(attotime::T(total_cycles()) / attotime::T(clock())); }
	void adjust_icount(int delta) { *m_icountptr += delta; }
	int icount() const { return *m_icountptr; }
	int default_irq_vector(int) const { return 0; }
	offs_t state_pc() const { return m_pc_view; }
protected:
	void set_icountptr(int &icount) { m_icountptr = &icount; }
	void debugger_instruction_hook(offs_t pc) { m_pc_view = pc; if (m_insn_hook) m_insn_hook(pc); }
	void debugger_wait_hook() { }
	virtual u32 execute_min_cycles() const noexcept { return 1; }
	virtual u32 execute_max_cycles() const noexcept { return 1; }
	virtual void execute_run() = 0;
	virtual void execute_set_input(int, int) { }
	virtual bool execute_input_edge_triggered(int) const noexcept { return false; }
	virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return nullptr; }
private:
	int *m_icountptr = nullptr;
	void (*m_insn_hook)(offs_t);     // g_insn_hook of the constructing thread
	offs_t m_pc_view = 0;
	int m_slice = 0;
	u64 m_total = 0;
};

#endif
