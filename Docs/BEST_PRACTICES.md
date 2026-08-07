# Best Practices Guide

## Overview

This guide documents best practices learned from fixing 41 issues in the GOG Manager codebase. Follow these patterns to prevent similar bugs in future development.

---

## Memory Management

### GLib Object Reference Counting

#### ✅ Correct: Keep Reference Until Done
```c
GogGame *game = g_list_model_get_item (store, i);

// Use the object
gint64 id = gog_game_get_id (game);
const char *title = gog_game_get_title (game);

// Unref when completely done
g_object_unref (game);
```

#### ❌ Wrong: Unref Too Early
```c
GogGame *game = g_list_model_get_item (store, i);
g_object_unref (game);                    // Too early!
gint64 id = gog_game_get_id (game);      // Use-after-free
```

#### ✅ Correct: Early Return With Transfer
```c
GogGame *game = g_list_model_get_item (store, i);
if (game_matches_id (game, search_id))
  return game;                            // Return with ref, let caller unref
g_object_unref (game);                    // Unref only if not returning
```

---

### Allocation Patterns

#### ✅ Correct: Allocate, Use, Free
```c
gchar *temp = g_strdup ("value");
process (temp);
g_free (temp);
```

#### ❌ Wrong: Use After Free
```c
gchar *temp = g_strdup ("value");
g_free (temp);
process (temp);                           // Use after free!
```

#### ✅ Correct: Transfer Ownership Explicitly
```c
// Caller allocates
gchar *data = g_strdup ("important");

// Transfer to GString (which frees it)
GString *str = g_string_new (NULL);
g_string_append (str, data);
g_free (data);  // Now caller's responsibility

// Or: let GString own it
GString *str = g_string_new (NULL);
g_string_free_and_steal (str);  // Get string and free GString
```

---

## Input Validation

### Path Validation

#### ✅ Correct: Comprehensive Path Checks
```c
static gboolean
is_safe_path (const char *path, const char *install_dir, GError **error)
{
  // Check if path is empty
  if (!path || path[0] == '\0')
  {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                 "Path cannot be empty");
    return FALSE;
  }

  // Reject absolute paths (escape attempt)
  if (g_path_is_absolute (path))
  {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                 "Path must be relative: %s", path);
    return FALSE;
  }

  // Reject directory traversal attempts
  if (g_strstr_len (path, -1, ".."))
  {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                 "Path cannot contain directory traversal: %s", path);
    return FALSE;
  }

  // Construct full path and verify it's within install_dir
  gchar *full = g_build_filename (install_dir, path, NULL);
  gchar *real = g_canonicalize_filename (full, install_dir);
  gchar *real_install = g_canonicalize_filename (install_dir, NULL);
  
  gboolean ok = g_str_has_prefix (real, real_install);
  
  g_free (full);
  g_free (real);
  g_free (real_install);

  if (!ok)
  {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL,
                 "Path escapes install directory: %s", path);
    return FALSE;
  }

  return TRUE;
}
```

#### ✅ Correct: Configuration Path Validation
```c
GogConfig *gog_config_load (void)
{
  GogConfig *cfg = g_new0 (GogConfig, 1);
  // ... load from JSON ...
  
  const char *library_path = ju_get_string (root, "library_path", NULL);
  if (library_path && g_path_is_absolute (library_path))
  {
    cfg->library_path = g_strdup (library_path);
  }
  // Reject relative paths silently - they're not useful for config
  
  return cfg;
}
```

---

## Error Handling

### Return Value Checking

#### ✅ Correct: Check All System Calls
```c
gboolean
perform_operation (const char *path, GError **error)
{
  // Check file system operations
  if (!g_file_test (path, G_FILE_TEST_EXISTS))
  {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                 "Path does not exist: %s", path);
    return FALSE;
  }

  // Check directory operations
  if (g_mkdir_with_parents (path, 0755) != 0)
  {
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                 "Could not create directory %s", path);
    return FALSE;
  }

  // Check file operations
  GError *file_error = NULL;
  GFile *file = g_file_new_for_path (path);
  if (!g_file_copy (file, dest, G_FILE_COPY_OVERWRITE, NULL, NULL, NULL, &file_error))
  {
    g_propagate_error (error, file_error);
    g_object_unref (file);
    return FALSE;
  }
  
  g_object_unref (file);
  return TRUE;
}
```

#### ❌ Wrong: Ignoring Return Values
```c
gboolean
perform_operation (const char *path)
{
  g_mkdir_with_parents (path, 0755);          // Ignore result!
  g_file_copy (...);                          // Assume success!
  return TRUE;                                // Report success anyway
}
```

---

### Error Propagation

#### ✅ Correct: Propagate Errors Up Call Stack
```c
static gboolean
low_level_operation (GError **error)
{
  // Return FALSE on error, let caller decide what to do
  if (!g_file_test (path, G_FILE_TEST_EXISTS))
  {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                 "File not found: %s", path);
    return FALSE;
  }
  return TRUE;
}

static gboolean
mid_level_operation (GError **error)
{
  GError *local_error = NULL;
  
  // Call low level, capture error
  if (!low_level_operation (&local_error))
  {
    // Either propagate or wrap with context
    g_propagate_error (error, local_error);  // Pass to caller
    return FALSE;
  }
  
  return TRUE;
}

static void
high_level_operation (void)
{
  GError *error = NULL;
  
  if (!mid_level_operation (&error))
  {
    // User-facing error handling
    g_warning ("Operation failed: %s", error->message);
    g_clear_error (&error);
    return;
  }
}
```

#### ❌ Wrong: Silent Failures
```c
if (!low_level_operation (NULL))          // NULL error: silent!
{
  continue;                                // Ignore failure
}
```

---

## File Operations

### Safe File Copying

#### ✅ Correct: No-Follow Symlinks
```c
GFile *src = g_file_new_for_path (source);
GFile *dst = g_file_new_for_path (dest);

GError *error = NULL;
gboolean ok = g_file_copy (src, dst,
                           G_FILE_COPY_OVERWRITE |
                           G_FILE_COPY_NOFOLLOW_SYMLINKS,
                           NULL, NULL, NULL, &error);

if (!ok)
{
  g_warning ("Copy failed: %s", error->message);
  g_clear_error (&error);
}

g_object_unref (src);
g_object_unref (dst);
```

#### ❌ Wrong: Following Symlinks
```c
g_file_copy (src, dst, G_FILE_COPY_OVERWRITE,
             NULL, NULL, NULL, &error);
// Might follow symlinks to unexpected locations
```

---

### Directory Removal

#### ✅ Correct: Track Failures
```c
static gboolean
remove_directory_recursive (const char *path, GError **error)
{
  GDir *dir = g_dir_open (path, 0, error);
  if (!dir)
    return FALSE;

  const char *name;
  gboolean ok = TRUE;
  while (ok && (name = g_dir_read_name (dir)))
  {
    gchar *child_path = g_build_filename (path, name, NULL);
    
    if (g_file_test (child_path, G_FILE_TEST_IS_DIR))
    {
      if (!remove_directory_recursive (child_path, error))
      {
        ok = FALSE;
      }
    }
    else
    {
      if (g_remove (child_path) != 0)
      {
        g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                     "Failed to remove %s", child_path);
        ok = FALSE;
      }
    }
    
    g_free (child_path);
  }

  g_dir_close (dir);

  if (ok && g_rmdir (path) != 0)
  {
    g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
                 "Failed to remove directory %s", path);
    ok = FALSE;
  }

  return ok;
}
```

---

## String and Argument Handling

### Argument Parsing

#### ✅ Correct: Handle Parse Errors
```c
static gboolean
add_command_arguments (GPtrArray *argv, const char *arguments, GError **error)
{
  if (!arguments || !*arguments)
    return TRUE;

  gint argc = 0;
  gchar **split = NULL;
  GError *parse_error = NULL;

  if (!g_shell_parse_argv (arguments, &argc, &split, &parse_error))
  {
    g_propagate_error (error, parse_error);
    return FALSE;
  }

  for (gint i = 0; i < argc; i++)
  {
    g_ptr_array_add (argv, split[i]);
  }
  g_free (split);

  return TRUE;
}
```

#### ❌ Wrong: Silent Parse Failures
```c
if (g_shell_parse_argv (arguments, &argc, &split, NULL))  // NULL error!
{
  for (gint i = 0; i < argc; i++)
    g_ptr_array_add (argv, split[i]);
}
// If parse fails, arguments are silently dropped
```

---

## Configuration Security

### Safe Configuration Loading

#### ✅ Correct: Validate All Values
```c
typedef struct
{
  gchar *library_path;
  gchar *install_root;
  GStrv custom_runners;
} Config;

Config *config_load (GError **error)
{
  Config *cfg = g_new0 (Config, 1);
  
  json_object *root = ju_load_file (config_file(), error);
  if (!root)
    return cfg;

  // Validate library path
  const char *lib_path = ju_get_string (root, "library_path", NULL);
  if (lib_path)
  {
    if (!g_path_is_absolute (lib_path))
    {
      g_warning ("Config library_path must be absolute, ignoring: %s", lib_path);
    }
    else
    {
      cfg->library_path = g_strdup (lib_path);
    }
  }

  // Validate install root
  const char *install_root = ju_get_string (root, "install_root", NULL);
  if (install_root && g_path_is_absolute (install_root))
  {
    cfg->install_root = g_strdup (install_root);
  }

  // Validate custom runners
  json_object *runners = ju_get_array (root, "custom_runners");
  if (runners)
  {
    // Each runner should be validated similarly
    // Example: ensure paths are absolute, files exist, etc.
  }

  json_object_put (root);
  return cfg;
}
```

---

## Compiler Warnings

### Treating Warnings as Errors

#### ✅ Setup in meson.build
```meson
add_project_arguments(
  '-Wall',
  '-Wextra',
  '-Werror',  # Treat warnings as errors
  '-Wno-unused-parameter',  # OK to have unused params in callbacks
  language: 'c'
)
```

#### ✅ Fix Unused Variables
```c
// Instead of: (void) unused_param;
// Just remove the variable if not used

// Or, if kept for interface compatibility:
static void
callback (gpointer data, gpointer user_data)
{
  (void) user_data;  // Not used in this callback
  process_data (data);
}
```

---

## Testing Patterns

### Unit Test Example

```c
#include <glib.h>

static void
test_path_validation_rejects_traversal (void)
{
  GError *error = NULL;
  
  g_assert_false (is_safe_path ("../../etc/passwd", "/games", &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_clear_error (&error);
}

static void
test_path_validation_rejects_absolute (void)
{
  GError *error = NULL;
  
  g_assert_false (is_safe_path ("/etc/passwd", "/games", &error));
  g_assert_error (error, G_FILE_ERROR, G_FILE_ERROR_INVAL);
  g_clear_error (&error);
}

static void
test_path_validation_accepts_relative (void)
{
  GError *error = NULL;
  
  g_assert_true (is_safe_path ("game/exe.exe", "/games", &error));
  g_assert_no_error (error);
}

int
main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  
  g_test_add_func ("/path/validation/traversal",
                   test_path_validation_rejects_traversal);
  g_test_add_func ("/path/validation/absolute",
                   test_path_validation_rejects_absolute);
  g_test_add_func ("/path/validation/relative",
                   test_path_validation_accepts_relative);
  
  return g_test_run ();
}
```

---

## Code Review Checklist

Before submitting code for review:

- [ ] All system calls have return value checks
- [ ] All g_malloc/g_new have corresponding g_free/g_object_unref
- [ ] No use-after-free patterns (objects used after unref)
- [ ] Paths from external sources validated
- [ ] Error messages propagated or logged
- [ ] No NULL pointer dereferences
- [ ] Casts are safe (no casting away const on mutable data)
- [ ] Dead code removed
- [ ] Unused variables removed
- [ ] Compiler builds with -Wall -Wextra -Werror
- [ ] Unit tests added for new validation
- [ ] Tested error paths, not just happy path

---

## Common Mistakes to Avoid

| Mistake | ❌ Wrong | ✅ Correct |
|---------|---------|----------|
| Unref too early | `unref(obj); use(obj);` | `use(obj); unref(obj);` |
| Ignore return values | `g_mkdir (path, 0755);` | `if (g_mkdir (...) != 0) handle_error();` |
| Follow symlinks | `g_file_copy (src, dst, ...)` | `... \| G_FILE_COPY_NOFOLLOW_SYMLINKS` |
| Cast away const | `(char*)"string"` | `g_strdup("string")` |
| Relative config paths | Accept from config | Reject, only accept absolute |
| Silent failures | `if (op()) continue;` | Check error, propagate or log |
| Use stack addresses | `&local_var` as hash key | Allocate heap: `g_new()` |
| Unused variables | Leave unused | `(void) var;` or remove |

---

## References

- [GLib Memory Allocation](https://developer.gnome.org/glib/stable/glib-Memory-Allocation.html)
- [GLib Type System](https://developer.gnome.org/gobject/stable/index.html)
- [GLib Errors](https://developer.gnome.org/glib/stable/glib-Error-Reporting.html)
- [GIO File Operations](https://developer.gnome.org/gio/stable/GFile.html)
- [OWASP Path Traversal](https://owasp.org/www-community/attacks/Path_Traversal)
- [CWE-367: TOCTOU Race Condition](https://cwe.mitre.org/data/definitions/367.html)
