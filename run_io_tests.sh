#!/bin/bash

HOSTS="zeta sigma"
extra_args=""

function kill_tests()
{
	local h=""
	for h in ${HOSTS}
	do
		ssh $h killall iobench
	done
	killall iobench
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
	*write*) echo ${res} | awk '{print $2, $3}';;
	*) echo ${res};;
	esac
}

function execute_test()
{
	local h=""
	local LOG="${HOME}/iobench_results/$1.log"
	local TIME="$2"

	cd ${HOME}/iobench
	rm -f ./run_script
	cat << EOF > run_script
#!/bin/bash
cd ${HOME}/iobench
rm -f ${LOG}
ulimit -n 40960
./run_io_bench >& ${LOG}
EOF
	chmod 755 run_script
	for h in ${HOSTS}
	do
		scp run_io_bench run_script $h:iobench >&/dev/null &&
		scp ~/.io_bench_params $h: >& /dev/null
		if [ $? -ne 0 ]; then
			echo "Failed to copy files to $h" >&2
			return 1
		fi
	done

	for h in ${HOSTS}
	do
		ssh $h ${HOME}/iobench/run_script &
	done
	${HOME}/iobench/run_script &
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
		ssh $h killall iobench
	done
	killall iobench
	get_results ${LOG}
}

function run_test()
{
	local args=""
	local TST="$1"

	if [ "$(echo ${TST} | grep -E '^read')" != "" ]; then
		args=""
	elif [ "$(echo ${TST} | grep -E '^write')" != "" ]; then
		args="-w"
	elif [ "$(echo ${TST} | grep -E '^rw')" != "" ]; then
		args="=-wp  $(echo ${TST} | awk -F _ '{print $3}')"
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
		SFX="aio"
		extra_args="${extra_args} -engine aio_linux"
	fi
	if [ "$(echo ${extra_args} | grep '\-rr')" != "" ]; then
		SFX="${SFX}_rr"
	fi
	if [ "$(echo ${extra_args} | grep '\-numa')" != "" ]; then
		SFX="${SFX}_numa"
	fi
	mkdir -p ${HOME}/iobench_results
	for h in ${HOSTS}
	do
		ssh $h mkdir -p ${HOME}/iobench_results
	done
 
	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test read_rnd_hit_${SFX}_$i
	done

	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test read_seq_hit_${SFX}_$i
	done

	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test read_rnd_miss_${SFX}_$i
	done

	for i in 512 4K 8K 16K 32K 64K 128K 256K
	do
		run_test read_seq_miss_${SFX}_$i
	done
}

extra_args="$@"
trap kill_tests SIGINT
trap kill_tests SIGTERM

main
