#!/bin/bash

systemctl restart sshd

if [ ! -d /opt/test_repo ]
then
    mkdir -f /opt/test_repo
fi
mv cubrid-testtools /opt/test_repo
mv cubrid-testcases /opt/test_repo
chmod 777 -R /opt/test_repo

exec "$@"
