#!/bin/bash

comb_rec() {
    local -r start="${1}"
    local -r stackLen=${#stack[@]}
    if [ "${stackLen}" -eq "${k}" ]; then
        # found combination
        echo "${stack[@]}"
        return 0
    fi
    local i="${start}"
    for ((; i < arrLen; ++i)); do
        stack[${stackLen}]=${array[${i}]}
        # recursive call
        comb_rec $((i+1))
        unset "stack[-1]"
    done
    return 0
}

comb() {
    # Brief: Generates k-length combinations of elements in the given array
    # Usage: comb [array] [k]
    local -n array="${1}"
    local -r k="${2}"
    local arrLen=${#array[@]}
    local -a stack
    comb_rec 0
    return 0
}

clean_up() {
    set +o errexit # ignore error when cleaning
    # kill all non-terminated child
    for pid in "${pids[@]}"; do
        if [ "${pid}" -ne -1 ]; then 
            # kill only if process exists
            ps -p "${pid}" > /dev/null 2>&1 && kill "${pid}"
        fi
    done
    # remove fifo and temp files
    rm -f combinations.tmp
    # close all opened fds
    for fd in "${fds[@]}"; do
        exec {fd}<&-
    done
    for ((i=0; i <= n_host; ++i)); do
        rm -f "fifo_${i}.tmp"
    done
}

set -o errexit   # abort on nonzero exitstatus
set -o pipefail  # don't hide errors within pipes

# check argument
if [ "$#" -lt 2 ]; then
    echo "Error: Not enough arguments" 1>&2
    echo "Usage: bash auction_system.sh [n_host] [n_player]" 1>&2
    exit 1;
fi

readonly n_host="${1}"
readonly n_player="${2}"
readonly batch_size=8
declare -a pids
declare -a fds
declare -a score
TERMINATE_STR="-1";
for ((i=1; i<batch_size; ++i)); do
    TERMINATE_STR+=' -1'
done

# initialize player_ids and score
readarray -t player_ids < <(seq "${n_player}")
for player_id in ${player_ids[*]}; do
    score[${player_id}]=0
done

# make fifo and start hosts in the background
mkfifo combinations.tmp
mkfifo fifo_0.tmp

# start comb in the background
comb player_ids "${batch_size}" > combinations.tmp &
# open fd for reading
exec {fd}<combinations.tmp
combFd="${fd}"
fds+=("${fd}")
for ((i=1; i <= n_host; i=i+1)); do
    mkfifo "fifo_${i}.tmp"
    ./host "${i}" "${i}" 0 &
    exec {fd}>"fifo_${i}.tmp" # blocks until fifo is opened for read by someone
    fds+=("${fd}")
    pids[${i}]=$!
done
# run `clean_up` on exit or error
trap clean_up EXIT ERR

# send initial request to hosts
noComb=0
for host_id in ${!pids[*]}; do
    if [ "${noComb}" -eq 0 ] && read -u "${combFd}" -r comb; then
        echo "${comb}" > "fifo_${host_id}.tmp"
    else
        echo "${TERMINATE_STR}" > "fifo_${host_id}.tmp"
        pids[${host_id}]=-1
        noComb=1
    fi
done

while read -r key; do
    host_id="${key}"
    for ((i=0; i < batch_size; ++i)); do
        read -r player_id player_rank;
        score[${player_id}]=$((score[player_id]+8-player_rank))
    done
    # send next request to host
    if [ "${noComb}" -eq 0 ] && read -u "${combFd}" -r comb; then
        echo "${comb}" > "fifo_${host_id}.tmp"
    else
        echo "${TERMINATE_STR}" > "fifo_${host_id}.tmp"
        pids[${host_id}]=-1
        noComb=1
    fi
done < fifo_0.tmp

for player_id in "${player_ids[@]}"; do
    echo "${player_id} ${score[${player_id}]}"
done

# wait for all hosts and comb to finish
wait