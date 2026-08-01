/**
 * @file order.cc
 * @brief Implementation of order normalization (see order.h).
 */

#include "codicis/core/order.h"

namespace codicis {

void Normalize(Order* order) {
  // Collapse convenience TIFs onto the canonical (type, tif, flags) axes so
  // the matcher only ever sees GTC/DAY/GTD/IOC.
  if (order->tif == Tif::GTX) {
    SetFlag(&order->flags, OrderFlag::kPostOnly);
    order->tif = Tif::GTC;
  } else if (order->tif == Tif::FOK) {
    SetFlag(&order->flags, OrderFlag::kAon);
    order->tif = Tif::IOC;
  }

  if (order->leaves == 0 && order->filled == 0) {
    order->leaves = order->qty;
  }
  if (order->display_qty == 0) {
    order->display_qty = order->qty;
  }
}

}  // namespace codicis
