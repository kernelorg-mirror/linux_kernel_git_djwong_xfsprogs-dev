# Check if the platform has rust tools such as cargo
#

AC_DEFUN([AC_HAVE_RUST],
[
  AC_CHECK_PROG([have_rust], [cargo], [yes], [no])
  AC_SUBST(have_rust)
])
