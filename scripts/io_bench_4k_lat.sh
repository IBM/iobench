#!/bin/bash


# sample script to build latency curves for NVMeOF hosts
# Assumptions and requirements:
# * for this latency test, it is assumed that each mapped volume will have a single optimized path
# * specifically, to avoid issues with Linux NVMe load balancer, multiport hosts shall be connected multiple times, each
#   host association mapping unique volumes
# * each host association shall map equal number of devices
# * when ANA is used, it is assumed that single host association connects to two target ports (creating two controllers),
#   and each such controller sees all devices mapped to the host instance
# * number of "unique" host associations does not exceed 4 (can be 1, 2 or 3)

# Description
# block size is defined by the name of the script, by matching the following strings 512b, 4k, 8k, 16k, 32k, 64k, 128k, 256k
# if script name does not have "rw" or "wr" substring, read tests will be used
# if script name has "wr_" substring, write tests will be used
# if script name has "rw" substring, followed by a number, read-write tests will be used (rw70 means 30% write, 70% read)
# if sctipt name has "miss" substring, miss tests will be used. Otherwise, cache-hit tests will be used.
# the latency is measured by accessing increasing number of devices concurrently at queue depth 1.
# if script name has "seq" subtring, sequential IO tests will be used. Otherwise, random IO tests will be used.
# the first stage of the tests will start from a single pair of devices from one host association on a sigle host
# (a) then, a single pair of devices from one host association is added from each other host, one by one
# (b) then, a pair of devices on the first host is added from another first host accosiation
# (c) then, additional pair of devices from another controller is added on each other host one by one
# (d) the process repeats until all contollers on all hosts run with single pair of devices
# then, steps a-d are repeated to use 4, 6, 8, etc. devices from each host accosiation, until all devices are used
# at the second stage of the tests, all devices are used with equal increased queue depth (the first SECOND_STAGE_DEV_CONFIG devices from each host association)
# The test outputs bandwidth (MiB/sec), IOPS (KIOPs), and latency in microseconds (average, minimal and maximal)
# Latency curves are build as a function of latency vs IOPs or latency vs BW

# Quick recipe (how-to)
# must run as root
# Copy a sample file to the script that will run. Adjust the name of sthe script for the type of the test that will run.
# Edit the copied script:
# * change HOSTS="" line to match you host list. The HOSTS list shall list the host on swhich the script will run.
# * Optionally change TIMEOUT value to change each test duration
# * Opionally change PATTERN_FILE for write tests
# * Optionally change the number of devices used in second stage (SECOND_STAGE_DEV_CONFIG)
# Choose a host to run the script (master host), make sure it can SSH to other hosts without a password
# Create run_iobench directory in the home directory (typically /root) on all hosts
# Copy iobench and the modified latency script to that directory on the selected master host
# Run the created script on master host from run_iobench drirectory
# It is recommended to run the script in screen or tmux session and redirect the script output to a file

HOSTS="host1 host2 host3"
ME=$(hostname -s)
NGRPOUPS=""
GROUP1=()
GROUP2=()
GROUP3=()
GROUP4=()
GROUP_DEVS=()
BS=4K
SEQ_RND="rnd"
MISS_HIT="hit"
RW="read"
TIMEOUT=60
PATTERN_FILE=/perf_tool/patterns/8MB.65p
SECOND_STAGE_DEV_CONFIG="16"

function my_ssh()
{
	local h=$1
	shift
	if [ $h = ${ME} ]; then
		$@
	else
		ssh $h $@
	fi
}

function kill_tests()
{
	local h=""
	for h in ${HOSTS}
	do
		my_ssh $h killall iobench run_script_$h >& /dev/null
	done
	exit 1
}



function list_controller_devs()
{
	ls -d   /sys/class/nvme/$1/nvme*c*n*| #list devs as /sys/....
	sed -e 's/.*\(nvme.*c.*n\)/\/dev\/\1 /' | #replace /sys/*nvme*c*nX to /dev/nvme*c*n X
	awk '{print $2, $1}' | #print number first (X)
	sort -n | #sort by number (X)
	awk '{print $2,$1}'| #print number (X) last
	sed -e 's/\(nvme[0-9][0-9]\)*c[0-9][0-9]*/\1/' | # convert nvme*cYn to nvme*n
	tr -d ' '|  #remove space between /dev/nvme*n and X (print /dev/nvme*X)
	tr '\n' ' ' #replace new line with space
}

function write_groups()
{
	local i=""
	cd /sys/class/nvme
	for i in *
	do
		list_controller_devs $i
		echo
	done
}

function set_groups()
{
	local i=""
	local g=""
	local GFILE=""
	local l1=""
	local l2=""
	local tbl=""
	NGRPOUPS=$(ls /sys/class/nvme| wc -l)
	if [ "${NGRPOUPS}" = "" ]; then
		echo "No NVMe controllers" >&2
		exit 1
	fi
	if [ $[${NGRPOUPS} % 2] -ne 0 ]; then
		echo "Number of NNVMe controllers ${NGRPOUPS} is not even" >&2
		exit 1
	fi
	NGRPOUPS=$[${NGRPOUPS} / 2]
	GFILE=$(mktemp -q  /tmp/grp.XXXXXXXX)
	if [ ! -e ${GFILE} ]; then
		echo "Failed to write groups file" >&2
		exit 1
	fi
	write_groups | sort -u | # sort
	sed -e 's/\(nvme[0-9][0-9]*\)n\(.\) /\1n0\2 /g' | #replace 'n1-9 ' with 'n01-09'
	sort -n | # devices 'n1-9' are on the first lines now, because we added zero
	sed -e 's/\(nvme[0-9][0-9]*\)n0/\1n/g' > ${GFILE} # replace 'n01-09' with n0-9

	if [ "$(wc -l ${GFILE} | awk '{printf "%s", $1}')" -ne ${NGRPOUPS} ]; then
		echo "Unexpected number of groups, expect ${NGRPOUPS}, group file is: " >&2
		cat ${GFILE} >&2
		rm -f ${GFILE}
		exit 1
	fi
	for i in `seq 1 ${NGRPOUPS}`
	do
		eval GROUP${i}="($(sed -n ${i}p  ${GFILE}))"
		tbl=GROUP${i}
		eval ls '${'$tbl'[*]}' >& /dev/null
		if [ $? -ne 0 ]; then
			echo "Cannot find all devices in GROUP$i" >&2
			eval "echo GROUP{$i} devices are  ${GROUP${i}[*]} >&2"
			rm -f ${GFILE}
			exit 1
		fi
		GROUP_DEVS[$[$i-1]]=0
	done
	rm -f ${GFILE}

	l1=$(echo ${#GROUP1[*]})
	for i in `seq 2 ${NGRPOUPS}`
	do
		tbl=GROUP${i}
		l2=$(eval echo '${#'$tbl'[*]}')
		if [ $l1 -ne $l2 ]; then
			"echo GROUP$i has different number of devices ($l2) from GROUP1 ($l1)" >&2
			exit 1
		fi
	done

}

function make_next2()
{
	local i=0
	local h=""
	local hh=""

	eval tbl_$ME="(${GROUP_DEVS[*]})"
	for h in ${HOSTS}
	do
		eval tbl_$h="(${GROUP_DEVS[*]})"
	done

	while [ $i -lt ${NGRPOUPS} ]
	do

		for h in ${HOSTS}
		do
			eval tbl_${h}[$i]=$[2+${GROUP_DEVS[$i]}]
			for h in ${HOSTS}
			do
				echo -n "$h='"
				eval echo -n '${'tbl_$h'[*]}'
				echo -n "';"
			done
			echo
		done
		GROUP_DEVS[$i]=$[2+${GROUP_DEVS[$i]}]
		i=$[$i+1]
	done

}

function build_dev_list()
{
	local i=1
	local j=""
	local k=""
	for k in $@
	do
		j=0
		while [ $j -lt $k ];
		do
			eval echo -n '${'GROUP$i'[$j]}'
			echo -n " "
			j=$[$j+1]
		done
		i=$[$i+1]
	done
}

function build_script()
{
	local devs=""
	devs=$(build_dev_list ${!1})
	cat << EOF > run_script_$1
#!/bin/bash
cd ${HOME}/run_iobench
rm -f ${LOG}
(echo ${BASE_ARGS} -qs ${QS} -cpuset 0-63 ${devs}
${BASE_ARGS} -qs ${QS} -cpuset 0-63 ${devs}
echo EXITED) >& ${LOG}
EOF
	chmod +x run_script_$1
}

function parse_one_result()
{
	my_ssh $1 tail -4 ${LOG}  |   grep -A 1 "INF: -----" | grep -v '\----' | awk '{print $3, $5, $7, $8, $9, $4, $6, $10, $11, $12}'
}

function is_h_null()
{
	local h="$1"
	if [ "$(echo ${!h} | tr ' ' '\n' | grep -w  -v 0)" = "" ]; then
		return 0
	fi
	return 1
}

function get_results()
{
	local h=""
	local cnt=1
	local res=""
	local one=""
	local QD_STRING=""
	local qd=0
	local el=""

	res=$(parse_one_result ${ME})
	QD_STRING="${!ME}"
	for h in ${HOSTS}
	do
		if [ $h = ${ME} ]; then
			continue
		fi
		if is_h_null $h; then
			continue
		fi
		QD_STRING="${QD_STRING} ${!h}"
		one=$(parse_one_result $h)
		res=$(echo ${res} ${one} | awk '{print $1+$11, $2+$12, $3+$13, $4+$14, $5+$15, $6+$16, $7+$17, $8+$18, $9+$19, $10+$20}')
		cnt=$[${cnt}+1]
	done
	for el in ${QD_STRING}
	do
		qd=$[${qd}+${el}]
	done
	qd=$[${qd} * ${QS}]
	echo -n "${qd} "
	case ${LOG} in
		*read*) echo ${cnt} ${res} | awk '{printf "%.2lf %.2lf %lu %lu %lu\n", $2, $3, $4/$1, $5/$1, $6/$1}';;
		*write*) echo ${cnt} ${res} | awk '{printf "%.2lf %.2lf %lu %lu %lu\n", $7, $8, $9/$1, $10/$1, $11/$1}';;
		*) echo ${cnt} ${res} | awk '{printf "%.2lf %.2lf %lu %lu %lu %.2lf %.2lf %lu %lu %lu\n", $2, $3, $4/$1, $5/$1, $6/$1, $7, $8, $9/$1, $10/$1, $11/$1}';;
	esac
}

function run_once()
{
	local h=""
	LOG="${HOME}/iobench_results/${RW}_${BS}_${SEQ_RND}_${MISS_HIT}_qs=${QS}"
	for h in ${HOSTS}
	do
		LOG="${LOG}_$h=$(echo ${!h}| tr -d ' ')"
	done
	for h in ${HOSTS}
	do
		if ! is_h_null $h; then
			build_script $h
			if [ "$h" != ${ME} ]; then
				scp -p run_script_$h $h:${HOME}/run_iobench >& /dev/null
				if [ $? -ne 0 ]; then
					echo "Failed to copy files to $h" >&2
					return 1
				fi
			fi
		fi
	done
	for h in ${HOSTS}
	do
		if ! is_h_null $h; then
			my_ssh $h ${HOME}/run_iobench/run_script_$h &
		fi
	done
	sleep ${TIMEOUT}
	for h in ${HOSTS}
	do
		if is_h_null $h; then
			continue
		fi
		if [ "$(my_ssh $h grep EXITED ${LOG})" != "" ]; then
			echo "failed on $h" >&2
		fi
	done
	for h in ${HOSTS}
	do
		if is_h_null $h; then
			continue
		fi
		my_ssh $h killall iobench  >& /dev/null
	done
	sleep 5
	get_results
}

function run_group()
{
	local n=1
	local line=""

	line=$(sed -n 1p ${TESTS_FILE})
	while [ "${line}" != "" ]
	do
		eval ${line}
		run_once
		n=$[${n}+1]
		line=$(sed -n ${n}p ${TESTS_FILE})
	done
}

function set_test_params()
{
	case ${PROG} in
		*64k*) BS=64K;;
		*8k*) BS=8K;;
		*16k*) BS=16K;;
		*32k*) BS=32K;;
		*4k*) BS=4K;;
		*128k*) BS=128K;;
		*256k*) BS=256K;;
		*512b*) BS=512;;
	*)
		echo "Unsupported block size in test name" >&2
		exit 1
		;;
	esac

	case ${PROG} in
	*seq*)
		SEQ_RND="seq";;
	*)
		SEQ_RND="rnd";;
	esac

	case ${PROG} in
	*miss*)
		MISS_HIT="miss";;
	*)
		MISS_HIT="hit";;
	esac

	case ${PROG} in
	*wr_*)
		RW="write";;
	*rw*)
		RW=$(echo ${PROG} | sed -e 's/.*rw\([0-9][0-9]*\).*/\1/')
		if [ "$(echo "${RW}"| grep -E '^[0-9][0-9]*$')" = "" ] || [ ${RW} -gt 99 ]; then
			echo "Invalid read percent ratio in RW test" >&1
			exit 1
		fi
		RW=$[100-${RW}]
		RW="wp${RW}"
		;;
	*)
		RW="read";;
	esac
}

PROG=$(basename $0)
set_test_params

cd ~/run_iobench
if [ $(echo ${HOSTS} | tr ' ' '\n' | grep -E "^${ME}$") = "" ]; then
	echo "Local host $ME is not in the host list -- host list is wrong?" >&2
	exit 1
fi

HOSTS=$(echo ${HOSTS} | tr ' ' '\n' | grep -vE "^${ME}$" | tr '\n' ' ')
HOSTS="${ME} ${HOSTS}"

QS=1
trap kill_tests SIGINT
trap kill_tests SIGTERM

BASE_ARGS="./iobench -bs ${BS}"
if [ "${MISS_HIT}" = hit ]; then
	BASE_ARGS="${BASE_ARGS} -hit-size 20M"
fi

if [ "${SEQ_RND}" = "seq" ]; then
	BASE_ARGS="${BASE_ARGS} -seq"
fi

if [ "${RW}" = write ]; then
	BASE_ARGS="${BASE_ARGS} -write -pf ${PATTERN_FILE}"
elif [ "$(echo ${RW}| grep wp)" != "" ]; then
	wp=$(echo ${RW}| sed -e 's/wp//')
	BASE_ARGS="${BASE_ARGS} -wp ${wp} -pf ${PATTERN_FILE}"
fi

TESTS_FILE=$(mktemp -q /tmp/tests.XXXXXXXX)
if [ $? -ne 0 ]; then
	echo "Cannot create tests file" >&2
	exit 1
fi

set_groups
devs=${#GROUP1[*]}
done=0
while [ ${done} -lt ${devs} ]
do
	make_next2 > ${TESTS_FILE}
	run_group
	done=$[${done}+2]
done

rm -f ${TESTS_FILE}

for h in ${HOSTS}
do
	eval "$h=\"${SECOND_STAGE_DEV_CONFIG} ${SECOND_STAGE_DEV_CONFIG} ${SECOND_STAGE_DEV_CONFIG} ${SECOND_STAGE_DEV_CONFIG}\""
done

QS=2
while [ ${QS} -le 128 ]
do
	run_once
	QS=$[${QS}+2]
done
