#!/bin/bash

HOSTS="zeta sigma"
extra_args=""

function kill_tests()
{
	local h=""
	for h in ${HOSTS}
	do
		ssh $h killall iobench run_script run_io_bench >& /dev/null
	done
	killall iobench run_script run_io_bench >& /dev/null
	exit 1
}

function get_one_result()
{
	if [ "$2" = "" ]; then
		tail -4 "$1"  | grep -A 1 "INF: -----" | tail -1 | awk '{print $6, $7, $8, $9, $10, $11}'
	else
		ssh $2 tail -4 "$1"  | grep -A 1 "INF: -----" | tail -1 | awk '{print $6, $7, $8, $9, $10, $11}'
	fi
}

function update_results()
{
	awk 'BEGIN {
		getline
		avg_r=$1+$7
		avg_w=$4+$10
		min_r=$2+$8
		max_r=$3+$9
		min_w=$5+$11
		max_w=$6+$12
		printf("%.2lf %lu %lu %.2lf %lu %lu\n"), avg_r, min_r, max_r, avg_w, min_w, max_w 
	}'
}

function get_results()
{
	local one=""
	local LOG=$1
	local h=""
	local cnt=0

	res=$(get_one_result ${LOG})
	cnt=$[${cnt}+1]
	for h in ${HOSTS}
	do
		one=$(get_one_result ${LOG} $h)
		res=$(echo ${res} ${one} | update_results)
		cnt=$[${cnt}+1]
	done
	case ${LOG} in
	*read*) echo ${cnt} ${res} | awk '{printf "%.2lf %lu %lu\n", $2/$1, $3/$1, $4/$1}';;
	*write*) echo ${cnt} ${res} | awk '{printf "%.2lf %lu %lu\n", $5/$1, $6/$1, $7/$1}';;
	*) echo ${cnt} ${res} | awk '{printf "%.2lf %lu %lu %.2lf %lu %lu\n", $2/$1, $3/$1, $4/$1, $5/$1, $6/$1, $7/$1}';;
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
		ssh $h killall iobench run_script run_io_bench >& /dev/null
	done
	killall iobench run_script run_io_bench >& /dev/null
	sleep 5
	get_results ${LOG}
}

function run_test()
{
	local args=""
	local TST="$1"
	local SFX=""

	if [ "$(echo ${extra_args} | grep -E '\-write')" != "" ]; then
		TST=write_${TST}
	elif [ "$(echo ${extra_args} | grep -E '\-wp')" != "" ]; then
		SFX=$(echo ${extra_args}  | sed -e 's/.*-wp//' | awk '{printf "%d_%d\n", 100-$1, $1}')
		TST=rw_${SFX}_${TST}
	else
		TST=read_${TST}
	fi
	if [ "$(echo $TST| grep seq)" != "" ]; then
		args="${args} -seq"
	elif [ "$(echo $TST| grep rnd)" == "" ]; then
		echo "Invalid test" >&2
		return 1
	fi
	if [ "$(echo $TST| grep hit)" != "" ]; then
		args="${args} -hit-size 20M" 
	elif [ "$(echo $TST| grep miss)" == "" ]; then
		echo "Invalid test" >&2
		return 1
	fi
	args="${args} -bs $(echo ${TST} | awk -F _ '{print $NF}')"
	rm -f  ~/.io_bench_params
	echo "args=\"${args} ${extra_args}\"" >  ~/.io_bench_params
	echo -n "$TST: "
	execute_test ${TST} 60
}

function main()
{
	local SFX=""
	local h=""

	SFX=$(echo ${extra_args} | grep engine | sed -e 's/.*engine //' | awk '{print $1}')
	if [ "${SFX}" = "" ]; then
		SFX="dio"
		extra_args="-engine dio ${extra_args}"
	fi
	if [ "$(echo ${extra_args} | grep '\-rr')" != "" ]; then
		SFX="${SFX}_rr"
	fi
	if [ "$(echo ${extra_args} | grep '\-numa')" != "" ]; then
		SFX="${SFX}_numa"
	fi
	if [ "$(echo ${extra_args} | grep '/dev/')" != "" ]; then
		SFX=${SFX}_single
	fi
	SFX=${SFX}_lat
	extra_args="-qs 1 ${extra_args}"
	mkdir -p ${HOME}/iobench_results
	for h in ${HOSTS}
	do
		ssh $h mkdir -p ${HOME}/iobench_results ${HOME}/run_iobench
		scp ${HOME}/run_iobench/* $h:${HOME}/run_iobench >& /dev/null
	done
 
	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test rnd_hit_${SFX}_$i
	done

	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test seq_hit_${SFX}_$i
	done

	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test rnd_miss_${SFX}_$i
	done

	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test seq_miss_${SFX}_$i
	done
}

extra_args="$@"
trap kill_tests SIGINT
trap kill_tests SIGTERM

main
