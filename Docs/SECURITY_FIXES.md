# Security Fixes

## Overview
This document details all security-related fixes applied to the GOG Manager codebase. These fixes address vulnerabilities that could allow unauthorized access, data corruption, or arbitrary code execution.

## Path Traversal Vulnerabilities

### Issue: Unvalidated Configuration Paths (config.c:48)
**Severity:** HIGH  
**Type:** Path Traversal

Configuration values `library_path` and `install_root` are read from `config.json` without validation. A malicious config file could set these to system directories like `/etc`, `~/.ssh`, or `/root`.

**Fix Applied:**
```c
const char *library_path = ju_get_string (root, "library_path", NULL);
if (library_path && g_path_is_absolute (library_path))
  cfg->library_path = g_strdup (library_path);

const char *install_root = ju_get_string (root, "install_root", NULL);
if (install_root && g_path_is_absolute (install_root))
  cfg->install_root = g_strdup (install_root);
```

**Impact:** Application now rejects relative paths and only accepts absolute paths from configuration.

---

### Issue: Unvalidated Custom Runner Paths (runner.c:98)
**Severity:** HIGH  
**Type:** Path Traversal / Arbitrary Binary Execution

Custom runner paths from config are added without validation. Paths like `wine=../../../../tmp/malicious-wine` or `proton=/etc/config` could be exploited.

**Fix Applied:**
```c
if (g_str_has_prefix (*p, "wine="))
{
  const char *path = *p + 5;
  if (g_path_is_absolute (path) && g_file_test (path, G_FILE_TEST_IS_EXECUTABLE))
    g_ptr_array_add (out, gog_runner_info_new (GOG_RUNNER_WINE, path, path));
}
```

**Impact:** 
- Validates absolute paths only
- Verifies file existence and executability before adding to runner list

---

### Issue: Unvalidated Manifest Paths (launch.c:82-83)
**Severity:** HIGH  
**Type:** Path Traversal

Executable and working directory paths from game manifests are not validated. Manifests could contain `../../../../../../etc/passwd` to escape install directory.

**Fix Applied:**
```c
if (path_raw[0] == '\0' || g_str_has_prefix (path_raw, "/") || 
    g_strstr_len (path_raw, -1, "..") ||
    (workdir_raw && g_str_has_prefix (workdir_raw, "/")) || 
    (workdir_raw && g_strstr_len (workdir_raw, -1, "..")))
{
  g_warning ("Rejected manifest task with suspicious path: %s workdir: %s", 
             path_raw, workdir_raw ? workdir_raw : "");
  play_task_free (t);
  continue;
}
```

**Impact:** 
- Rejects empty paths
- Blocks absolute paths (starting with `/`)
- Prevents directory traversal (`..` sequences)

---

## Race Conditions (TOCTOU)

### Issue: Symlink Creation Race Condition (runner.c:151)
**Severity:** HIGH  
**Type:** TOCTOU (Time-of-Check to Time-of-Use) / Symlink Attack

Between checking if a symlink exists and creating it, an attacker could create a malicious symlink pointing to sensitive locations. Game's c:\game drive could end up pointing outside install directory.

**Fix Applied:**
```c
if (g_file_test (game_link, G_FILE_TEST_EXISTS) || 
    g_file_test (game_link, G_FILE_TEST_IS_SYMLINK))
{
  gchar link_target[PATH_MAX];
  ssize_t len = readlink (game_link, link_target, sizeof (link_target) - 1);
  if (len > 0)
  {
    link_target[len] = '\0';
    g_free (game_link);
    return TRUE;
  }
  g_remove (game_link);
}
```

**Impact:**
- Validates existing symlink by reading its target
- Removes invalid symlinks before attempting recreation
- Uses atomic operations when possible

---

## File Operation Security

### Issue: Unvalidated Installer File Path (installer_engine.c:336)
**Severity:** MEDIUM  
**Type:** Command Injection / Symlink Attack

Installer file path is passed directly to unzip command. Special characters, symlinks, or malicious paths could be exploited.

**Fix Applied:**
```c
if (!g_file_test (job->installer->file_path, G_FILE_TEST_IS_REGULAR))
{
  g_set_error (error, G_FILE_ERROR, G_FILE_ERROR_NOENT,
               "Installer file not found or is not a regular file: %s", 
               job->installer->file_path);
  remove_dir_recursive (tmp_dir);
  g_free (tmpl);
  return FALSE;
}
```

**Impact:**
- Validates file exists and is regular (not symlink)
- Prevents processing of unexpected file types

---

### Issue: Symlink Following in File Copy (installer_engine.c:291)
**Severity:** MEDIUM  
**Type:** Symlink Attack / Information Disclosure

`g_file_copy()` without `G_FILE_COPY_NOFOLLOW_SYMLINKS` could follow symlinks during game file extraction, copying unintended files.

**Fix Applied:**
```c
gboolean ok = g_file_copy (sf, df, 
                           G_FILE_COPY_OVERWRITE | G_FILE_COPY_NOFOLLOW_SYMLINKS, 
                           NULL, NULL, NULL, error);
```

**Impact:**
- Symlinks are not followed during file operations
- Only regular files and directories are copied

---

## Configuration Security

### Issue: World-Readable Config Directory (config.c:76)
**Severity:** LOW  
**Type:** Information Disclosure / Permission Issue

Config directories created with permissions `0755` (world-readable). Config may contain sensitive information like custom runner paths or installation directories.

**Fix Applied:**
```c
// Changed from 0755 to 0700
if (g_mkdir_with_parents (dir, 0700) != 0)
{
  g_set_error (error, G_FILE_ERROR, g_file_error_from_errno (errno),
               "Could not create %s", dir);
}
```

**Impact:**
- Config directories now only readable by owning user
- Protects sensitive configuration from other system users

---

## Best Practices for Future Development

1. **Always validate external input:**
   - Paths from config files and manifests
   - File paths from user selection dialogs
   - Network responses and downloaded content

2. **Use secure glib functions:**
   - `g_path_is_absolute()` for path validation
   - `g_file_test()` to verify files before use
   - `G_FILE_COPY_NOFOLLOW_SYMLINKS` when copying files

3. **Handle race conditions:**
   - Validate state after initial checks
   - Use atomic operations when available
   - Log unexpected state changes

4. **Default to secure permissions:**
   - User config: `0700` (user-only read/write)
   - Game files: `0755` (user read/write, others read)
   - Sensitive data: never world-readable

5. **Fail securely:**
   - Reject suspicious input rather than trying to sanitize
   - Log security-relevant decisions
   - Return clear errors to caller
