/* SPDX-License-Identifier: Zlib */

#ifndef ZATHURA_THEME_H
#define ZATHURA_THEME_H

#include "types.h"

/**
 * Follow the desktop theme selector (default: $XDG_CONFIG_HOME/themes/.caelestia-use).
 *
 * Colors are read from the selector's Colors.qml, falling back to kitty.conf, and applied to the color settings.
 * The selector, its symlink chain and the resolved theme directory are watched so that static theme switches, shell
 * mode switches and in-place rewrites of dynamic themes all repaint a running instance. Honors the settings
 * "theme-follow" and "theme-source". Safe to call again after either setting changes.
 *
 * @param zathura The zathura session
 */
void zathura_theme_init(zathura_t* zathura);

/**
 * Stop following the theme and release its resources.
 *
 * @param zathura The zathura session
 */
void zathura_theme_free(zathura_t* zathura);

#endif
