#!/bin/bash

RF_HOME=/srv/openvapour/RouteFlow
CONTROLLER_PORT=6633
LXCDIR=/var/lib/lxc
RFVM1=$LXCDIR/rfvm1
RFBR=br-rf
RFDP=br-dp
RFDPID=7266767372667673
OFP=OpenFlow13
RFSERVERCONF=/etc/routeflow/rfserverconfig.csv
RFSERVERINTERNAL=/etc/routeflow/rfserverinternal.csv
RFSERVERDPIDMODE=/etc/routeflow/rfserverdptypes.csv
HOSTVMIP=172.30.20.253/28
RFVM1IP=172.30.20.254
OVSSOCK=/var/run/openvswitch/db.sock
VSCTL="ovs-vsctl --db=unix:$OVSSOCK"
OFCTL="ovs-ofctl -O$OFP"
source /srv/openvapour/pythonenv/bin/activate
export PATH=$PATH:/usr/local/bin:/usr/local/sbin
export PYTHONPATH=$PYTHONPATH:$RF_HOME

#modprobe 8021q
ulimit -c 1000000000

if [ "$EUID" != "0" ]; then
  echo "You must be root to run this script."
  exit 1
fi

ACTION=""
case "$1" in
--ryu)
    ACTION="RYU"
    ;;
--reset)
    ACTION="RESET"
    ;;
*)
    echo "Invalid argument: $1"
    echo "Options: "
    echo "    --ryu: run using RYU"
    echo "    --reset: stop running and clear data from previous executions"
    exit
    ;;
esac

cd $RF_HOME

wait_port_listen() {
    port=$1
    while ! `nc -z localhost $port` ; do
        echo -n .
        sleep 1
    done
}

echo_bold() {
    echo -e "\033[1m${1}\033[0m"
}

kill_process_tree() {
    top=$1
    pid=$2

    children=`ps -o pid --no-headers --ppid ${pid}`

    for child in $children
    do
        kill_process_tree 0 $child
    done

    if [ $top -eq 0 ]; then
        kill -9 $pid &> /dev/null
    fi
}

add_local_br() {
    br=$1
    dpid=$2
    $VSCTL add-br $br
    $VSCTL set bridge $br protocols=$OFP
    if [ "$dpid" != "" ] ; then
      $VSCTL set bridge $br other-config:datapath-id=$dpid
    fi
    ip link set up $br
    check_local_br_up $br
}

check_local_br_up() {
    br=$1
    echo waiting for OVS sw/controller $br to come up
    while ! $OFCTL ping $br 64|grep -q "64 bytes from" ; do
      echo -n "."
      sleep 1
    done
}

start_rfvm1() {
    echo_bold "-> Starting the rfvm_wand virtual machine..."
    ROOTFS=$RFVM1/rootfs

    VMLOG=/tmp/rfvm1.log
    rm -f $VMLOG
    lxc-start -n rfvm1 -l DEBUG -o $VMLOG -d
}

reset() {
    echo_bold "-> Stopping and resetting LXC VMs..."
    lxc-stop -n rfvm1 &> /dev/null

    init=$1
    if [ ! $init -eq 1 ]; then
        echo_bold "-> Stopping child processes..."
        kill_process_tree 1 $$
    fi

    sudo $VSCTL del-br $RFBR &> /dev/null
    sudo $VSCTL del-br $RFDP &> /dev/null
    sudo $VSCTL emer-reset &> /dev/null

    rm $RFVM1/rootfs/var/run/network/ifstate &> /dev/null

    echo_bold "-> Deleting old veths..."
    for i in `netstat -i|grep rfvm1|cut -f 1 -d " "` ; do
      ip link set down $i &> /dev/null
      ip link del $i &> /dev/null
    done

    echo_bold "-> Deleting data from previous runs..."
    rm -rf $HOME/db
}
reset 1
trap "reset 0; exit 0" INT

if [ "$ACTION" != "RESET" ]; then
    echo_bold "-> Starting the management network ($RFBR)..."
    add_local_br $RFBR
    ip address add $HOSTVMIP dev $RFBR

    echo_bold "-> Starting RFServer..."
    # parse some config files
    rf_args="$RFSERVERCONF -i $RFSERVERINTERNAL"

    if [ -f $RFSERVERDPIDMODE ]; then
        MULTITABLEDPS=$(tail -n +2 $RFSERVERDPIDMODE | \
            grep multitable | sed s/,multitable// | sed -n -e 'H;${x;s/\n/,/g;s/^,//;p;}')
        SATELLITEDPS=$(tail -n +2 $RFSERVERDPIDMODE | \
            grep satellite | sed s/,satellite// | sed -n -e 'H;${x;s/\n/,/g;s/^,//;p;}')

        if [ ! -z "$MULTITABLEDPS" ]; then
            rf_args="$rf_args -m $MULTITABLEDPS"
        fi

        if [ ! -z "$SATELLITEDPS" ]; then
            rf_args="$rf_args -s $SATELLITEDPS"
        fi
    fi

    nice ./rfserver/rfserver.py $rf_args &

    echo_bold "-> Starting the controller ($ACTION) and RFPRoxy..."
    case "$ACTION" in
    RYU)
        cd ryu-rfproxy
        ryu-manager --use-stderr --ofp-tcp-listen-port=$CONTROLLER_PORT rfproxy.py &
        ;;
    esac
    cd - &> /dev/null
    wait_port_listen $CONTROLLER_PORT
    check_local_br_up tcp:127.0.0.1:$CONTROLLER_PORT

    echo_bold "-> Starting the control plane network ($RFDP VS)..."
    $VSCTL add-br $RFDP
    $VSCTL set bridge $RFDP other-config:datapath-id=$RFDPID
    $VSCTL set bridge $RFDP protocols=$OFP
    $VSCTL set-controller $RFDP tcp:127.0.0.1:$CONTROLLER_PORT
    $OFCTL add-flow $RFDP actions=CONTROLLER:65509
    ip link set up $RFDP
    check_local_br_up $RFDP

    echo_bold "-> Waiting for $RFDP to connect to controller..."
    while ! $VSCTL find Controller target=\"tcp:127.0.0.1:$CONTROLLER_PORT\" is_connected=true | grep -q connected ; do
      echo -n .
      sleep 1
    done

    echo_bold "-> Starting rfvm1..."
    start_rfvm1
    while ! ifconfig -s rfvm1.0 ; do
      echo -n .
      sleep 1
    done

    $VSCTL add-port $RFBR rfvm1.0
    for i in `netstat -i|grep rfvm1|cut -f 1 -d " "` ; do
      if [ "$i" != "rfvm1.0" ] ; then
        $VSCTL add-port $RFDP $i
      fi
    done

    echo_bold "-> Waiting for rfvm1 to come up..."
    while ! ping -W 1 -c 1 $RFVM1IP ; do
      echo -n .
      sleep 1
    done

    echo_bold "You can stop this test by pressing Ctrl+C."
    wait
fi
exit 0
