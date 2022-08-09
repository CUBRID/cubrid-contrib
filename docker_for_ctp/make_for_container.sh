# !/bin/bash

dir=`pwd`
VERSION=0.8
IMAGE_NAME=ctp_container
CONTAINER_NAME=docker_for_ctp

ip_range="172.200.1.0"
ip_list=("172.200.1.2" "172.200.1.3")

# Remove the previous container & image
IMAGE_ID=`docker images | grep "${IMAGE_NAME}" | awk '{print $3}'`
docker rm ${CONTAINER_NAME}_01
docker rm ${CONTAINER_NAME}_02
sleep 5;
docker rmi ${IMAGE_ID}
sleep 5;

docker build -t ${IMAGE_NAME}:${VERSION} .

sleep 10;

docker network create --driver bridge br1 --ip-range=${ip_range}/24 --subnet=${ip_range}/24

#centos 7
docker run --privileged -dit --cap-add=SYS_PTRACE --net br1 --ip ${ip_list[0]} -v ${dir}/ctp_config:/home/ctp/ctp_config --name ${CONTAINER_NAME}_01 --hostname ${CONTAINER_NAME}_01 ${IMAGE_NAME}:${VERSION} /sbin/init
docker run --privileged -dit --cap-add=SYS_PTRACE --net br1 --ip ${ip_list[1]} -v ${dir}/ctp_config:/home/ctp/ctp_config --name ${CONTAINER_NAME}_02 --hostname ${CONTAINER_NAME}_02 ${IMAGE_NAME}:${VERSION} /sbin/init

# set for shell_HA test
sed -i 's/MASTER_SERVER_IP =.*/MASTER_SERVER_IP = ${ip_list[0]}/' ctp_config/HA.properties
sed -i 's/SLAVE_SERVER_IP =.*/SLAVE_SERVER_IP = ${ip_list[1]}/' ctp_config/HA.properties

sleep 3;

docker exec ${CONTAINER_NAME}_01 systemctl restart sshd
docker exec ${CONTAINER_NAME}_02 systemctl restart sshd

sleep 3;
docker exec ${CONTAINER_NAME}_01 chown -R ctp:ctp /home/ctp/CTP/conf/

sleep 3;
docker exec ${CONTAINER_NAME}_01 chown -R ctp:ctp /home/ctp/ctp_config

sleep 3;
docker exec ${CONTAINER_NAME}_01 chown -R ha_repl_01:ha_repl_01 /home/ha_repl_01/ctp_config

sleep 3;
docker exec ${CONTAINER_NAME}_02 chown -R ctp:ctp /home/ctp/CTP/conf/

sleep 3;
docker exec ${CONTAINER_NAME}_02 chown -R ctp:ctp /home/ctp/ctp_config

sleep 3;
docker exec ${CONTAINER_NAME}_02 chown -R ha_repl_01:ha_repl_01 /home/ha_repl_01/ctp_config

