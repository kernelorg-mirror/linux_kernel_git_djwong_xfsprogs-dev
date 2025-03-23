# SPDX-License-Identifier: GPL-2.0
# Copyright (C) 2025 Oracle.  All Rights Reserved.

## Check if the platform has rust tools such as cargo

# Check if rustc is installed
AC_DEFUN([AC_HAVE_RUSTC],
[
  AC_CHECK_PROG([have_rustc], [rustc], [yes], [no])
  AC_SUBST(have_rustc)
])

# Check if cargo is installed
AC_DEFUN([AC_HAVE_CARGO],
[
  AC_CHECK_PROG([have_cargo], [cargo], [yes], [no])
  AC_SUBST(have_cargo)
])

# Check if cargo-clippy (aka the linter) is installed
AC_DEFUN([AC_HAVE_CLIPPY],
[
  AC_CHECK_PROG([have_clippy], [cargo-clippy], [yes], [no])
  AC_SUBST(have_clippy)
])

# Require that we use the system crates
AC_DEFUN([AC_USE_SYSTEM_CRATES],
[
  use_system_crates=yes
  AC_SUBST(use_system_crates)
])

# Check if we're building Rust under one of those distributions that provides
# stabilized Rust crates (e.g. Debian, EPEL) and should therefore use them.
AC_DEFUN([AC_MAYBE_USE_SYSTEM_CRATES],
[
  AC_MSG_CHECKING([if we use system Rust crates])
  if test -f /etc/debian_version || test -f /etc/redhat-release; then
    use_system_crates=yes
    AC_MSG_RESULT(yes)
  else
    AC_MSG_RESULT(no)
  fi
  AC_SUBST(use_system_crates)
])

# Check if rustc knows about the LTO option
AC_DEFUN([AC_RUSTC_CHECK_LTO],
[
  AC_MSG_CHECKING([if Rust compiler supports LTO])
  rm -f /tmp/enoent.rs
  # check that rustc fails because it can't find enoent.rs, not
  # because codegen doesn't recognize lto.
  if LANG=C rustc -C lto /tmp/enoent.rs 2>&1 | grep -q -i 'enoent.rs.*no.*such'; then
    have_rustc_lto=yes
    AC_MSG_RESULT(yes)
  else
    AC_MSG_RESULT(no)
  fi
  AC_SUBST(have_rustc_lto)
])

# Check if we have a particular crate configuration.  The arguments are:
#
# 1. Name of variable to set.
# 2. User-friendly description of what we're checking.
# 3. List of crates in Cargo.toml dependencies format.
# 4. Value if the test build succeeds.
# 5. Value if the test build fails.
#
# The variable will be AC_SUBST'd automatically.  Be careful to escape the
# brackets that rustc/cargo want.
AC_DEFUN([AC_CHECK_CRATES],
[
  if test "$enable_crate_checks" = "no"; then
    $1=$4
    AC_SUBST([$1])
  else
    AC_MSG_CHECKING([for Rust crates for $2])
    rm -r -f .havecrate
    mkdir -p .havecrate/src/
    cat > .havecrate/Cargo.toml << ENDL
[[package]]
name = "havecrate"
version = "0.1.0"
edition = "2021"

[[dependencies]]
$3
ENDL
    cat > .havecrate/src/main.rs << ENDL
fn main() { }
ENDL
    if test -n "$use_system_crates"; then
      mkdir -p .havecrate/.cargo
      cat > .havecrate/.cargo/config.toml << ENDL
[[source]]
[[source.system-packages]]
directory = "/usr/share/cargo/registry"
[[source.crates-io]]
replace-with = "system-packages"
ENDL
    fi
    cat .havecrate/Cargo.toml >> config.log
    # Is there a faster way to check crate presence than this?
    if (cd .havecrate && cargo check) >>config.log 2>&1; then
      AC_MSG_RESULT([$4])
      $1=$4
    else
      AC_MSG_RESULT([$5])
      $1=$5
    fi
    AC_SUBST([$1])
    rm -r -f .havecrate
  fi
])

# Do we have all the crates we need for xfs_healer?
AC_DEFUN([AC_HAVE_HEALER_CRATES],
[
  AC_CHECK_CRATES([have_healer_crates], [xfs_healer],
    [
clap = { version = "4.0.32", features = [["derive"]] }
anyhow = { version = "1.0.69" }
],
    [yes], [no])
])
