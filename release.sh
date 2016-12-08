#!/bin/bash

if [ $# = 3 ]
then
	echo $1
else
	echo "release 0.1 rel-alias entos"
	exit 1
fi

path=$(dirname $0)
oldpwd=$(pwd)
cd ${path}
path=$(pwd)

cd ${path}

#############################################################################
# Platform
m=`uname -m`
if uname -o | grep -i linux;
then
	o=linux
fi


rm -rf $3-niuhttpd-bin-$2-${m}-${o}
mkdir $3-niuhttpd-bin-$2-${m}-${o}
mkdir $3-niuhttpd-bin-$2-${m}-${o}/html

cp html/* $3-niuhttpd-bin-$2-${m}-${o}/html/

cp src/niuhttpd $3-niuhttpd-bin-$2-${m}-${o}/niuhttpd
cp src/niutool $3-niuhttpd-bin-$2-${m}-${o}/niutool
cp src/libniuhttp.so $3-niuhttpd-bin-$2-${m}-${o}/libniuhttp.so
cp src/libniuauth.so $3-niuhttpd-bin-$2-${m}-${o}/libniuauth.so
cp src/libniuwebsocket.so $3-niuhttpd-bin-$2-${m}-${o}/libniuwebsocket.so

cp script/install.sh $3-niuhttpd-bin-$2-${m}-${o}/
cp script/uninstall.sh $3-niuhttpd-bin-$2-${m}-${o}/

cp script/niuhttpd.conf $3-niuhttpd-bin-$2-${m}-${o}/niuhttpd.conf
cp script/permit.list $3-niuhttpd-bin-$2-${m}-${o}/
cp script/reject.list $3-niuhttpd-bin-$2-${m}-${o}/
cp script/extension.xml $3-niuhttpd-bin-$2-${m}-${o}/

cp script/niuhttpd.sh $3-niuhttpd-bin-$2-${m}-${o}/

cp ca/ca.crt $3-niuhttpd-bin-$2-${m}-${o}/ca.crt

cp ca/server.p12 $3-niuhttpd-bin-$2-${m}-${o}/server.p12
cp ca/server.crt $3-niuhttpd-bin-$2-${m}-${o}/server.crt
cp ca/server.key $3-niuhttpd-bin-$2-${m}-${o}/server.key

cp ca/client.p12 $3-niuhttpd-bin-$2-${m}-${o}/client.p12
cp ca/client.crt $3-niuhttpd-bin-$2-${m}-${o}/client.crt
cp ca/client.key $3-niuhttpd-bin-$2-${m}-${o}/client.key

chmod a+x $3-niuhttpd-bin-$2-${m}-${o}/*
#ls -al $3-niuhttpd-bin-$2-${m}-${o}
tar zcf $3-niuhttpd-bin-$2-${m}-${o}-$1.tar.gz $3-niuhttpd-bin-$2-${m}-${o}
cd ${oldpwd}
