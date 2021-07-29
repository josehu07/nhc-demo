#!/bin/bash

#
# Do a round of throughput benchmark with given parameters. Must have two
# flashsim instances (cache + core) running. It is recommended to kill and
# restart the flashsim instances before every invocation of this script.
#
# Usage: ./tp-benchmark.sh MODE INTENSITY READ_PERCENT HIT_RATIO
#


if [[ $# -ne 4 ]]; then
    echo "Usage: ./tp-benchmark.sh MODE INTENSITY READ_PERCENT HIT_RATIO"
    exit 1
fi


if [[ ! -d result ]]; then
    mkdir result
fi

if [[ ! -d logs ]]; then
    mkdir logs
fi


./bench $1 throughput $2 $3 $4 | tee result/bench-int$2-read$3-hit$4-$1.txt


if [[ $? -ne 0 ]]; then
    echo "Warn: throughput benchmark not successful"
    exit 1
fi
