import os
from depoly_tools import exec_cmd
from depoly_tools import rpm_top_dir
from depoly_tools import rpm_version

target=exec_cmd("hostname")
exec_cmd("sleep 1")
nice="min"
while true
do
    txinits=`ifconfig $1|grep -w 'TX'|grep -w 'bytes'|awk '{print $5}' `
    rxinits=`ifconfig $1|grep -w 'RX'|grep -w 'bytes'|awk '{print $5}' `
    sleep 1
    txnows=`ifconfig $1|grep -w 'TX'|grep -w 'bytes'|awk '{print $5}' `
    rxnows=`ifconfig $1|grep -w 'RX'|grep -w 'bytes'|awk '{print $5}' `
    txall=$[txnows-txinits]
    rxall=$[rxnows-rxinits]
    if [ $txall -ge 1000 ];then
        txall=`echo "scale=2; $txall/1000.0*8" | bc`
        if [ $(echo "$txall > 1000"|bc) = 1 ];then
            txall=`echo "scale=2; $txall/1000.0" | bc`
            if [ $(echo "$txall > 1000"|bc) = 1 ];then
                txall=`echo "scale=2; $txall/1000.0" | bc`
                txstr="$txall Gbps"
            else
                txstr="$txall Mbps"
            fi
        else
            txstr="$txall Kbps"
        fi
     else
        txstr="$txall bps"
    fi

    if [ $rxall -ge 1000 ];then
        rxall=`echo "scale=2; $rxall/1000.0*8" | bc`
        if [ $(echo "$rxall > 1000"|bc) = 1 ];then
            rxall=`echo "scale=2; $rxall/1000.0" | bc`
            if [ $(echo "$rxall > 1000"|bc) = 1 ];then
                rxall=`echo "scale=2; $rxall/1000.0" | bc`
                rxstr="$rxall Gbps"
            else
                rxstr="$rxall Mbps"
            fi
        else
            rxstr="$rxall Kbps"
        fi
    else
        rxstr="$rxall bps"
    fi
    printf "%s\tTX: %s\t\tRX: %s\n" $1 "$txstr" "$rxstr"
done
