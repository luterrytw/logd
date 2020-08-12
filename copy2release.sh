#!/bin/bash
SRC_ROOT=$(pwd)

function install_all()
{
        rm -fr ${SRC_ROOT}/.release
        mkdir ${SRC_ROOT}/.release
        make install DESTDIR=${SRC_ROOT}/.release

        mkdir -p ${SRC_ROOT}/release
	cp -a ${SRC_ROOT}/.release/usr/local/bin/logd ${SRC_ROOT}/release
        cp ${SRC_ROOT}/log.ini ${SRC_ROOT}/release
}

install_all
