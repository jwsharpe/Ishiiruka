#!/bin/bash -e
# build-linux.sh - simple script for invoking CMake
# Note that this will destroy any local configuration in build/Binaries/.

if [ -e "build/" ]; then
	rm -rf build/
	mkdir build
else
	mkdir build
fi

pushd build
cmake -DLINUX_LOCAL_DEV=true ../
protoc -I ../Source/Core/Core/Slippi/ --cpp_out=../Source/Core/Core/Slippi/ ../Source/Core/Core/Slippi/slippicomm.proto 
#cmake -DLINUX_LOCAL_DEV=true -DFASTLOG=true../
#cmake -DCMAKE_BUILD_TYPE=Debug -DLINUX_LOCAL_DEV=true ../
make -j7
popd

touch build/Binaries/portable.txt
cp -R Overwrite/* build/Binaries/
