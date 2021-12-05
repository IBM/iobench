#!/bin/bash

HOSTS="zeta sigma"
extra_args=""

function kill_tests()
{
	local h=""
	for h in ${HOSTS}
	do
		ssh $h killall iobench run_script run_io_bench
	done
	killall iobench run_script run_io_bench
	exit 1
}

function get_one_result()
{
	if [ "$2" = "" ]; then
		tail -4 "$1"  | grep -A 1 "INF: -----" | tail -1 | awk '{print $2, $3, $4, $5}'
	else
		ssh $2 tail -4 "$1"  | grep -A 1 "INF: -----" | tail -1 | awk '{print $2, $3, $4, $5}'
	fi
}

function get_results()
{
	local one=""
	local res="0 0 0 0"
	local LOG=$1
	local h=""

	one=$(get_one_result ${LOG})
	if [ "${one}" = "" ]; then
		one="0 0 0 0"
	fi
	res=$(echo ${res} ${one} | awk '{printf "%.2lf %.2lf %.2lf %.2lf\n", $1+$5, $2+$6, $3+$7, $4+$8}')
	for h in ${HOSTS}
	do
		one=$(get_one_result ${LOG} $h)
		if [ "${one}" = "" ]; then
			one="0 0 0 0"
		fi
		res=$(echo ${res} ${one} | awk '{printf "%.2lf %.2lf %.2lf %.2lf\n", $1+$5, $2+$6, $3+$7, $4+$8}')
	done
	case ${LOG} in
	*read*) echo ${res} | awk '{print $1, $3}';;
	*write*) echo ${res} | awk '{print $2, $4}';;
	*) echo ${res};;
	esac
}

function execute_test()
{
	local h=""
	local LOG="${HOME}/iobench_results/$1.log"
	local TIME="$2"

	cd ${HOME}/run_iobench
	rm -f ./run_script
	cat << EOF > run_script
#!/bin/bash
cd ${HOME}/run_iobench
rm -f ${LOG}
ulimit -n 40960
./run_io_bench >& ${LOG}
EOF
	chmod 755 run_script
	for h in ${HOSTS}
	do
		scp run_script $h:${HOME}/run_iobench >&/dev/null &&
		scp ~/.io_bench_params $h: >& /dev/null
		if [ $? -ne 0 ]; then
			echo "Failed to copy files to $h" >&2
			return 1
		fi
	done

	for h in ${HOSTS}
	do
		ssh $h ${HOME}/run_iobench/run_script &
	done
	${HOME}/run_iobench/run_script &
	sleep ${TIME}
	for h in ${HOSTS}
	do
		if [ "$(ssh $h grep EXITED ${LOG})" != "" ]; then
			echo "failed on $h" >&2
		fi
	done
	if [ "$(grep EXITED ${LOG})" != "" ]; then
		echo "failed on ${hostname}" >&2
	fi
	for h in ${HOSTS}
	do
		ssh $h killall iobench run_script run_io_bench
	done
	killall iobench
	sleep 5
	get_results ${LOG}
}

HIT_SIZE="20M"

function choose_qs()
{
	case $1 in
		256K|128K|64K) echo "-qs 4";;
		32K) echo "-qs 8 -threads-per-dev 2";;
		16K) echo "-qs 32";;
		*) echo "-qs 128 -threads-per-dev 2";;
	esac
}

FORCE_NUMA=""
function choose_numa()
{
	if [ "${FORCE_NUMA}" = "nonuma" ]; then
		echo ""
		return 0
	elif [  "${FORCE_NUMA}" = "numa" ]; then
		echo "-numa"
		return 0
	fi
	case $1 in
		32K) echo "-numa";;
		*) echo "-cpuset 0-31";;
	esac
}

function run_test()
{
	local args=""
	local TST="$1"
	local BS=""
	local QS=""

	if [ "$(echo ${TST} | grep -E '^read')" != "" ]; then
		args=""
	elif [ "$(echo ${TST} | grep -E '^write')" != "" ]; then
		args="-write"
	elif [ "$(echo ${TST} | grep -E '^rw')" != "" ]; then
		args="-wp  $(echo ${TST} | awk -F _ '{print $3}')"
	else
		echo "Invalid test" >&2
		return 1
	fi
	if [ "$(echo $TST| grep seq)" != "" ]; then
		args="${args} -seq"
	elif [ "$(echo $TST| grep rnd)" == "" ]; then
		echo "Invalid test" >&2
		return 1
	fi
	if [ "$(echo $TST| grep hit)" != "" ]; then
		args="${args} -hit-size ${HIT_SIZE}" 
	elif [ "$(echo $TST| grep miss)" == "" ]; then
		echo "Invalid test" >&2
		return 1
	fi
	BS="$(echo ${TST} | awk -F _ '{print $NF}')"
	if [ "$(echo ${args} | grep '\-qs')" = "" ]; then
		QS="$(choose_qs ${BS})"
	fi
	args="${args} -bs ${BS} ${QS} $(choose_numa ${BS})"
	args="$(echo ${args} ${extra_args} | sed -e 's/  *$//')"
	rm -f  ~/.io_bench_params
	echo "args=\"${args}\"" >  ~/.io_bench_params
	echo -n "$TST: "
	execute_test ${TST} 60
}

function main()
{
	local SFX=""
	local h=""
	local RR=0
	local TST="read"
	local WP=""

	while [ $# -ne 0 ]
	do
		if [ "$1" = "-engine" ]; then
			shift
			extra_args="${extra_args} -engine $1"
			SFX="$1"
			shift
		elif [ "$1" = "-numa" ]; then
			shift
			FORCE_NUMA="numa"
			noauto_numa=1
			NUMA=1
		elif [ "$1" = "-nonuma" ]; then
			shift
			FORCE_NUMA="nonuma"
			noauto_numa=1
		elif [ "$1" = "-rr" ]; then
			shift
			extra_args="${extra_args} -rr"
			RR=1
		elif [ "$1" = "-qs" ]; then
			shift
			extra_args="${extra_args} -qs $1"
			shift
		elif [ "$1" = "-hit-size" ]; then
			shift
			HIT_SIZE="$1"
			shift
		elif [ "$1" = "-w" ]; then
			TST=write
			shift
		elif [ "$1" = "-wp" ]; then
			shift
			WP=$1
			shift
			TST="rw_$[100-${WP}]_${WP}"
		else
			echo "Wrong use: allowed args: -numa|-nonuma -qs val -rr -engine eng" >&2
			return 1
		fi
	done
	if [ "${SFX}" = "" ]; then
		extra_args="${extra_args} -engine aio_linux"
		SFX=aio
	fi

	if [ "${RR}" = 1 ]; then
		SFX="${SFX}_rr"
	fi
	if [ "${FORCE_NUMA}" = numa ]; then
		SFX="${SFX}_numa"
	else
		SFX="${SFX}_auto"
	fi
	mkdir -p ${HOME}/iobench_results
	for h in ${HOSTS}
	do
		ssh $h mkdir -p ${HOME}/iobench_results  ${HOME}/run_iobench
		scp ${HOME}/run_iobench/* $h:${HOME}/run_iobench
	done

        #HIT_SIZE=12800K
	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test ${TST}_rnd_hit_${SFX}_$i
	done

	#HIT_SIZE=6400K
	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test ${TST}_seq_hit_${SFX}_$i
	done

	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test ${TST}_rnd_miss_${SFX}_$i
	done

	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test ${TST}_seq_miss_${SFX}_$i
	done
}

trap kill_tests SIGINT
trap kill_tests SIGTERM

main $@

