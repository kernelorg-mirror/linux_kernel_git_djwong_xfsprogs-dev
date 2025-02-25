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

# Check if we're building Rust under Debian and therefore use Rust crates
# from the distributor.
AC_DEFUN([AC_USE_DEBIAN_CRATES],
[
  AC_MSG_CHECKING([if we use Debian Rust crates])
  if test -f /etc/debian_version; then
    use_debian_crates=yes
    AC_MSG_RESULT(yes)
  else
    AC_MSG_RESULT(no)
  fi
  AC_SUBST(use_debian_crates)
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
  if test -n "$use_debian_crates"; then
    mkdir -p .havecrate/.cargo
    cat > .havecrate/.cargo/config.toml << ENDL
[[source]]
[[source.debian-packages]]
directory = "/usr/share/cargo/registry"
[[source.crates-io]]
replace-with = "debian-packages"
ENDL
  fi
  # Is there a fast way to check crate presence than this?
  if (cd .havecrate && cargo check >/dev/null 2>&1); then
    AC_MSG_RESULT([$4])
    $1=$4
  else
    AC_MSG_RESULT([$5])
    $1=$5
  fi
  AC_SUBST([$1])
  rm -r -f .havecrate
])

# Do we have all the crates we need for xfs_healer?
AC_DEFUN([AC_HAVE_HEALER_CRATES],
[
  AC_CHECK_CRATES([have_healer_crates], [xfs_healer],
    [
clap = { version = "4.0.32", features = [["derive"]] }
nix = { version = "0.26.1" }
serde_json = { version = "1.0.87" }
enumset = { version = "1.0.12" }
strum = { version = "0.19.2" }
strum_macros = { version = "0.19.2" }
],
    [yes], [no])
])

# Check if clang is installed so that bindgen can find system headers.
AC_DEFUN([AC_HAVE_CLANG],
[
  AC_CHECK_PROG([have_clang], [clang], [yes], [no])
  AC_SUBST(have_clang)
])

# Check if rustfmt is installed; bindgen needs this to produce readable source
# code.
AC_DEFUN([AC_HAVE_RUSTFMT],
[
  AC_CHECK_PROG([have_rustfmt], [rustfmt], [yes], [no])
  AC_SUBST(have_rustfmt)
])

# Check if bindgen (aka the C FFI generator) is installed
AC_DEFUN([AC_HAVE_BINDGEN],
[
  AC_CHECK_PROG([have_bindgen], [bindgen], [yes], [no])
  AC_SUBST(have_bindgen)
])
