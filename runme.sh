#!/bin/bash

BUILD_DIR=@BUILD
DATA_DIR=@DATA

if [ ! -s $BUILD_DIR/Makefile ]; then
	echo "*** update submodule(s)..."
	git submodule sync
	git submodule update --init --recursive

	echo "*** cleanup..."
	git clean -x -f -d
	git submodule foreach --recursive 'git clean -x -f -d'

	echo "*** run cmake..."
	mkdir -p $BUILD_DIR && \
		(cd $BUILD_DIR && cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo .. \
		-DENABLE_ROCKSDB=OFF \
		-DENABLE_FORESTDB=OFF \
		-DENABLE_SOPHIA=OFF \
		-DENABLE_WIREDTIGER=OFF \
		-DENABLE_LMDB=OFF \
		-DENABLE_MDBX=ON \
		-DENABLE_LEVELDB=OFF \
		-DENABLE_SQLITE3=OFF \
		-DENABLE_VEDISDB=OFF \
		-DENABLE_IOWOW=OFF \
		) || exit 1
fi

echo "*** run make..."
# $@ for additional params like ./runme.sh VERBOSE=1 -j 8
make "$@" -C $BUILD_DIR

echo "--------------------------------------------------------------------------------------"

function bench {
	local mode=$1
	local count=$2
	local options=$3
	echo -n "Bench: mode $mode, $options, $count iterations... "
	test -z "$DATA_DIR" -o ! -d $DATA_DIR || rm -rf $DATA_DIR/* || (echo "Cleanup FAILED!" >&2; exit -1)
	$BUILD_DIR/src/ioarena -p $DATA_DIR -D mdbx -o ${options} -B crud -m ${mode} -n ${count} | tee bench-$mode-$options-$count.log | grep 'throughput'
}

bench sync 100000 NoMetaSync=OFF,LifoReclaim=OFF
bench sync 100000 NoMetaSync=OFF,LifoReclaim=ON
bench sync 100000 NoMetaSync=ON,LifoReclaim=OFF
bench sync 100000 NoMetaSync=ON,LifoReclaim=ON
