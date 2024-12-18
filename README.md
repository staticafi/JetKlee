# JetKlee Progress Recorder Setup

Setup for linux machine (Ubuntu), on Windows or MacOS you can use VM.
Alternatively, you can follow the official [guide](http://klee-se.org/build/build-llvm13/) for building klee.

1. Install dependencies
    ```bash
    $ sudo apt-get install build-essential cmake curl file g++-multilib gcc-multilib git libcap-dev libgoogle-perftools-dev libncurses5-dev libsqlite3-dev libtcmalloc-minimal4 python3-pip unzip graphviz doxygen

    $ sudo apt-get install clang-14 llvm-14 llvm-14-dev llvm-14-tools`
    ```

2. Set-up `klee-uclibc`
    
    (in the root of the JetKlee repository)
    ```bash
    $ git clone https://github.com/klee/klee-uclibc

    $ cd klee-uclibc

    $ ./configure --make-llvm-lib

    $ make -j2
    ```

3. Set-up cmake & build JetKlee:

    (in the root of the JetKlee repository)
    ```bash
     $ mkdir build && cd build

     $ cmake -DENABLE_SOLVER_STP=OFF -DENABLE_SOLVER_Z3=ON -DENABLE_KLEE_UCLIBC=ON -DENABLE_POSIX_RUNTIME=ON -DKLEE_UCLIBC_PATH=../klee-uclibc -DENABLE_UNIT_TESTS=OFF ..

     $ make
     ```

     This should create the `klee` binary in the `build/bin` directory.

# Test on an example

1. Compile `.c` or `.i` file into bytecode

    (replace `<FILENAME>` with the name of the file you want to test)
    ```
    $ clang -I include -emit-llvm -c -g -O0 -Xclang -disable-O0-optnone <FILENAME>`
    ```

    This should generate a `<FILENAME>.bc` file.

    > Note: some examples are present within the `examples` directory (e.g. `examples/arrays/array.c`)

2. Run KLEE on the bytecode with progress recording enabled
    (replace `<OUTPUT_DIR>` with desired output directory)

    ```
    $ ./build/bin/klee --progress-recording --output-dir=<OUTPUT_DIR> <FILENAME>.bc
    ```

    > Note: optionally, you can supply `--max-time=60` to limit the run time to 60 seconds (or any other desired time limit).

    Once the run finishes, the `<OUTPUT_DIR>` will contain recorded progress information from the run. This folder should be able to load into [JetKlee Progress Explorer](https://github.com/staticafi/JetKleeProgressExplorer).


KLEE Symbolic Virtual Machine
=============================

[![Build Status](https://github.com/klee/klee/workflows/CI/badge.svg)](https://github.com/klee/klee/actions?query=workflow%3ACI)
[![Build Status](https://api.cirrus-ci.com/github/klee/klee.svg)](https://cirrus-ci.com/github/klee/klee)
[![Coverage](https://codecov.io/gh/klee/klee/branch/master/graph/badge.svg)](https://codecov.io/gh/klee/klee)

`KLEE` is a symbolic virtual machine built on top of the LLVM compiler
infrastructure. Currently, there are two primary components:

  1. The core symbolic virtual machine engine; this is responsible for
     executing LLVM bitcode modules with support for symbolic
     values. This is comprised of the code in lib/.

  2. A POSIX/Linux emulation layer oriented towards supporting uClibc,
     with additional support for making parts of the operating system
     environment symbolic.

To easily install required dependencies, see `installDependencies.sh` file included.

Additionally, there is a simple library for replaying computed inputs
on native code (for closed programs). There is also a more complicated
infrastructure for replaying the inputs generated for the POSIX/Linux
emulation layer, which handles running native programs in an
environment that matches a computed test input, including setting up
files, pipes, environment variables, and passing command line
arguments.

For further information, see the [webpage](http://klee.github.io/).