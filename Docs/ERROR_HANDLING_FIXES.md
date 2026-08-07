# Error Handling Fixes

## Overview
This document details all error handling improvements made to the GOG Manager codebase. These fixes prevent silent failures, improve error reporting, and ensure operations complete successfully or fail with clear diagnostics.

## Unchecked Return Values

### Category Overview
Many system functions return status codes indicating success or failure. Ignoring these codes can lead to:
- Silent data loss
- Incomplete operations  
- Confusing errors downstream
- Difficult debugging

---

### Issue 1: Directory Removal Failures (installer_engine.c:310)
**Severity:** HIGH  
**Type:** Unchecked Return Value in Recursive Function

`remove_dir_recursive()` doesn't check return values from recursive calls or `g_remove()`. If removal fails, function still returns success, leaving orphaned files.

**Root Cause:**
```c
// BEFORE (BROKEN)
static gboolean
remove_dir_recursive (const char *path)
{
  GDir *d = g_dir_open (path, 0, NULL);
  if (!d)
    return g_remove (path) == 0;
  
  const char *name;
  while ((name = g_dir_read_name (d)))
  {
    gchar *child = g_build_filename (path, name, NULL);
    if (g_file_test (child, G_FILE_TEST_IS_DIR))
      remove_dir_recursive (child);    // Return value ignored!
    else
      g_remove (child);                // Return value ignored!
    g_free (child);
  }
  g_dir_close (d);
  return g_rmdir (path) == 0;          // Only checks final rmdir
}
```

**Fix Applied:**
```c
// AFTER (FIXED)
static gboolean
remove_dir_recursive (const char *path)
{
  GDir *d = g_dir_open (path, 0, NULL);
  if (!d)
    return g_remove (path) == 0;
  
  const char *name;
  gboolean ok = TRUE;
  while ((name = g_dir_read_name (d)))
  {
    gchar *child = g_build_filename (path, name, NULL);
    if (g_file_test (child, G_FILE_TEST_IS_DIR) && 
        !g_file_test (child, G_FILE_TEST_IS_SYMLINK))
    {
      if (!remove_dir_recursive (child))
        ok = FALSE;
    }
    else
    {
      if (g_remove (child) != 0)
        ok = FALSE;
    }
    g_free (child);
  }
  g_dir_close (d);
  if (g_rmdir (path) != 0)
    ok = FALSE;
  return ok;
}
```

**Impact:**
- All removal operations are tracked
- Failures propagate up call stack
- Caller knows if entire directory removal succeeded

---

### Issue 2: Directory Creation Failures (installer_engine.c:220, 353)
**Severity:** MEDIUM  
**Type:** Unchecked mkdir Return Value

`g_mkdir_with_parents()` called without checking return value. If directory creation fails, operations continue with invalid paths.

**Location 1: extract_linux_sync**
```c
// BEFORE (BROKEN)
g_mkdir_with_parents (job->install_dir, 0755);
ok = move_path (source, job->install_dir, error);  // Proceeds regardless
```

**Location 2: install_windows_sync**
```c
// BEFORE (BROKEN)
g_mkdir_with_parents (job->install_dir, 0755);
job_progress (job, -1, "Preparing Wine/Proton prefix…");  // No error check
```

**Fix Applied (Both Locations):**
```c
// AFTER (FIXED)
if (g_mkdir_with_parents (job->install_dir, 0755) != 0)
{
  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
               "Could not create install directory %s", job->install_dir);
  return FALSE;  // or: ok = FALSE; continue;
}
// Only proceed if directory created successfully
```

**Impact:**
- Installation fails clearly if directory creation fails
- User gets actionable error message instead of cryptic downstream error
- Logs full path for debugging

---

### Issue 3: File Removal in Move Operation (installer_engine.c:284)
**Severity:** MEDIUM  
**Type:** Unchecked rmdir Return Value

After moving directory contents, `g_rmdir()` return value not checked. Source directory left behind if removal fails.

**Root Cause:**
```c
// BEFORE (BROKEN)
if (ok)
  g_rmdir (src);  // Return value ignored!
return ok;  // Reports success even if rmdir failed
```

**Fix Applied:**
```c
// AFTER (FIXED)
if (ok)
{
  if (g_rmdir (src) != 0)
  {
    g_warning ("Failed to remove directory %s: %s", src, g_strerror (errno));
    ok = FALSE;
  }
}
return ok;
```

**Impact:**
- Empty source directories no longer left behind
- Caller aware of incomplete move operations
- Debug logs aid troubleshooting

---

## Silent Errors and Missing Diagnostics

### Issue: Silent Command-Line Argument Parsing Failure (runner.c:351)
**Severity:** HIGH  
**Type:** Silent Error / No Error Reporting

`g_shell_parse_argv()` called with NULL error parameter. If parsing fails, arguments silently dropped.

**Root Cause:**
```c
// BEFORE (BROKEN)
if (arguments && *arguments)
{
  gint argc = 0;
  gchar **split = NULL;
  if (g_shell_parse_argv (arguments, &argc, &split, NULL))  // NULL error!
  {
    // If parsing fails, this block never runs
    for (gint i = 0; i < argc; i++)
      g_ptr_array_add (argv, split[i]);
    g_free (split);
  }
  // No else block - parse failure silently ignored
}
```

**Fix Applied:**
```c
// AFTER (FIXED)
if (arguments && *arguments)
{
  gint argc = 0;
  gchar **split = NULL;
  GError *parse_error = NULL;
  if (!g_shell_parse_argv (arguments, &argc, &split, &parse_error))
  {
    g_warning ("Failed to parse command arguments: %s", parse_error->message);
    g_error_free (parse_error);
  }
  else
  {
    for (gint i = 0; i < argc; i++)
      g_ptr_array_add (argv, split[i]);
    g_free (split);
  }
}
```

**Impact:**
- Parse failures logged for debugging
- Game launch no longer silently drops arguments
- Users see warnings about malformed manifest arguments

---

### Issue: Unchecked chmod Calls (launch.c, installer_engine.c)
**Severity:** LOW  
**Type:** Silent Permission Failures

`g_chmod()` calls without checking return values. If permission setting fails, operations fail downstream without clear cause.

**Locations:**
- `launch.c:189` - Native executable execution
- `launch.c:214` - start.sh script execution  
- `installer_engine.c:360` - Post-install script

**Fix Applied:**
```c
// BEFORE (BROKEN)
g_chmod (full, 0755);
gchar *argv[] = { full, NULL };
ok = spawn_detached (argv, NULL, cwd, error);  // May fail due to missing execute bit

// AFTER (FIXED)
if (g_chmod (full, 0755) != 0)
  g_warning ("Failed to set execute permission on %s", full);
gchar *argv[] = { full, NULL };
ok = spawn_detached (argv, NULL, cwd, error);
```

**Impact:**
- Permission failures logged for debugging
- Users not confused by "permission denied" errors from spawn

---

## Hash Table State Consistency

### Issue: Hash Table Key Type Mismatch (config.c:191, 244)
**Severity:** HIGH  
**Type:** Lookup Failure Due to Key Mismatch

Hash table stores heap-allocated `gint64*` keys (from line 147), but lookups use stack-local `&id` addresses. Stack addresses never match heap keys, causing lookups to fail.

**Root Cause:**
```c
// During load (line 149):
gint64 *id = g_new (gint64, 1);        // Heap allocated
*id = g_ascii_strtoll (key, NULL, 10);
g_hash_table_insert (state->by_id, id, val);

// Later during lookup (line 191):
gint64 id = gog_game_get_id (game);
json_object *rec = g_hash_table_lookup (state->by_id, &id);  // Stack address!
// &id is different from heap-allocated keys, lookup fails
```

**Fix Applied:**
```c
// AFTER (FIXED)
gint64 id = gog_game_get_id (game);
gint64 *id_ptr = g_new (gint64, 1);
*id_ptr = id;
json_object *rec = g_hash_table_lookup (state->by_id, id_ptr);  // Heap address
g_free (id_ptr);
if (!rec)
  return;
```

**Impact:**
- Game state lookups now succeed
- Saved game settings properly restored
- Game uninstall tracking works correctly

---

## Pattern: Proper Error Handling Chain

### Good Pattern: Propagate Errors Upward
```c
static gboolean
operation (const char *path, GError **error)
{
  // Check preconditions
  if (!g_file_test (path, G_FILE_TEST_EXISTS))
  {
    g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
                 "File not found: %s", path);
    return FALSE;
  }
  
  // Check system call results
  GError *local_error = NULL;
  gboolean ok = g_spawn_sync (path, argv, envp,
                               G_SPAWN_SEARCH_PATH,
                               NULL, NULL, NULL, NULL,
                               NULL, &local_error);
  if (!ok)
  {
    g_propagate_error (error, local_error);
    return FALSE;
  }
  
  return TRUE;
}
```

### Calling Site Pattern
```c
GError *error = NULL;
if (!operation (path, &error))
{
  g_warning ("Operation failed: %s", error->message);
  g_clear_error (&error);
  return FALSE;
}
```

---

## Error Logging Strategy

### Severity Levels
- **g_warning()** - Non-critical errors that don't stop operation
  ```c
  g_warning ("Failed to set execute permission on %s", path);
  ```

- **g_error()** - Fatal errors; should rarely be used (causes abort)
  ```c
  // Avoid this pattern - only for truly unrecoverable conditions
  ```

- **g_return_val_if_fail()** - Programming errors, precondition violations
  ```c
  g_return_val_if_fail (state != NULL, FALSE);
  ```

- **g_set_error()** - User-facing errors to return to caller
  ```c
  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
               "Install directory not found: %s", install_dir);
  ```

---

## Testing Error Paths

### Manual Testing
```bash
# Test with read-only filesystem
chmod 555 /path/to/gamelib

# Test with missing directories
rm -rf ~/.config/gog-manager/

# Test with malformed config
echo '{"corrupt json' > ~/.config/gog-manager/config.json

# Test with invalid manifest paths
# Manually create game with manifest containing ../ paths
```

### Automated Testing
```c
static void
test_invalid_manifest_paths (void)
{
  // Test that paths with .. are rejected
  g_assert_false (validate_manifest_path ("../../etc/passwd"));
  
  // Test that absolute paths are rejected
  g_assert_false (validate_manifest_path ("/etc/passwd"));
  
  // Test that valid relative paths are accepted
  g_assert_true (validate_manifest_path ("game.exe"));
  g_assert_true (validate_manifest_path ("subdir/game.exe"));
}
```
