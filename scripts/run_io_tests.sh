#!/bin/bash

# sample script to run a workload on multiple hosts and report KIOPs and bandwidth in MiB/sec
# runs random hit, seqential hit, random miss and sequential miss tests, for 512, 4K, 8K, 16K, 32K, 64K, 128K and 256K block sizes
# No arguments will trigger read tests
# "-w" argument will trigger write tests
# "-wp n" argument will trigger read-write test, with n % write ratio

# How to run:
# must run as root
# choose hosts where the the tool will run, and connect devices
# choose a master host among the hosts (where the script will run)
# Create your test_profile from one of the samples:
# * specify your hosts in HOSTS="..." line. Master host shall be present in the list as well.
# * update choose_qs() function if needed. This function selects queue size for each test and block size.
#   * Script vrun can be used to select to find the best parameters manually
# * update choose_numa() function if needed. This function is used to set affinity parameters.
#    * Script vrun can be used to select to find the best parameters manually
# * optional: change single test duration by updating TEST_DURATION variable (default is 60 seconds for each test)
# * optional: change PATTERN_FILE variable (used for write tests only, /perf_tool/patterns/8MB.65p)
#    * pattetn file must be present on all hosts
# make sure that master host can ssh other hosts without a password
# on master host, create run_iobench directory in the home directory (typically /root)
# copy test_profile, run_io_tests.sh iobench and run_io_bench files to the created directory
# on master host, execute in run_iobench directory one of (depending on the required test):
# * ./run_io_tests.sh
# * ./run_io_tests.sh -w
# * ./run_io_tests.sh -wp number
# It is recommended to run the script in screen/tmux session and redirect output to a file.

extra_args=""
PF=""
HIT_SIZE="20M"
IS_SEQ=0
IS_MISS=0
IS_WRITE=0
FORCE_NUMA=""
TEST_DURATION=60 #can be overidden by test_profile
PATTERN_FILE=/perf_tool/patterns/8MB.65p #can be overidden by test_profile
ME=$(hostname -s)

if [ ! -e test_profile ]; then
	echo "Test profile file (test_profile) is not present" >&2
	exit 1
fi

. ./test_profile
if [ "$(echo ${HOSTS} | grep -w ${ME})" = "" ]; then
	echo "HOSTS lists does not contain this host ($(hostname -s))" >&2
	exit 1
fi

ME="$(hostname -s)"
HOSTS=$(echo ${HOSTS} | tr ' ' '\n' | grep -v -E "^${ME}$" | tr '\n' ' ')

function kill_tests()
{
	local h=""
	for h in ${HOSTS}
	do
		ssh $h killall iobench run_script run_io_bench >& /dev/null
	done
	killall iobench run_script run_io_bench	>& /dev/null
	exit 1
}

function get_one_result()
{
	if [ "$2" = "" ]; then
		tail -4 "$1"  | grep -A 1 "INF: -----" | tail -1 | awk '{print $3, $4, $5, $6}'
	else
		ssh $2 tail -4 "$1"  | grep -A 1 "INF: -----" | tail -1 | awk '{print $3, $4, $5, $6}'
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
		ssh $h killall iobench run_script run_io_bench >& /dev/null
	done
	killall iobench >& /dev/null
	sleep 5
	get_results ${LOG}
}

function run_test()
{
	local args=""
	local TST="$1"
	local BS=""
	local QS=""

	IS_SEQ=0
	IS_MISS=0
	IS_WRITE=0	
	if [ "$(echo ${TST} | grep -E '^read')" != "" ]; then
		args=""
	elif [ "$(echo ${TST} | grep -E '^write')" != "" ]; then
		args="-write"
		IS_WRITE=1
	elif [ "$(echo ${TST} | grep -E '^rw')" != "" ]; then
		args="-wp  $(echo ${TST} | awk -F _ '{print $3}')"
	else
		echo "Invalid test" >&2
		return 1
	fi
	if [ "$(echo $TST| grep seq)" != "" ]; then
		args="${args} -seq"
		IS_SEQ=1
	elif [ "$(echo $TST| grep rnd)" == "" ]; then
		echo "Invalid test" >&2
		return 1
	fi
	if [ "$(echo $TST| grep hit)" != "" ]; then
		args="${args} -hit-size ${HIT_SIZE}" 
	else
		IS_MISS=1
		if [ "$(echo $TST| grep miss)" == "" ]; then
			echo "Invalid test" >&2
			return 1
		fi
	fi
	BS="$(echo ${TST} | awk -F _ '{print $NF}')"
	if [ "$(echo ${args} | grep '\-qs')" = "" ]; then
		QS="$(choose_qs ${BS})"
	fi
	args="${args} -bs ${BS} ${QS} ${PF} $(choose_numa ${BS})"
	args="$(echo ${args} ${extra_args} | sed -e 's/  *$//')"
	rm -f  ~/.io_bench_params
	echo "args=\"${args}\"" >  ~/.io_bench_params
	echo -n "$TST: "
	execute_test ${TST} ${TEST_DURATION}
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
			PF="-pf ${PATTERN_FILE}"
			shift
		elif [ "$1" = "-wp" ]; then
			shift
			WP=$1
			shift
			TST="rw_$[100-${WP}]_${WP}"
			PF="-pf ${PATTERN_FILE}"
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
		ssh $h mkdir -p ${HOME}/iobench_results  ${HOME}/run_iobench >& /dev/null &&
		scp ${HOME}/run_iobench/* $h:${HOME}/run_iobench >& /dev/null
		if [ $? -ne 0 ]; then
			echo "Failed to access host $h" >&2
			exit 1
		fi
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
