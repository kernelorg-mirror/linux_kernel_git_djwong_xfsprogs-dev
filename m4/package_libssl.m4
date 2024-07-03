#
# Check if we have a pwritev2 libc call (Linux)
#
AC_DEFUN([AC_HAVE_LIBSSL_SHA2],
  [ AC_MSG_CHECKING([for SHA256_DIGEST_LENGTH in openssl/sha.h])
    AC_LINK_IFELSE(
    [	AC_LANG_PROGRAM([[
#define _GNU_SOURCE
#include <openssl/sha.h>
	]], [[
int moo = SHA256_DIGEST_LENGTH;
	]])
    ], have_libssl_sha2=yes
       AC_MSG_RESULT(yes),
       AC_MSG_RESULT(no))
    AC_SUBST(have_libssl_sha2)
  ])
