#!/bin/bash

set -eu

# print the hostname to be able to identify runner by logs
echo "HOSTNAME=`hostname`"
TIMESTAMP=$(date +%s)
echo "TIMESTAMP=$TIMESTAMP" >> $GITHUB_ENV
echo "TIMESTAMP=$TIMESTAMP"

sudo apt-get -y install -qq wget ca-certificates

sudo apt-get update -qq

apt_packages="build-essential flex bison pkg-config libreadline-dev make gdb libipc-run-perl libicu-dev python3 python3-dev python3-pip python3-setuptools python3-testresources"

if [ $COMPILER = "clang" ]; then
	apt_packages="$apt_packages llvm-$LLVM_VER clang-$LLVM_VER clang-tools-$LLVM_VER"
fi

# install required packages
sudo apt-get -o Dpkg::Options::="--force-confdef" -o Dpkg::Options::="--force-confold" -y install -qq $apt_packages
