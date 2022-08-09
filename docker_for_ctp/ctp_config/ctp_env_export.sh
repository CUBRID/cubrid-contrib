# !/bin/bash

echo "export JAVA_HOME=/usr/lib/jvm/java-1.6.0-openjdk-1.6.0.41.x86_64" >> ${HOME}/.bash_profile
echo "export CTP_HOME=/home/ctp/CTP" >> ${HOME}/.bash_profile
echo ". ${HOME}/.cubrid.sh" >> ${HOME}/.bash_profile
sed -i 's@:$HOME/bin@:$HOME/bin:$HOME/CTP/bin:$HOME/CTP/common/script@' ${HOME}/.bash_profile
. ${HOME}/.bash_profile
