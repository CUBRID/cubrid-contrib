# **Docker for CTP**

## Prerequisites
You need to install Docker Engine on your host system.   
Please refer to the link, [Install Docker Engine](https://docs.docker.com/engine/install) to install.

As a known issue, the docker network is not well configured when you install Docker on Windows.
Installing Ubuntu on WSL2 on Windows 10 is recommended.
Please refer to [the details](https://github.com/CUBRID/cubrid-contrib/pull/3#issuecomment-1223908324)

## Quick Start
### How to startup CTP containers?
```shell
$ git clone https://github.com/CUBRID/cubrid-contrib.git
$ cd cubrid-contrib/docker_for_ctp
$ sh make_for_container.sh
```

### How to start an existing CTP container?
```shell
# If the CTP container already exists.
$ docker start docker_for_ctp_01(or docker_for_ctp_02)
```

### How to start testing with CTP?
```shell
$ ssh ctp@172.200.1.2(or 172.200.1.3)
$ ctp.sh [test scenario] -c [ctp config file]

# e.g.) ctp.sh sql -c ${CTP_HOME}/conf/sql.conf -- for sql test
```
The detail is followed URL
https://github.com/CUBRID/cubrid-testtools

## What is the CTP?
CTP(= CUBRID Test Progam)  
The goal of this tool is to execute test cases already written in [cubrid-testcases](https://github.com/CUBRID/cubrid-testcases) or test cases written by the user and report the results.
For more details, please refer to [the link](https://github.com/CUBRID/cubrid-testtools/tree/develop/CTP)

### How to use ctp?
ctp.sh [Test scenario] -c [${CTP_HOME}/conf/*.conf]
(ex : ctp.sh sql -c ~/${CTP_HOME}/conf/sql.conf )

If you want to change the 'test scenario path', open the '${CTP_HOME}/conf/*.conf'.  
and change the 'scenario' variable.    

The detail is followed URL  
https://github.com/CUBRID/cubrid-testtools     

### How to create the container?
Execute the 'sh make_for_container.sh'.  
Two containers are created with the names 'docker_for_ctp_01' and 'docker_for_ctp_02' respectively. 
And you can connect the container by SSH.    

### Container construct
The default user name & password
id : ctp, ha_repl_01
pw : 1234  
| Container name | ip | User | Possible to scenario | Tools |
| ------------ | ------------ | ------------ | ------------ | ------------ |
| docker_for_ctp_01 | 172.200.1.2 | ctp | HA test Controller & SQL, SQL_BY_CCI, MEDIUM, Isolation | CTP, cubrid-testcases |
|  |  | ha_repl_01 | HA_REPL(Master), HA_SHELL(Master), SHELL test, CCI test, JDBC unit test | CTP |
| docker_for_ctp_02 | 172.200.1.3 | ctp | HA test Controller & SQL, SQL_BY_CCI, MEDIUM, Isolation | CTP, cubrid-testcases |
|  |  | ha_repl_01 | HA_REPL(Slave), HA_SHELL(Slave), SHELL test, CCI test, JDBC unit test | CTP |

    


### How to install the CUBRID?
https://www.cubrid.org/manual/en/11.2/install.html#installing-and-running-cubrid-on-linux     

### Q&A
1. If can not work 'ctp.sh'
   - please execute 'sh test_repo/ctp_config/ctp_env_export.sh'
2. Where do I put the test build?
   - ./test_repo
   - The 'test_repo' dir is shared dir with the parent machine.
3. How to debug a core file that was created during the test?
   - You can use the GDB command in the container.
4. Can not CCI driver test
   - please try to this
      ```
      cp $CUBRID/cci/include/* $CUBRID/include/
      cp $CUBRID/cci/lib/libcascci.so* lib/
      ```
5. If rebooted the machine OR stop the container.
   - please try to this
      ```
      docker start docker_for_ctp_01
      ssh ctp@172.200.1.2
      ```
6. Where are the test cases & CTP dir?
   - They are in '/home/(ctp or ha_repl_01)/test_repo/*'.
   - This dir is shared with all ctp containers & parent machine.
