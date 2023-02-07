#!/bin/bash

set -e

read -p "This tool downloads dependencies through apt-get, do you want to continue? [y(es)/n(o)] " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]
then
    exit 1
fi

sudo apt-get update

PACKAGES="git make cmake python3 python3-pip llvm-10 clang-10 zlib1g-dev libz3-dev libgoogle-perftools-dev libgtest-dev libsqlite3-dev"

sudo apt-get -y install $PACKAGES

# executing this file with "sudo install_instructions.sh" will install required dependencies for building Klee. Tested on clean vm with ubuntu 20.04

# install required pip tools.

sudo pip3 install lit wllvm tabulate

# create symlinks for llvm-10
sudo ln -s /usr/bin/llvm-config-10 /usr/bin/llvm-config
sudo ln -s /usr/bin/clang-10 /usr/bin/clang
sudo ln -s /usr/bin/clang-cpp-10 /usr/bin/clang-cpp
sudo ln -s /usr/bin/clang++-10 /usr/bin/clang++
