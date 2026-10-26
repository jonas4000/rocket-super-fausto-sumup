#include "SaleJson.h"

#include <cstdio>

namespace grinder {

bool formatSaleJson(char* buf, size_t len, const Dose& dose, bool refunded) {
  int n = std::snprintf(buf, len, "{\"dose\":\"%s\",\"price_cents\":%u,\"refunded\":%s}",
                        dose.name, static_cast<unsigned>(dose.priceCents),
                        refunded ? "true" : "false");
  return n > 0 && static_cast<size_t>(n) < len;
}

}  // namespace grinder
