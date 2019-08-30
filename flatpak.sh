#!/bin/bash -xe

if [ "$1" = "--help" ]; then
	echo "Usage: $0 [branch] [build]"
	echo "Build xfsprogs flatpak from the given branch."
fi

branch="$1"
test -z "${branch}" && branch=master
build="$2"
test -z "${build}" && build=release

if [ "${build}" = "debug" ]; then
	config_opts=', "--disable-lto"'
fi

cat > flatpak.ini << ENDL
[Application]
name=org.xfs.progs
runtime=org.freedesktop.Platform/$(uname -m)/18.08

[Context]
devices=all;
ENDL

cat > org.xfs.progs.json << ENDL
{
    "app-id": "org.xfs.progs",
    "runtime": "org.freedesktop.Platform",
    "runtime-version": "18.08",
    "sdk": "org.freedesktop.Sdk",
    "command": "/app/sbin/xfs_repair",
    "metadata": "flatpak.ini",
    "modules": [
        {
            "name": "xfsprogs",
            "buildsystem": "autotools",
            "config-opts": ["--enable-shared=no" ${config_opts} ],
            "sources": [
                {
                    "type": "git",
                    "path": "${PWD}/",
                    "branch": "${branch}"
                }
            ]
        }
    ]
}
ENDL

flatpak-builder --repo=repo build-dir org.xfs.progs.json --force-clean
flatpak build-bundle repo xfsprogs.flatpak org.xfs.progs
echo build done, run:
echo flatpak install xfsprogs.flatpak
echo flatpak run org.xfs.progs
