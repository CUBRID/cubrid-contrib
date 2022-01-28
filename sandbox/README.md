# Sandbox
Sandbox is a virtual platform that provides a space with preinstalled development tools and an interactive shell, `bash` for building [CUBRID](https://github.com/CUBRID/cubrid) engine. All dependencies and tools are automatically installed while pulling a container image. Eventually the sandbox would take your time less than 30 minutes to get a completely isolated shell of the space from host. The platform is based on Docker container technology.

# Supported Platforms
- CentOS 7

# Prerequisites
You need to install Docker Engine on your host system. Please refer to the link, [Install Docker Engine](https://docs.docker.com/engine/install/) to install.

# Quick Start
You can get a sandbox space as easy as pie by following the instructions.
```sh
$ git clone https://github.com/CUBRID/cubrid-contrib.git
$ cd cubrid-contrib/sandbox
$ ./sandbox.sh img new -f Dockerfiles/Dockerfile_centos
$ ./sandbox.sh pod run -s <cubrid source path>
cubrid-sandbox# ./build.sh ...
```

# Best Practice
An isolated space in a container and a host system are sharing the same directory with `-s <cubrid source path>` option at runtime, hence all changes happened on the host affect the container as well. This saying if some code has been modified on it, the container can see the changes.

We suggest the following model as a best practice.
```sh
+-----------+     +-------+
| Container | --> | Shell |
+-----------+     +-------+
    ^                |
    |         +------+  'build' code
    |         |
+--------------------+                     (Isolated)
| Directory (Shared) | ------------------------------
+--------------------+                         (Host)
    |         |
    |         +------+  'edit' code
    v                |
+-----------+     +-------+
| Host Sys  | --> |  IDE  |
+-----------+     +-------+
```

# Sandbox Launcher Arguments
### 1. img
```sh
$ sandbox.sh img <cmd1> [args, ...] [options, ...]
```
|  cmd1  |  args  |  options  |  description  |
|-------:|:------:|:----------|:--------------|
| new    |        |           | create a new image |
|        |        | -f `<file>` | `<file>` must be a Dockerfile |
|        |        |           |               |
| rm     |        |           | remove images |
|        | `<args>` |           | `<args>` must be ID of images |
|        |        |           |               |
| ls     |        |           | list of Cubrid images |

### 2. pod
```sh
$ sandbox.sh pod <cmd1> [args, ...] [options, ...]
```
|  cmd1  |  args  |  options  |  description  |
|-------:|:-------|:----------|:--------------|
| run    |        |           | run a container |
|        |        | -s `<path>` | `<path>` must be a path to cubrid source (must option) |
|        |        | --cpu-range `<nums>` | `<nums>` must be CPUs. (`0`, `0-3` or so forth) |
|        |        |           |               |
| rm     |        |         | remove containers |
|        | `<args>` |         | `<args>` must be ID of containers |
|        |        |           |               |
| ls     |        |           | list of Cubrid containers |
