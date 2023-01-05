#!/bin/bash

dir=`pwd`
VERSION=1.0
PREFIX=cubrid
IMAGE_NAME=${PREFIX}_ctp_container
CONTAINER_NAME=docker_for_ctp

ip_range="172.200.1.0"
ip_list=("172.200.1.2" "172.200.1.3")

# Set tests configs for related by HA
sed -i "s/MASTER_SERVER_IP=.*/MASTER_SERVER_IP=${ip_list[0]}/" test_repo/ctp_config/HA.properties
sed -i "s/SLAVE_SERVER_IP=.*/SLAVE_SERVER_IP=${ip_list[1]}/" test_repo/ctp_config/HA.properties
sed -i "s/env.instance1.ssh.host=.*/env.instance1.ssh.host=${ip_list[0]}/" test_repo/ctp_config/ha_shell.conf
sed -i "s/env.instance1.ssh.relatedhosts=.*/env.instance1.ssh.relatedhosts=${ip_list[1]}/" test_repo/ctp_config/ha_shell.conf
sed -i "s/env.ha1.master.ssh.host=.*/env.ha1.master.ssh.host=${ip_list[0]}/g" ha_repl.conf
sed -i "s/env.ha1.slave1.ssh.host=.*/env.ha1.slave1.ssh.host=${ip_list[1]}/g" ha_repl.conf

# Remove the previous container & image
echo ""
echo "(1/3) Initialize"
echo ""
IMAGE_ID=`docker images | grep "${IMAGE_NAME}" | awk '{print $3}'`
docker rm -f ${CONTAINER_NAME}_01
docker rm -f ${CONTAINER_NAME}_02
sleep 5;
docker rmi ${IMAGE_ID}
sleep 5;
docker network rm br1
sleep 5;

echo ""
echo "(2/3) Build docker image"
echo ""
docker build -t ${IMAGE_NAME}:${VERSION} .

sleep 5;

echo ""
echo "(3/3) Create docker network & docker container"
echo ""
docker network create --driver bridge br1 --ip-range=${ip_range}/24 --subnet=${ip_range}/24

# The Test OS is Centos 7
# ./test_repo is shared dir with all containers and parents machine
docker run --privileged -dit --cap-add=SYS_PTRACE --net br1 --ip ${ip_list[0]} -v ${dir}/test_repo:/opt/test_repo --name ${CONTAINER_NAME}_01 --hostname ${CONTAINER_NAME}_01 ${IMAGE_NAME}:${VERSION} /bin/bash
docker run --privileged -dit --cap-add=SYS_PTRACE --net br1 --ip ${ip_list[1]} -v ${dir}/test_repo:/opt/test_repo --name ${CONTAINER_NAME}_02 --hostname ${CONTAINER_NAME}_02 ${IMAGE_NAME}:${VERSION} /bin/bash
sleep 3;


echo ""
echo "Done"
echo ""
