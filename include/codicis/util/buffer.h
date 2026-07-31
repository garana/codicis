#ifndef CODICIS_UTIL_BUFFER_H
#define CODICIS_UTIL_BUFFER_H

/**
 * @file buffer.h
 * @brief A growable byte buffer with FIFO append/consume semantics.
 *
 * @ref codicis::Buffer backs non-blocking socket and pipe I/O: bytes are
 * appended at the tail (from reads or when composing a message) and consumed
 * from the head as they are parsed or written out. A read offset avoids
 * shifting data on every consume; storage is compacted lazily.
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

namespace codicis {

/**
 * @brief A contiguous, growable byte buffer with head/tail cursors.
 */
class Buffer {
 public:
  Buffer() = default;

  /**
   * @brief Number of unconsumed (readable) bytes.
   * @return The count of bytes between the read and write cursors.
   */
  std::size_t size() const { return write_pos_ - read_pos_; }

  /** @return True if there are no unconsumed bytes. */
  bool empty() const { return size() == 0; }

  /**
   * @brief Pointer to the first unconsumed byte.
   * @return Read pointer, valid for size() bytes; null if empty.
   */
  const std::uint8_t* data() const {
    return storage_.data() + read_pos_;
  }

  /**
   * @brief View the unconsumed bytes without copying.
   * @return A view valid until the next mutating call.
   */
  std::string_view view() const {
    return std::string_view(
        reinterpret_cast<const char*>(data()), size());
  }

  /**
   * @brief Append @p len bytes from @p src to the tail.
   * @param src Source bytes.
   * @param len Number of bytes to copy.
   */
  void append(const void* src, std::size_t len) {
    if (len == 0) {
      return;
    }
    reserve_tail(len);
    std::memcpy(storage_.data() + write_pos_, src, len);
    write_pos_ += len;
  }

  /**
   * @brief Append the contents of @p sv to the tail.
   * @param sv Source bytes.
   */
  void append(std::string_view sv) { append(sv.data(), sv.size()); }

  /**
   * @brief Reserve @p len writable bytes at the tail and return a pointer.
   *
   * The caller writes up to @p len bytes there and then calls @ref commit
   * with the number actually written. Useful for scatter-free socket reads.
   * @param len Minimum number of writable bytes required.
   * @return Pointer to writable storage of at least @p len bytes.
   */
  std::uint8_t* reserve(std::size_t len) {
    reserve_tail(len);
    return storage_.data() + write_pos_;
  }

  /**
   * @brief Mark @p len bytes previously obtained via @ref reserve as written.
   * @param len Number of bytes now valid at the tail.
   */
  void commit(std::size_t len) { write_pos_ += len; }

  /**
   * @brief Consume (discard) @p len bytes from the head.
   * @param len Number of bytes to drop; clamped to size().
   */
  void consume(std::size_t len) {
    if (len >= size()) {
      clear();
      return;
    }
    read_pos_ += len;
    maybe_compact();
  }

  /** @brief Drop all unconsumed bytes and reset cursors. */
  void clear() {
    read_pos_ = 0;
    write_pos_ = 0;
  }

 private:
  /** @brief Ensure at least @p len writable bytes exist at the tail. */
  void reserve_tail(std::size_t len) {
    if (storage_.size() - write_pos_ >= len) {
      return;
    }
    // Reclaim already-consumed head space before growing.
    if (read_pos_ > 0) {
      compact();
    }
    if (storage_.size() - write_pos_ < len) {
      storage_.resize(write_pos_ + len);
    }
  }

  /** @brief Move unconsumed bytes to the front when the head grows large. */
  void maybe_compact() {
    if (read_pos_ > 0 && read_pos_ >= storage_.size() / 2) {
      compact();
    }
  }

  /** @brief Shift unconsumed bytes to offset zero. */
  void compact() {
    const std::size_t n = size();
    if (n > 0 && read_pos_ > 0) {
      std::memmove(storage_.data(), storage_.data() + read_pos_, n);
    }
    read_pos_ = 0;
    write_pos_ = n;
  }

  std::vector<std::uint8_t> storage_;
  std::size_t read_pos_ = 0;
  std::size_t write_pos_ = 0;
};

}  // namespace codicis

#endif  // CODICIS_UTIL_BUFFER_H
