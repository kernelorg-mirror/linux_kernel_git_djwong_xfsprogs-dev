#
# Figure out where to put systemd service units
#
AC_DEFUN([AC_CONFIG_SYSTEMD_SYSTEM_UNIT_DIR],
[
	AC_REQUIRE([PKG_PROG_PKG_CONFIG])
	AC_ARG_WITH([systemd_unit_dir],
	  [AS_HELP_STRING([--with-systemd-unit-dir@<:@=DIR@:>@],
		[Install systemd system units into DIR.])],
	  [],
	  [with_systemd_unit_dir=yes])
	AS_IF([test "x${with_systemd_unit_dir}" != "xno"],
	  [
		AS_IF([test "x${with_systemd_unit_dir}" = "xyes"],
		  [
			PKG_CHECK_MODULES([systemd], [systemd],
			  [
				with_systemd_unit_dir="$($PKG_CONFIG --variable=systemdsystemunitdir systemd)"
			  ], [
				with_systemd_unit_dir=""
			  ])
			m4_pattern_allow([^PKG_(MAJOR|MINOR|BUILD|REVISION)$])
		  ])
		AC_MSG_CHECKING([for systemd system unit dir])
		systemd_system_unit_dir="${with_systemd_unit_dir}"
		AS_IF([test -n "${systemd_system_unit_dir}"],
		  [
			AC_MSG_RESULT(${systemd_system_unit_dir})
			have_systemd="yes"
		  ],
		  [
			AC_MSG_RESULT(no)
			have_systemd="no"
		  ])
	  ],
	  [
		have_systemd="disabled"
	  ])
	AC_SUBST(have_systemd)
	AC_SUBST(systemd_system_unit_dir)
])

#
# Figure out where to install crontabs
#
AC_DEFUN([AC_CONFIG_CROND_DIR],
[
	AC_ARG_WITH([crond_dir],
	  [AS_HELP_STRING([--with-crond-dir@<:@=DIR@:>@],
		[Install system crontabs into DIR.])],
	  [],
	  [with_crond_dir=yes])
	AS_IF([test "x${with_crond_dir}" != "xno"],
	  [
		AS_IF([test "x${with_crond_dir}" = "xyes"],
		  [
			AS_IF([test -d "/etc/cron.d"],
			  [with_crond_dir="/etc/cron.d"])
		  ])
		AC_MSG_CHECKING([for system crontab dir])
		crond_dir="${with_crond_dir}"
		AS_IF([test -n "${crond_dir}"],
		  [
			AC_MSG_RESULT(${crond_dir})
			have_crond="yes"
		  ],
		  [
			AC_MSG_RESULT(no)
			have_crond="no"
		  ])
	  ],
	  [
		have_crond="disabled"
	  ])
	AC_SUBST(have_crond)
	AC_SUBST(crond_dir)
])

#
# Figure out where to put udev files
#
AC_DEFUN([AC_CONFIG_UDEV_DIR],
[
	AC_REQUIRE([PKG_PROG_PKG_CONFIG])
	AC_ARG_WITH([udev_dir],
	  [AS_HELP_STRING([--with-udev-dir@<:@=DIR@:>@],
		[Install udev files underneath DIR.])],
	  [],
	  [with_udev_dir=yes])
	AS_IF([test "x${with_udev_dir}" != "xno"],
	  [
		AS_IF([test "x${with_udev_dir}" = "xyes"],
		  [
			PKG_CHECK_MODULES([udev], [udev],
			  [
				with_udev_dir="$($PKG_CONFIG --variable=udev_dir udev)"
			  ], [
				with_udev_dir=""
			  ])
			m4_pattern_allow([^PKG_(MAJOR|MINOR|BUILD|REVISION)$])
		  ])
		AC_MSG_CHECKING([for udev dir])
		udev_dir="${with_udev_dir}"
		AS_IF([test -n "${udev_dir}"],
		  [
			AC_MSG_RESULT(${udev_dir})
			have_udev="yes"
		  ],
		  [
			AC_MSG_RESULT(no)
			have_udev="no"
		  ])
	  ],
	  [
		have_udev="disabled"
	  ])
	AC_SUBST(have_udev)
	AC_SUBST(udev_dir)
])
