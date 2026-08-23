#pragma once
/*
 * json_chunked.h -- Buffer-free chunked JSON streaming for ESPAsyncWebServer.
 *
 * Overview
 * --------
 * Provides stateful writer objects and helper functions for streaming
 * arbitrarily large JSON arrays and objects over HTTP in chunks, without
 * allocating a full serialization buffer.  All state is held in the writer
 * objects themselves; sendChunked callbacks capture them by value.
 *
 * Core types (namespace json_chunked)
 * ------------------------------------
 *  WriteResult  -- return value for every writer call.
 *    {false, 0}   no room; retry with the same (or larger) buffer
 *    {false, n}   wrote n bytes; sub-writer not finished -- stop filling the
 *                 current buffer and flush what has been collected so far
 *    {true,  n}   wrote n final bytes; writer is done, caller may advance
 *    {true,  0}   writer already finished, or item is empty/skipped.
 *                 In list context (factory form) a {true,0} on the first call
 *                 silently omits the item -- no separator is emitted.
 *
 *  Element  -- unified type for all serialization slots: list items, object
 *            keys, and object values.  Implicitly constructs from:
 *              const char*                -> JSON-quoted string
 *              String / __FlashStringHelper*  -> JSON-quoted string (copied)
 *              any integral type (not bool) -> unquoted decimal integer
 *              bool                       -> true / false literal
 *              JsonVariant                -> ChunkPrint re-serialization
 *              any (uint8_t*,size_t)->WriteResult callable
 *                                         -> wrapped directly (JSONListWriter,
 *                                           JSONObjectWriter, lambdas, ...)
 *            For cases not covered (PROGMEM raw bytes, integer object keys),
 *            use the explicit factory functions below.
 *
 *  KeyValuePair  -- {Element key; Element value}; one JSON object entry.
 *
 * Explicit factory functions
 * --------------------------
 *  makeProgmemRawWriter(s)  PROGMEM byte sequence streamed verbatim
 *                           (already-serialized JSON; distinct from const char*
 *                           which goes through JSON string escaping)
 *
 * Stateful composers
 * ------------------
 *  writeJSONList(begin, end, cb)
 *    cb is either:
 *      (Iterator, uint8_t*, size_t) -> size_t   direct: cb writes one item
 *      (Iterator) -> Element                      factory: one writer per item
 *    In the factory form, returning an Element whose first call returns {true,0}
 *    silently skips that entry -- useful for filtering sparse ranges without
 *    disturbing separators.
 *
 *  writeJSONObject(begin, end, makeItem)
 *    makeItem: (Iterator) -> KeyValuePair
 *
 *  writeJSONObject({kvpair, kvpair, ...})
 *    Initializer-list form; items are evaluated at the call site (eager Element
 *    construction, lazy byte-writing).  Returns Element so the result can be
 *    used as a nested value or passed to respondJSONChunked.
 *
 * Sending responses
 * -----------------
 *  respondJSONChunked(request, Element)
 *    Core responder: pipes any Element into request->sendChunked.  Use this
 *    directly when you already have an Element (e.g. from writeJSONObject or
 *    writeJSONList) or when composing writers by hand.
 *
 *  respondJSONList / respondJSONObject
 *    Convenience wrappers: construct the writer and call respondJSONChunked.
 *    respondJSONObject also has an initializer-list overload.
 *
 * Lifetime
 * --------
 * The sendChunked lambda captures the outer writer by value.  Any per-item
 * state (shared_ptr to heap data, captured references to stable globals, raw
 * PROGMEM pointers) is held inside Element closures; only one item writer is
 * live at a time.  No global JSON buffer lock is needed for these endpoints.
 *
 *
 * Developed by Claude Sonnet, guided by Will Miles <will@willmiles.net>.
 */

#include <functional>
#include <initializer_list>
#include <type_traits>
#include <valarray>
#include <new>
#include <cstring>
#include <cstdio>
#include <ESPAsyncWebServer.h>
#include "src/dependencies/json/ArduinoJson-v6.h"
#include "src/dependencies/json/AsyncJson-v6.h"   // ChunkPrint

#if __cplusplus >= 201402L
  #define JC_CAPTURE_BY_MOVE(x) x = std::move(x)
#else
  #define JC_CAPTURE_BY_MOVE(x) x
#endif


namespace json_chunked {

// Writes a JSON-escaped quoted string into dest[0..maxLen-1].
// Returns bytes written, or 0 if the buffer was too small.
inline size_t quoteJsonString(uint8_t* dest, size_t maxLen, const char* src) {
  size_t pos = 0;
  auto emit = [&](char c) -> bool {
    if (pos >= maxLen) return false;
    dest[pos++] = static_cast<uint8_t>(c);
    return true;
  };
  if (!emit('"')) return 0;
  for (const char* p = src; *p; ++p) {
    char esc = 0;
    switch (*p) {
      case '"':  esc = '"';  break;
      case '\\': esc = '\\'; break;
      case '\b': esc = 'b';  break;
      case '\f': esc = 'f';  break;
      case '\n': esc = 'n';  break;
      case '\r': esc = 'r';  break;
      case '\t': esc = 't';  break;
    }
    if (esc) {
      if (!emit('\\') || !emit(esc)) return 0;
    } else {
      if (!emit(*p)) return 0;
    }
  }
  if (!emit('"')) return 0;
  return pos;
}

// Return type from element writing functions.
struct WriteResult {
  bool   done;
  size_t count;
};


// C++11 substitute for std::is_invocable_r<WriteResult, F, uint8_t*, size_t>.
template<typename F, typename = void>
struct is_element_fn : std::false_type {};

template<typename F>
struct is_element_fn<F, typename std::enable_if<
    std::is_same<
      decltype(std::declval<F>()(std::declval<uint8_t*>(), std::declval<size_t>())),
      WriteResult
    >::value
  >::type> : std::true_type {};


// Lightweight owning type-erasure for Element callables.
// Trivially-copyable targets that fit in N bytes are stored inline (no heap,
// no per-type manager code).  Larger or non-trivial targets fall back to heap.
template<typename Sig>
class inplace_fn;

template<typename Ret, typename... Args>
class inplace_fn<Ret(Args...)> {
  static const size_t N = 24;

  struct Ops {
    void* (*clone)(const void*);
    void  (*destroy)(void*);
  };

  union {
    typename std::aligned_storage<N, alignof(long long)>::type buf_;
    void* ptr_;
  };
  Ret       (*invoke_)(const void*, Args...);
  const Ops*  ops_;

  const void* target() const { return ops_ ? ptr_ : static_cast<const void*>(&buf_); }

  template<typename Fn> static Ret invoke_thunk(const void* p, Args... args) {
    return (*const_cast<Fn*>(static_cast<const Fn*>(p)))(static_cast<Args>(args)...);
  }
  template<typename Fn> static void* clone_thunk(const void* p) { return new Fn(*static_cast<const Fn*>(p)); }
  template<typename Fn> static void  destroy_thunk(void* p)     { delete static_cast<Fn*>(p); }
  template<typename Fn> static const Ops* heap_ops() {
    static const Ops o{ &clone_thunk<Fn>, &destroy_thunk<Fn> };
    return &o;
  }

  template<typename Fn>
  using fits_inline = std::integral_constant<bool,
      (sizeof(Fn) <= N) && std::is_trivially_copyable<Fn>::value
                        && std::is_trivially_destructible<Fn>::value>;

  template<typename Fn> void emplace(Fn&& f, std::true_type /*inline*/) {
    using D = typename std::decay<Fn>::type;
    new (&buf_) D(std::forward<Fn>(f));
    invoke_ = &invoke_thunk<D>;
    ops_    = nullptr;
  }
  template<typename Fn> void emplace(Fn&& f, std::false_type /*heap*/) {
    using D = typename std::decay<Fn>::type;
    ptr_    = new D(std::forward<Fn>(f));
    invoke_ = &invoke_thunk<D>;
    ops_    = heap_ops<D>();
  }

  void reset() noexcept { if (ops_) ops_->destroy(ptr_); invoke_ = nullptr; ops_ = nullptr; }
  void swap_storage(inplace_fn& o) noexcept {
    std::swap(invoke_, o.invoke_);
    std::swap(ops_, o.ops_);
    typename std::aligned_storage<N, alignof(long long)>::type t;
    std::memcpy(&t, &buf_, N); std::memcpy(&buf_, &o.buf_, N); std::memcpy(&o.buf_, &t, N);
  }

public:
  inplace_fn() noexcept : invoke_(nullptr), ops_(nullptr) {}

  template<typename Fn, typename = typename std::enable_if<
      !std::is_same<typename std::decay<Fn>::type, inplace_fn>::value>::type>
  inplace_fn(Fn&& f) { emplace(std::forward<Fn>(f), fits_inline<typename std::decay<Fn>::type>{}); }

  inplace_fn(const inplace_fn& o) : invoke_(o.invoke_), ops_(o.ops_) {
    if (ops_) ptr_ = ops_->clone(o.ptr_);
    else      buf_ = o.buf_;
  }
  inplace_fn(inplace_fn&& o) noexcept : invoke_(o.invoke_), ops_(o.ops_) {
    if (ops_) { ptr_ = o.ptr_; o.invoke_ = nullptr; o.ops_ = nullptr; }
    else        buf_ = o.buf_;
  }
  inplace_fn& operator=(const inplace_fn& o) { inplace_fn t(o); swap_storage(t); return *this; }
  inplace_fn& operator=(inplace_fn&& o) noexcept { inplace_fn t(std::move(o)); swap_storage(t); return *this; }

  template<typename Fn, typename = typename std::enable_if<
      !std::is_same<typename std::decay<Fn>::type, inplace_fn>::value>::type>
  inplace_fn& operator=(Fn&& f) { inplace_fn t(std::forward<Fn>(f)); swap_storage(t); return *this; }

  ~inplace_fn() { reset(); }

  Ret operator()(Args... args) const { return invoke_(target(), static_cast<Args>(args)...); }
  explicit operator bool() const noexcept { return invoke_ != nullptr; }
};


// Unified serialization slot type.  See file header for implicit conversions.
struct Element {
  inplace_fn<WriteResult(uint8_t*, size_t)> fn;

  Element() = default;
  Element(const Element&) = default;
  Element(Element&&) = default;
  Element& operator=(const Element&) = default;
  Element& operator=(Element&&) = default;

  // From any callable with the Element signature
  template<typename F,
    typename = typename std::enable_if<
      is_element_fn<typename std::decay<F>::type>::value
      && !std::is_same<typename std::decay<F>::type, Element>::value
    >::type>
  Element(F&& f) : fn(std::forward<F>(f)) {}

  // const char* -> JSON-quoted string (captures pointer; safe for literals)
  Element(const char* s) {
    bool written = false;
    fn = [s, written](uint8_t* buf, size_t len) mutable -> WriteResult {
      size_t n = 0;
      if (!written) {
        n = quoteJsonString(buf, len, s);
        written = (n>0);
      }
      return {written, n};
    };
  }

  // String -> JSON-quoted string (copies into closure)
  Element(String s) {
    bool written = false;
    fn = [s, written](uint8_t* buf, size_t len) mutable -> WriteResult {
      if (written) return {true, 0};
      size_t n = quoteJsonString(buf, len, s.c_str());
      if (n) { written = true; return {true, n}; }
      return {false, 0};
    };
  }

  // __FlashStringHelper* -> copies to String, then JSON-quoted
  Element(const __FlashStringHelper* fs) : Element(String(fs)) {}

  // Any integer type (not bool) -> unquoted decimal
  template<typename T,
    typename = typename std::enable_if<
      std::is_integral<T>::value && !std::is_same<T, bool>::value
    >::type>
  Element(T v) : Element(static_cast<int32_t>(v)) {}

  // bool -> true / false JSON literal
  Element(bool v) {
    const char* s = v ? "true" : "false";
    size_t      n = v ? 4 : 5;
    bool written = false;
    fn = [s, n, written](uint8_t* buf, size_t len) mutable -> WriteResult {
      if (written) return {true, 0};
      if (n > len)  return {false, 0};
      memcpy(buf, s, n);
      written = true;
      return {true, n};
    };
  }

  // int32_t -> unquoted decimal integer
  Element(int32_t v) {
    bool written = false;
    fn = [v, written](uint8_t* buf, size_t len) mutable -> WriteResult {
      if (written) return {true, 0};
      char tmp[12];
      int n = snprintf(tmp, sizeof(tmp), "%ld", (long)v);
      if (n < 0) { written = true; return {true, 0}; }
      if ((size_t)n > len) return {false, 0};
      memcpy(buf, tmp, n);
      written = true;
      return {true, (size_t)n};
    };
  }

  // JsonVariant -> ChunkPrint re-serialization (variant must outlive the writer)
  Element(JsonVariant v) {
    size_t total = measureJson(v);
    size_t sent  = 0;
    fn = [v, total, sent](uint8_t* buf, size_t maxLen) mutable -> WriteResult {
      if (sent >= total) return {true, 0};
      size_t n = total - sent < maxLen ? total - sent : maxLen;
      ChunkPrint cp(buf, sent, n);
      serializeJson(v, cp);
      sent += n;
      return {sent >= total, n};
    };
  }

  WriteResult operator()(uint8_t* buf, size_t len) const { return fn(buf, len); }
  explicit operator bool() const { return bool(fn); }
};


// Streams raw PROGMEM bytes verbatim (pre-serialized JSON)
inline Element makeProgmemRawWriter(const char* src) {
  size_t total = strlen_P(src);
  size_t sent  = 0;
  return Element([src, total, sent](uint8_t* buf, size_t maxLen) mutable -> WriteResult {
    if (sent >= total) return {true, 0};
    size_t n = total - sent < maxLen ? total - sent : maxLen;
    memcpy_P(buf, src + sent, n);
    sent += n;
    return {sent >= total, n};
  });
}


// One JSON object entry.
struct KeyValuePair {
  Element key;
  Element value;
};


// Detects factory vs direct callback form.
template<typename Callback, typename Iterator, typename = void>
struct is_item_factory : std::false_type {};

template<typename Callback, typename Iterator>
struct is_item_factory<Callback, Iterator,
  decltype(void(std::declval<Callback>()(std::declval<Iterator>())))>
  : std::true_type {};


// Direct-callback list writer.
// Callback: (Iterator, uint8_t*, size_t) -> size_t
template<typename Iterator, typename Callback>
struct JSONListWriter {
  Iterator current, begin_val, end_val;
  Callback cb;
  bool done;

  JSONListWriter(Iterator begin, Iterator end, Callback cb_)
    : current(begin), begin_val(begin), end_val(end), cb(cb_), done(false) {}

  WriteResult operator()(uint8_t* dest, size_t maxLen) {
    if (done) return {true, 0};
    size_t pos = 0;

    while (current != end_val) {
      if (pos + 2 > maxLen) break;
      size_t n = cb(current, dest + pos + 1, maxLen - pos - 1);
      if (n == 0) break;
      dest[pos] = (current == begin_val) ? '[' : ',';
      pos += 1 + n;
      ++current;
    }

    if (current == end_val) {
      const size_t need = (current == begin_val) ? 2 : 1;
      if (pos + need <= maxLen) {
        if (current == begin_val) dest[pos++] = '[';
        dest[pos++] = ']';
        done = true;
      }
    }

    return {done, pos};
  }
};


// Factory-callback list writer.
// MakeItem: (Iterator) -> Element
template<typename Iterator, typename MakeItem>
struct JSONListFactoryWriter {
  Iterator current, end_val;
  MakeItem makeItem;
  Element   item;
  bool     first, done, itemActive;

  JSONListFactoryWriter(Iterator begin, Iterator end, MakeItem makeItem_)
    : current(begin), end_val(end), makeItem(makeItem_),
      first(true), done(false), itemActive(false) {}

  WriteResult operator()(uint8_t* dest, size_t maxLen) {
    if (done) return {true, 0};
    size_t pos = 0;

    while (current != end_val) {
      if (!item) {
        item       = makeItem(current);
        itemActive = false;
      }

      const size_t sepReserve = itemActive ? 0 : 1;
      if (pos + sepReserve + 1 > maxLen) break;

      WriteResult ir = item(dest + pos + sepReserve, maxLen - pos - sepReserve);

      if (ir.count > 0 && !itemActive) {
        dest[pos] = first ? '[' : ',';
        first      = false;
        pos       += 1;
        itemActive = true;
      }
      pos += ir.count;

      if (ir.done) {
        item       = Element();
        ++current;
        itemActive = false;
      } else {
        break;
      }
    }

    if (current == end_val) {
      const size_t need = first ? 2 : 1;
      if (pos + need <= maxLen) {
        if (first) dest[pos++] = '[';
        dest[pos++] = ']';
        done = true;
      }
    }

    return {done, pos};
  }
};


// writeJSONList -- direct callback form
template<typename Iterator, typename Callback>
typename std::enable_if<
  !is_item_factory<Callback, Iterator>::value,
  JSONListWriter<Iterator, Callback>
>::type
writeJSONList(Iterator begin, Iterator end, Callback cb) {
  return JSONListWriter<Iterator, Callback>(begin, end, cb);
}

// writeJSONList -- factory callback form
template<typename Iterator, typename Callback>
typename std::enable_if<
  is_item_factory<Callback, Iterator>::value,
  JSONListFactoryWriter<Iterator, Callback>
>::type
writeJSONList(Iterator begin, Iterator end, Callback cb) {
  return JSONListFactoryWriter<Iterator, Callback>(begin, end, cb);
}


// Object writer.  MakeItem: (Iterator) -> KeyValuePair
template<typename Iterator>
struct JSONObjectWriter {
  enum class Phase : uint8_t { NeedItem, Sep, Key, Colon, Value };
  typedef std::function<KeyValuePair(Iterator)> MakeItem;

  Iterator current, end_val;
  MakeItem makeItem;
  Element   keyWriter, valueWriter;
  Phase    phase;
  bool     first, done;

  JSONObjectWriter(Iterator begin, Iterator end, MakeItem mi)
    : current(begin), end_val(end), makeItem(std::move(mi)),
      phase(Phase::NeedItem), first(true), done(false) {}

  WriteResult operator()(uint8_t* dest, size_t maxLen) {
    if (done) return {true, 0};
    size_t pos = 0;

    while (pos < maxLen) {
      if (current == end_val) {
        const size_t need = first ? 2 : 1;
        if (pos + need > maxLen) break;
        if (first) dest[pos++] = '{';
        dest[pos++] = '}';
        done = true;
        break;
      }

      switch (phase) {
        case Phase::NeedItem: {
          auto kv    = makeItem(current);
          keyWriter   = std::move(kv.key);
          valueWriter = std::move(kv.value);
          phase = Phase::Sep;
          continue;
        }
        case Phase::Sep:
          dest[pos++] = first ? '{' : ',';
          first = false;
          phase = Phase::Key;
          continue;
        case Phase::Key: {
          WriteResult kr = keyWriter(dest + pos, maxLen - pos);
          pos += kr.count;
          if (kr.done) { phase = Phase::Colon; continue; }
          goto exit_loop;
        }
        case Phase::Colon:
          dest[pos++] = ':';
          phase = Phase::Value;
          continue;
        case Phase::Value: {
          WriteResult vr = valueWriter(dest + pos, maxLen - pos);
          pos += vr.count;
          if (vr.done) { ++current; phase = Phase::NeedItem; continue; }
          goto exit_loop;
        }
      }
    }
    exit_loop:

    if (current == end_val && !done) {
      const size_t need = first ? 2 : 1;
      if (pos + need <= maxLen) {
        if (first) dest[pos++] = '{';
        dest[pos++] = '}';
        done = true;
      }
    }

    return {done, pos};
  }
};


// writeJSONObject -- factory form
template<typename Iterator, typename MakeItem>
JSONObjectWriter<Iterator>
writeJSONObject(Iterator begin, Iterator end, MakeItem mi) {
  return JSONObjectWriter<Iterator>(begin, end, std::move(mi));
}

// writeJSONObject -- two-callback form
template<typename Iterator, typename KeyCb, typename ValueCb>
JSONObjectWriter<Iterator>
writeJSONObject(Iterator begin, Iterator end, KeyCb keyCb, ValueCb valCb) {
  return JSONObjectWriter<Iterator>(begin, end,
    [keyCb, valCb](Iterator it) -> KeyValuePair {
      return {
        [keyCb, it](uint8_t* buf, size_t len) -> WriteResult {
          size_t n = keyCb(it, buf, len);
          return {n > 0, n};
        },
        [valCb, it](uint8_t* buf, size_t len) -> WriteResult {
          size_t n = valCb(it, buf, len);
          return {n > 0, n};
        }
      };
    });
}


// writeJSONObject -- initializer-list form
inline Element writeJSONObject(std::initializer_list<KeyValuePair> items) {
  std::valarray<KeyValuePair> vec(items);
  return writeJSONObject(size_t{0}, vec.size(),
      [JC_CAPTURE_BY_MOVE(vec)](size_t i) -> KeyValuePair { return std::move(vec[i]); });
}

// Core responder: pipes any Element into request->sendChunked.
void respondJSONChunked(AsyncWebServerRequest* request, Element writer) {
  request->sendChunked(FPSTR(CONTENT_TYPE_JSON),
    [JC_CAPTURE_BY_MOVE(writer)] (uint8_t* data, size_t len, size_t) mutable -> size_t {
      WriteResult r = writer(data, len);
      return r.count;
    });
}

// Convenience: list response
template<typename Iterator, typename Callback>
void respondJSONList(AsyncWebServerRequest* request, Iterator begin, Iterator end, Callback cb) {
  respondJSONChunked(request, writeJSONList(begin, end, cb));
}

// Convenience: object response (factory form)
template<typename Iterator, typename MakeItem>
void respondJSONObject(AsyncWebServerRequest* request, Iterator begin, Iterator end, MakeItem mi) {
  respondJSONChunked(request, writeJSONObject(begin, end, mi));
}

// Convenience: object response (two-callback form)
template<typename Iterator, typename KeyCb, typename ValueCb>
void respondJSONObject(AsyncWebServerRequest* request, Iterator begin, Iterator end, KeyCb keyCb, ValueCb valCb) {
  respondJSONChunked(request, writeJSONObject(begin, end, keyCb, valCb));
}

// Convenience: object response (initializer-list form)
inline void respondJSONObject(AsyncWebServerRequest* request, std::initializer_list<KeyValuePair> items) {
  respondJSONChunked(request, writeJSONObject(items));
}


} // namespace json_chunked
