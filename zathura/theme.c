/* SPDX-License-Identifier: Zlib */

#include "theme.h"

#include <girara-gtk/settings.h>
#include <girara/log.h>
#include <gio/gio.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <string.h>

#include "macros.h"
#include "zathura.h"

#define LENGTH(x) (sizeof(x) / sizeof((x)[0]))
#define THEME_SHADE_PEAK 0.07
#define THEME_DEBOUNCE_MS 150
#define THEME_MAX_FILE_SIZE (1024 * 1024)

typedef enum {
  C_BACKGROUND,
  C_BACKGROUND_ALT,
  C_BACKGROUND_GRAY,
  C_FOREGROUND,
  C_FOREGROUND_INACTIVE,
  C_PRIMARY,
  C_MUTED,
  C_SECONDARY,
  C_SUCCESS,
  C_WARNING,
  C_DANGER,
  C_SHADE, /**< Near-black derived from the background, for the area around pages */
  C_COUNT
} theme_color_t;

typedef struct {
  GdkRGBA color[C_COUNT];
  bool have[C_COUNT];
} theme_palette_t;

static const char* const qml_names[C_COUNT] = {
    [C_BACKGROUND]          = "background",
    [C_BACKGROUND_ALT]      = "backgroundAlt",
    [C_BACKGROUND_GRAY]     = "backgroundGray",
    [C_FOREGROUND]          = "foreground",
    [C_FOREGROUND_INACTIVE] = "foregroundInactive",
    [C_PRIMARY]             = "primary",
    [C_MUTED]               = "muted",
    [C_SECONDARY]           = "secondary",
    [C_SUCCESS]             = "success",
    [C_WARNING]             = "warning",
    [C_DANGER]              = "danger",
    [C_SHADE]               = NULL,
};

typedef struct {
  const char* setting;
  theme_color_t color;
  double alpha;
} theme_mapping_t;

/* One accent (primary) on shared surfaces; only error and warning keep their own hue. */
static const theme_mapping_t mappings[] = {
    {"default-bg", C_SHADE, 1.0},
    {"default-fg", C_FOREGROUND, 1.0},
    {"statusbar-bg", C_BACKGROUND_ALT, 1.0},
    {"statusbar-fg", C_FOREGROUND, 1.0},
    {"inputbar-bg", C_BACKGROUND_ALT, 1.0},
    {"inputbar-fg", C_FOREGROUND, 1.0},
    {"completion-bg", C_BACKGROUND_ALT, 1.0},
    {"completion-fg", C_FOREGROUND, 1.0},
    {"completion-group-bg", C_BACKGROUND_ALT, 1.0},
    {"completion-group-fg", C_FOREGROUND_INACTIVE, 1.0},
    {"completion-highlight-bg", C_PRIMARY, 1.0},
    {"completion-highlight-fg", C_BACKGROUND, 1.0},
    {"notification-bg", C_BACKGROUND_ALT, 1.0},
    {"notification-fg", C_FOREGROUND, 1.0},
    {"notification-error-bg", C_DANGER, 1.0},
    {"notification-error-fg", C_BACKGROUND, 1.0},
    {"notification-warning-bg", C_WARNING, 1.0},
    {"notification-warning-fg", C_BACKGROUND, 1.0},
    {"index-bg", C_BACKGROUND_ALT, 1.0},
    {"index-fg", C_FOREGROUND, 1.0},
    {"index-active-bg", C_PRIMARY, 1.0},
    {"index-active-fg", C_BACKGROUND, 1.0},
    {"scrollbar-bg", C_SHADE, 1.0},
    {"scrollbar-fg", C_MUTED, 1.0},
    {"highlight-color", C_PRIMARY, 0.35},
    {"highlight-active-color", C_PRIMARY, 0.6},
    {"render-loading-bg", C_BACKGROUND, 1.0},
    {"render-loading-fg", C_FOREGROUND, 1.0},
    {"recolor-lightcolor", C_BACKGROUND, 1.0},
    {"recolor-darkcolor", C_FOREGROUND, 1.0},
    {"signature-success-color", C_SUCCESS, 1.0},
    {"signature-warning-color", C_WARNING, 1.0},
    {"signature-error-color", C_DANGER, 1.0},
};

/* Text must stay readable on whatever surface the theme provides; the foreground is nudged until it is. */
typedef struct {
  const char* fg;
  const char* bg;
  double minimum;
} theme_contrast_pair_t;

static const theme_contrast_pair_t contrast_pairs[] = {
    {"default-fg", "default-bg", 7.0},
    {"statusbar-fg", "statusbar-bg", 7.0},
    {"inputbar-fg", "inputbar-bg", 7.0},
    {"completion-fg", "completion-bg", 7.0},
    {"completion-group-fg", "completion-group-bg", 4.5},
    {"completion-highlight-fg", "completion-highlight-bg", 4.5},
    {"notification-fg", "notification-bg", 7.0},
    {"notification-error-fg", "notification-error-bg", 4.5},
    {"notification-warning-fg", "notification-warning-bg", 4.5},
    {"index-fg", "index-bg", 7.0},
    {"index-active-fg", "index-active-bg", 4.5},
    {"scrollbar-fg", "scrollbar-bg", 3.0},
    {"render-loading-fg", "render-loading-bg", 4.5},
};

struct zathura_theme_s {
  zathura_t* zathura;
  char* source;         /**< Selector as configured, possibly a symlink */
  char* target_dir;     /**< Canonical directory currently watched for content changes */
  char* selector_dir;   /**< Canonical parent of the selector, watched for symlink swaps */
  GFileMonitor* target_monitor;
  GFileMonitor* selector_monitor;
  char* parent_dir;     /**< Canonical parent of the target, watched for whole-directory replacement */
  GFileMonitor* parent_monitor;
  dev_t target_dev;     /**< Identity of the watched target; a replaced directory keeps its path */
  ino_t target_ino;
  guint debounce;
  char* applied;        /**< Fingerprint of the last applied colors */
};

static char* canonical_path(const char* path) {
  char* resolved = realpath(path, NULL);
  if (resolved == NULL) {
    return NULL;
  }
  char* result = g_strdup(resolved);
  free(resolved);
  return result;
}

static bool read_small_file(const char* path, char** contents) {
  gsize length = 0;
  if (g_file_get_contents(path, contents, &length, NULL) == false) {
    return false;
  }
  if (length > THEME_MAX_FILE_SIZE) {
    g_clear_pointer(contents, g_free);
    return false;
  }
  return true;
}

static bool parse_channel(const char* text, double* value) {
  g_auto(GStrv) parts = g_strsplit(text, "/", 2);
  char* end           = NULL;
  *value              = g_ascii_strtod(g_strstrip(parts[0]), &end);
  if (end == parts[0] || *end != '\0') {
    return false;
  }
  if (parts[1] != NULL) {
    const double divisor = g_ascii_strtod(g_strstrip(parts[1]), &end);
    if (end == parts[1] || *end != '\0' || divisor <= 0.0) {
      return false;
    }
    *value /= divisor;
  }
  return *value >= 0.0 && *value <= 1.0;
}

/* Accepts "#rrggbb", "rrggbb", QML "#aarrggbb" and Qt.rgba(r, g, b, a) with optional "/ 255" divisors. */
static bool parse_theme_color(const char* text, GdkRGBA* color) {
  g_autofree char* value = g_strstrip(g_strdup(text));
  size_t length          = strlen(value);
  if (length >= 2 && (value[0] == '"' || value[0] == '\'') && value[length - 1] == value[0]) {
    value[length - 1] = '\0';
    memmove(value, value + 1, length - 1);
  }

  if (g_str_has_prefix(value, "Qt.rgba(") && g_str_has_suffix(value, ")")) {
    value[strlen(value) - 1] = '\0';
    g_auto(GStrv) channels   = g_strsplit(value + strlen("Qt.rgba("), ",", 0);
    double v[4];
    if (g_strv_length(channels) != 4) {
      return false;
    }
    for (int i = 0; i < 4; ++i) {
      if (parse_channel(channels[i], &v[i]) == false) {
        return false;
      }
    }
    *color = (GdkRGBA){v[0], v[1], v[2], v[3]};
    return true;
  }

  const char* digits = value[0] == '#' ? value + 1 : value;
  length             = strlen(digits);
  if ((length != 6 && length != 8) || strspn(digits, "0123456789abcdefABCDEF") != length) {
    return false;
  }
  unsigned int rgba[4] = {0, 0, 0, 255};
  const char* p        = digits;
  if (length == 8) {
    /* QML orders alpha first */
    sscanf(p, "%2x", &rgba[3]);
    p += 2;
  }
  sscanf(p, "%2x%2x%2x", &rgba[0], &rgba[1], &rgba[2]);
  *color = (GdkRGBA){rgba[0] / 255.0, rgba[1] / 255.0, rgba[2] / 255.0, rgba[3] / 255.0};
  return true;
}

static void palette_set(theme_palette_t* palette, theme_color_t index, const GdkRGBA* color, bool overwrite) {
  if (palette->have[index] == false || overwrite == true) {
    palette->color[index] = *color;
    palette->have[index]  = true;
  }
}

static void read_colors_qml(const char* path, theme_palette_t* palette) {
  g_autofree char* contents = NULL;
  if (read_small_file(path, &contents) == false) {
    return;
  }

  g_auto(GStrv) lines = g_strsplit(contents, "\n", 0);
  for (char** line = lines; *line != NULL; ++line) {
    char* property = strstr(*line, "property color ");
    if (property == NULL) {
      continue;
    }
    char* name  = property + strlen("property color ");
    char* colon = strchr(name, ':');
    if (colon == NULL) {
      continue;
    }
    *colon       = '\0';
    char* value  = colon + 1;
    char* comment = strstr(value, "//");
    if (comment != NULL) {
      *comment = '\0';
    }
    char* semicolon = strchr(value, ';');
    if (semicolon != NULL) {
      *semicolon = '\0';
    }
    g_strstrip(name);

    for (int i = 0; i < C_COUNT; ++i) {
      GdkRGBA color;
      if (g_strcmp0(name, qml_names[i]) == 0 && parse_theme_color(value, &color)) {
        palette_set(palette, i, &color, true);
      }
    }
  }
}

static void read_kitty_conf(const char* path, theme_palette_t* palette) {
  g_autofree char* contents = NULL;
  if (read_small_file(path, &contents) == false) {
    return;
  }

  GdkRGBA ansi[16];
  bool ansi_have[16] = {false};
  g_auto(GStrv) lines = g_strsplit(contents, "\n", 0);
  for (char** line = lines; *line != NULL; ++line) {
    g_autofree char* copy = g_strstrip(g_strdup(*line));
    if (copy[0] == '#' || copy[0] == '\0') {
      continue;
    }
    char* separator = strpbrk(copy, " \t");
    if (separator == NULL) {
      continue;
    }
    *separator = '\0';
    GdkRGBA color;
    if (parse_theme_color(separator + 1, &color) == false) {
      continue;
    }
    if (g_strcmp0(copy, "background") == 0) {
      palette_set(palette, C_BACKGROUND, &color, false);
    } else if (g_strcmp0(copy, "foreground") == 0) {
      palette_set(palette, C_FOREGROUND, &color, false);
    } else if (g_str_has_prefix(copy, "color")) {
      char* end           = NULL;
      const guint64 index = g_ascii_strtoull(copy + strlen("color"), &end, 10);
      if (*end == '\0' && index < 16) {
        ansi[index]      = color;
        ansi_have[index] = true;
      }
    }
  }

  static const struct {
    theme_color_t target;
    int index;
  } ansi_fallbacks[] = {
      {C_BACKGROUND_ALT, 0}, {C_BACKGROUND_GRAY, 8}, {C_FOREGROUND_INACTIVE, 8}, {C_MUTED, 8},  {C_PRIMARY, 4},
      {C_SECONDARY, 6},      {C_SUCCESS, 2},         {C_WARNING, 3},             {C_DANGER, 1},
  };
  for (size_t i = 0; i < LENGTH(ansi_fallbacks); ++i) {
    if (ansi_have[ansi_fallbacks[i].index]) {
      palette_set(palette, ansi_fallbacks[i].target, &ansi[ansi_fallbacks[i].index], false);
    }
  }
}

static bool load_palette(const char* directory, theme_palette_t* palette) {
  memset(palette, 0, sizeof(*palette));

  g_autofree char* qml   = g_build_filename(directory, "Colors.qml", NULL);
  g_autofree char* kitty = g_build_filename(directory, "kitty.conf", NULL);
  read_colors_qml(qml, palette);
  read_kitty_conf(kitty, palette);

  /* a half-written file must not repaint with a partial palette */
  if (palette->have[C_BACKGROUND] == false || palette->have[C_FOREGROUND] == false) {
    return false;
  }

  /* Remaining gaps derive from the base colors so every theme maps completely */
  const GdkRGBA* background = &palette->color[C_BACKGROUND];
  const GdkRGBA* foreground = &palette->color[C_FOREGROUND];
  palette_set(palette, C_BACKGROUND_ALT, background, false);
  palette_set(palette, C_BACKGROUND_GRAY, &palette->color[C_BACKGROUND_ALT], false);
  palette_set(palette, C_FOREGROUND_INACTIVE, foreground, false);
  palette_set(palette, C_MUTED, &palette->color[C_FOREGROUND_INACTIVE], false);
  palette_set(palette, C_PRIMARY, foreground, false);
  palette_set(palette, C_SECONDARY, &palette->color[C_PRIMARY], false);
  palette_set(palette, C_SUCCESS, &palette->color[C_PRIMARY], false);
  palette_set(palette, C_WARNING, &palette->color[C_PRIMARY], false);
  palette_set(palette, C_DANGER, &palette->color[C_PRIMARY], false);

  /* Keep the background's hue but cap the brightest channel so even light themes get a black-adjacent margin */
  const double peak = MAX(background->red, MAX(background->green, background->blue));
  const double scale = peak > THEME_SHADE_PEAK ? THEME_SHADE_PEAK / peak : 1.0;
  palette->color[C_SHADE] = (GdkRGBA){background->red * scale, background->green * scale, background->blue * scale, 1.0};
  palette->have[C_SHADE]  = true;
  return true;
}

static double channel_luminance(double value) {
  return value <= 0.03928 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

static double relative_luminance(const GdkRGBA* color) {
  return 0.2126 * channel_luminance(color->red) + 0.7152 * channel_luminance(color->green) +
         0.0722 * channel_luminance(color->blue);
}

static double contrast_ratio(const GdkRGBA* a, const GdkRGBA* b) {
  const double la = relative_luminance(a);
  const double lb = relative_luminance(b);
  return (MAX(la, lb) + 0.05) / (MIN(la, lb) + 0.05);
}

/* Mix the color towards black or white, whichever separates better from the surface, until it is readable. */
static void ensure_contrast(GdkRGBA* fg, const GdkRGBA* bg, double minimum) {
  if (contrast_ratio(fg, bg) >= minimum) {
    return;
  }
  const GdkRGBA white = {1, 1, 1, 1};
  const GdkRGBA black = {0, 0, 0, 1};
  const GdkRGBA* end  = contrast_ratio(&white, bg) >= contrast_ratio(&black, bg) ? &white : &black;
  const GdkRGBA start = *fg;
  for (int step = 1; step <= 20; ++step) {
    const double t = step / 20.0;
    fg->red        = start.red + (end->red - start.red) * t;
    fg->green      = start.green + (end->green - start.green) * t;
    fg->blue       = start.blue + (end->blue - start.blue) * t;
    if (contrast_ratio(fg, bg) >= minimum) {
      return;
    }
  }
}

static int mapping_index(const char* setting) {
  for (size_t i = 0; i < LENGTH(mappings); ++i) {
    if (g_strcmp0(mappings[i].setting, setting) == 0) {
      return (int)i;
    }
  }
  return -1;
}

/* GDK parses both forms; spell out hex when opaque so settings stay readable with :set */
static char* format_color(const GdkRGBA* color, double alpha) {
  const int r = (int)(CLAMP(color->red, 0.0, 1.0) * 255.0 + 0.5);
  const int g = (int)(CLAMP(color->green, 0.0, 1.0) * 255.0 + 0.5);
  const int b = (int)(CLAMP(color->blue, 0.0, 1.0) * 255.0 + 0.5);
  if (alpha >= 1.0) {
    return g_strdup_printf("#%02x%02x%02x", r, g, b);
  }
  char buffer[G_ASCII_DTOSTR_BUF_SIZE];
  g_ascii_formatd(buffer, sizeof(buffer), "%.2f", alpha);
  return g_strdup_printf("rgba(%d,%d,%d,%s)", r, g, b, buffer);
}

/* Resolve the selector to its canonical theme directory, or NULL while a swap is in flight */
static char* resolve_target(const zathura_theme_t* theme) {
  g_autofree char* resolved = canonical_path(theme->source);
  if (resolved == NULL) {
    return NULL;
  }
  if (g_file_test(resolved, G_FILE_TEST_IS_DIR) == true) {
    return g_steal_pointer(&resolved);
  }
  if (g_file_test(resolved, G_FILE_TEST_IS_REGULAR) == true) {
    return g_path_get_dirname(resolved);
  }
  return NULL;
}

static void cb_monitor_changed(GFileMonitor* UNUSED(monitor), GFile* UNUSED(file), GFile* UNUSED(other),
                               GFileMonitorEvent UNUSED(event), gpointer data);

static void watch_directory(zathura_theme_t* theme, GFileMonitor** monitor, char** current, const char* directory) {
  if (directory == NULL) {
    return;
  }
  if (*monitor != NULL && g_strcmp0(*current, directory) == 0) {
    return;
  }

  if (*monitor != NULL) {
    g_signal_handlers_disconnect_by_data(*monitor, theme);
    g_file_monitor_cancel(*monitor);
    g_clear_object(monitor);
  }
  g_free(*current);
  *current = g_strdup(directory);

  g_autoptr(GFile) file = g_file_new_for_path(directory);
  g_autoptr(GError) error = NULL;
  *monitor = g_file_monitor_directory(file, G_FILE_MONITOR_NONE, NULL, &error);
  if (*monitor == NULL) {
    girara_warning("Unable to watch theme directory %s: %s", directory, error != NULL ? error->message : "unknown");
    return;
  }
  g_signal_connect(*monitor, "changed", G_CALLBACK(cb_monitor_changed), theme);
}

static void drop_monitor(zathura_theme_t* theme, GFileMonitor** monitor, char** current) {
  if (*monitor != NULL) {
    g_signal_handlers_disconnect_by_data(*monitor, theme);
    g_file_monitor_cancel(*monitor);
    g_clear_object(monitor);
  }
  g_clear_pointer(current, g_free);
}

static void apply_theme(zathura_theme_t* theme) {
  g_autofree char* target = resolve_target(theme);
  girara_debug("Theme: source %s resolves to %s", theme->source, target != NULL ? target : "(nothing)");
  /* follow chains that were swapped since the last event */
  if (target != NULL) {
    struct stat info;
    if (stat(target, &info) == 0 && (info.st_ino != theme->target_ino || info.st_dev != theme->target_dev)) {
      /* the directory was replaced under the same path, so its old watch is dead */
      drop_monitor(theme, &theme->target_monitor, &theme->target_dir);
      theme->target_ino = info.st_ino;
      theme->target_dev = info.st_dev;
    }
    g_autofree char* parent = g_path_get_dirname(target);
    if (g_strcmp0(parent, theme->selector_dir) == 0) {
      drop_monitor(theme, &theme->parent_monitor, &theme->parent_dir);
    } else {
      watch_directory(theme, &theme->parent_monitor, &theme->parent_dir, parent);
    }
  }
  watch_directory(theme, &theme->target_monitor, &theme->target_dir, target);
  if (target == NULL) {
    return;
  }

  theme_palette_t palette;
  if (load_palette(target, &palette) == false) {
    girara_debug("Theme at %s is not readable yet, keeping current colors.", target);
    return;
  }

  GdkRGBA colors[LENGTH(mappings)];
  for (size_t i = 0; i < LENGTH(mappings); ++i) {
    colors[i] = palette.color[mappings[i].color];
  }
  for (size_t i = 0; i < LENGTH(contrast_pairs); ++i) {
    const int fg = mapping_index(contrast_pairs[i].fg);
    const int bg = mapping_index(contrast_pairs[i].bg);
    if (fg >= 0 && bg >= 0) {
      ensure_contrast(&colors[fg], &colors[bg], contrast_pairs[i].minimum);
    }
  }

  char* values[LENGTH(mappings)];
  GString* fingerprint = g_string_new(NULL);
  for (size_t i = 0; i < LENGTH(mappings); ++i) {
    values[i] = format_color(&colors[i], mappings[i].alpha);
    g_string_append_printf(fingerprint, "%s=%s;", mappings[i].setting, values[i]);
  }

  girara_debug("Theme: applying %s", fingerprint->str);
  if (g_strcmp0(theme->applied, fingerprint->str) != 0) {
    girara_session_t* session = theme->zathura->ui.session;
    for (size_t i = 0; i < LENGTH(mappings); ++i) {
      girara_setting_set(session, mappings[i].setting, values[i]);
    }
    g_free(theme->applied);
    theme->applied = g_string_free(fingerprint, false);
  } else {
    g_string_free(fingerprint, true);
  }

  for (size_t i = 0; i < LENGTH(mappings); ++i) {
    g_free(values[i]);
  }
}

static gboolean cb_debounce(gpointer data) {
  zathura_theme_t* theme = data;
  theme->debounce        = 0;
  apply_theme(theme);
  return G_SOURCE_REMOVE;
}

static void cb_monitor_changed(GFileMonitor* UNUSED(monitor), GFile* UNUSED(file), GFile* UNUSED(other),
                               GFileMonitorEvent UNUSED(event), gpointer data) {
  zathura_theme_t* theme = data;
  /* dynamic themes rewrite dozens of files; wait for the burst to settle */
  if (theme->debounce != 0) {
    g_source_remove(theme->debounce);
  }
  theme->debounce = g_timeout_add(THEME_DEBOUNCE_MS, cb_debounce, theme);
}

static char* default_source(void) {
  return g_build_filename(g_get_user_config_dir(), "themes", ".caelestia-use", NULL);
}

void zathura_theme_free(zathura_t* zathura) {
  if (zathura == NULL || zathura->theme == NULL) {
    return;
  }

  zathura_theme_t* theme = zathura->theme;
  zathura->theme         = NULL;

  if (theme->debounce != 0) {
    g_source_remove(theme->debounce);
  }
  GFileMonitor** monitors[] = {&theme->target_monitor, &theme->selector_monitor, &theme->parent_monitor};
  for (size_t i = 0; i < LENGTH(monitors); ++i) {
    if (*monitors[i] != NULL) {
      g_signal_handlers_disconnect_by_data(*monitors[i], theme);
      g_file_monitor_cancel(*monitors[i]);
      g_clear_object(monitors[i]);
    }
  }
  g_free(theme->source);
  g_free(theme->target_dir);
  g_free(theme->selector_dir);
  g_free(theme->parent_dir);
  g_free(theme->applied);
  g_free(theme);
}

void zathura_theme_init(zathura_t* zathura) {
  g_return_if_fail(zathura != NULL);
  g_return_if_fail(zathura->ui.session != NULL);

  zathura_theme_free(zathura);

  bool follow = true;
  girara_setting_get(zathura->ui.session, "theme-follow", &follow);
  if (follow == false) {
    return;
  }

  g_autofree char* configured = NULL;
  girara_setting_get(zathura->ui.session, "theme-source", &configured);

  zathura_theme_t* theme = g_malloc0(sizeof(*theme));
  theme->zathura         = zathura;
  theme->source          = (configured != NULL && configured[0] != '\0') ? g_strdup(configured) : default_source();
  zathura->theme         = theme;

  /* swapping the selector or a "current" link happens next to it, so watch its canonical parent */
  g_autofree char* parent = g_path_get_dirname(theme->source);
  g_autofree char* canonical_parent = canonical_path(parent);
  watch_directory(theme, &theme->selector_monitor, &theme->selector_dir, canonical_parent);

  apply_theme(theme);
}
