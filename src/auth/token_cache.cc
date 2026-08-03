/**
 * @file token_cache.cc
 * @brief Implementation of TokenCache (see token_cache.h).
 */

#include "codicis/auth/token_cache.h"

namespace codicis {

TokenCache::TokenCache(const Clock& clock, std::size_t capacity)
    : clock_(clock), capacity_(capacity) {}

TokenCache::~TokenCache() = default;

void TokenCache::unlink(Node* n) {
  if (n->prev != nullptr) {
    n->prev->next = n->next;
  } else {
    head_ = n->next;
  }
  if (n->next != nullptr) {
    n->next->prev = n->prev;
  } else {
    tail_ = n->prev;
  }
  n->prev = nullptr;
  n->next = nullptr;
}

void TokenCache::push_tail(Node* n) {
  n->prev = tail_;
  n->next = nullptr;
  if (tail_ != nullptr) {
    tail_->next = n;
  } else {
    head_ = n;
  }
  tail_ = n;
}

void TokenCache::erase(Node* n) {
  unlink(n);
  index_.erase(n->key);  // frees the Node (owned by the index)
}

void TokenCache::insert(const std::string& key, const std::string& value,
                        Nanos expiry_ns) {
  if (capacity_ == 0 || expiry_ns <= clock_.now()) {
    return;  // caching disabled, or the entry is already expired
  }
  const Nanos expiry = expiry_ns;

  // Replace an existing entry in place, refreshing value + expiry + position.
  if (const auto it = index_.find(key); it != index_.end()) {
    Node* n = it->second.get();
    n->value = value;
    n->expiry = expiry;
    unlink(n);
    push_tail(n);
    return;
  }

  // A new key: make room at the tail (cold end) before admitting.
  if (index_.size() >= capacity_ && tail_ != nullptr) {
    erase(tail_);
  }

  auto node = std::make_unique<Node>();
  node->key = key;
  node->value = value;
  node->expiry = expiry;
  Node* raw = node.get();
  index_.emplace(key, std::move(node));
  push_tail(raw);
}

const std::string* TokenCache::lookup(const std::string& key) {
  const auto it = index_.find(key);
  if (it == index_.end()) {
    return nullptr;
  }
  Node* n = it->second.get();
  if (clock_.now() >= n->expiry) {
    erase(n);  // expired
    return nullptr;
  }
  // Promote one step toward the head: swap with the predecessor.
  if (n->prev != nullptr) {
    Node* p = n->prev;
    unlink(n);
    // Re-insert n immediately before p.
    n->next = p;
    n->prev = p->prev;
    if (p->prev != nullptr) {
      p->prev->next = n;
    } else {
      head_ = n;
    }
    p->prev = n;
  }
  return &n->value;
}

}  // namespace codicis
