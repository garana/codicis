#ifndef CODICIS_APP_OPTIONS_H
#define CODICIS_APP_OPTIONS_H

/**
 * @file options.h
 * @brief Declares the application's configuration options.
 */

#include "codicis/config/option.h"

namespace codicis {

/**
 * @brief Build the registry of all codicis configuration options.
 *
 * Each option is declared once here and is settable via a config file key or
 * the identically named CLI flag (CLI wins).
 * @return The populated option registry.
 */
OptionRegistry BuildOptionRegistry();

}  // namespace codicis

#endif  // CODICIS_APP_OPTIONS_H
