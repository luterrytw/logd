#!/bin/bash
SRC_ROOT=$(pwd)

function build_all()
{
	autoreconf -fiv || exit 1
	if [ "${HOST}" == "" ]; then
		./configure || exit 1
	else
		./configure --host ${HOST} || exit 1
	fi

	make
}

function install_all()
{
	if [[ "${INSTALL_PATH}" != "" ]]; then
		mkdir "${INSTALL_PATH}"
		cd "${INSTALL_PATH}"
		INSTALL_PATH=$(pwd)
	fi
	cd "${SRC_ROOT}"
	make install DESTDIR="${INSTALL_PATH}"
}

INSTALL_PATH=${1:-"../out"}

build_all
install_all

