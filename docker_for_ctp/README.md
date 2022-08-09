# **Docker for CTP**
## What is the CTP?
CTP(= CUBRID Test Progam)<br/>
The goal is the test by CUBRID.<br/><br/>

### How to use ctp?
ctp.sh [Test scenario] -c [CTP/conf/*.conf]
(ex : ctp.sh sql -c ~/CTP/conf/sql.conf )

If you want to change the 'test scenario path', open the 'CTP/conf/*.conf'.<br/>
and change the 'scenario' variable.<br/><br/>

The detail is followed to refer to this.<br/>
https://github.com/CUBRID/cubrid-testtools <br/><br/>

### How to create the container?
Execute the 'sh make_for_container.sh'.<br/>
It makes the 2 containers.<br/><br/>

The container name is 'docker_for_ctp_01', 'docker_for_ctp_02'.<br/>
And you can connect the container by SSH.<br/><br/>

### Container construct
All user default password is 1234.<br/>
| Container name | ip | User | Possible to scenario | Tools |
| ------------ | ------------ | ------------ | ------------ | ------------ |
| docker_for_ctp_01 | 172.200.1.2 | ctp | Controller & SQL, SQL_BY_CCI, <br/>MEDIUM, JDBC unit test | CTP, cubrid-testcases |
|  |  | ha_repl_01 | HA_REPL(Master), HA_SHELL(Master), SHELL test, CCI test | CTP |
| docker_for_ctp_02 | 172.200.1.3 | ctp | Controller & SQL, SQL_BY_CCI, <br/>MEDIUM, JDBC unit test | CTP, cubrid-testcases |
|  |  | ha_repl_01 | HA_REPL(Slave), HA_SHELL(Slave), SHELL test, CCI test | CTP |
<br/><br/>


### How to install the CUBRID?
https://www.cubrid.org/manual/en/11.2/install.html#installing-and-running-cubrid-on-linux <br/><br/>

### Q&A
1. If can not work 'ctp.sh'
  - please execute 'sh ctp_config/ctp_env_export.sh'
2. Where do I put the test build?
  - ctp_config/test_build/
  - 'ctp_config' dir is shared dir by the parent machine.
3. Created core file during the test
  - You can use the GDB command in the container.
