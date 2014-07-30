#!/bin/bash

HOSTVMIP=172.30.20.253/28
DPPORTS=10
WAND_DPID=0x99
REANNZ_DPID=0x9a
RFVM1_ID=12A0A0A0A0A0

RF_HOME=/srv/openvapour/RouteFlow
CONTROLLER_PORT=6633
LXCDIR=/var/lib/lxc
RFVM1=$LXCDIR/rfvm1
RFBR=br-rf
RFDP=br-dp
RF_DPID=7266767372667673
RFSERVERCONF=/etc/routeflow/rfserverconfig.csv
RFSERVERINTERNAL=/etc/routeflow/rfserverinternal.csv
OVSSOCK=/var/run/openvswitch/db.sock
VSCTL="ovs-vsctl --db=unix:$OVSSOCK"
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

start_rfvm1() {
    echo_bold "-> Starting the rfvm_wand virtual machine..."
    ROOTFS=$RFVM1/rootfs

    VMLOG=/tmp/rfvm1.log
    rm -f $VMLOG
    lxc-start -n rfvm1 -l DEBUG -o $VMLOG -d
}

reset() {
    init=$1;
    if [ ! $init -eq 1 ]; then
        echo_bold "-> Stopping child processes...";
        kill_process_tree 1 $$
    fi

    echo_bold "-> Stopping and resetting LXC VMs...";
    lxc-stop -n rfvm1 &> /dev/null;

    echo_bold "-> Stopping and resetting virtual network...";
    sudo $VSCTL del-br $RFBR &> /dev/null;
    sudo $VSCTL del-br $RFDP &> /dev/null;
    sudo $VSCTL emer-reset &> /dev/null;

    echo_bold "-> Deleting ifstate data...";
    rm $RFVM1/rootfs/var/run/network/ifstate;

    echo_bold "-> Deleting data from previous runs...";
    rm -rf $HOME/db;
}
reset 1
trap "reset 0; exit 0" INT

if [ "$ACTION" != "RESET" ]; then
    echo_bold "-> Starting the management network ($RFBR)..."
    $VSCTL add-br $RFBR
    ip link set $RFBR up
    ip address add $HOSTVMIP dev $RFBR
    $VSCTL add-port $RFBR rfvm1.0
    start_rfvm1
    echo_bold "-> Configuring the virtual machines..."

    echo_bold "-> Starting the controller ($ACTION) and RFPRoxy..."
    case "$ACTION" in
    RYU)
        cd ryu-rfproxy
        /srv/openvapour/pythonenv/bin/ryu-manager rfproxy.py &
        ;;
    esac
    cd - &> /dev/null
    wait_port_listen $CONTROLLER_PORT

    echo_bold "-> Starting RFServer..."
    ./rfserver/rfserver.py $RFSERVERCONF -i $RFSERVERINTERNAL &

    echo_bold "-> Starting the control plane network ($RFDP VS)..."
    $VSCTL add-br $RFDP
    for i in `seq 1 $DPPORTS` ; do
      $VSCTL add-port $RFDP rfvm1.$i
    done
    $VSCTL set bridge $RFDP other-config:datapath-id=$RF_DPID
    $VSCTL set bridge $RFDP protocols=OpenFlow13
    $VSCTL set-controller $RFDP tcp:127.0.0.1:$CONTROLLER_PORT
    ip link set $RFDP up

    echo_bold "You can stop this test by pressing Ctrl+C."
    wait
fi
exit 0
