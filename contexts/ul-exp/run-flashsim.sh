#!/bin/bash

#
# Run a FlashSim SSD device instance.
#
# Usage: ./run-flashsim.sh cache|core
#


if [[ $# -ne 1 ]]; then
    echo "Usage: ./run-flashsim.sh cache|core"
    exit 1
fi


FLASHSIM_EXEC=../../flashsim/flashsim

(cd $(dirname ${FLASHSIM_EXEC}) && make)


if [[ $1 == "cache" ]]; then
    ${FLASHSIM_EXEC} cache-sock cache-ssd.conf
elif [[ $1 == "core" ]]; then
    ${FLASHSIM_EXEC} core-sock core-ssd.conf
else
    echo "Usage: ./run-flashsim.sh cache|core"
    exit 1
fi
