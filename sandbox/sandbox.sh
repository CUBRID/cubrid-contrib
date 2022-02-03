#!/bin/sh

# img cmd
DOCKERFILE="Dockerfile"
IMGNAME="cubrid-sandbox-img"

# pod cmd
CONTAINERNAME="cubrid-sandbox"
SOURCEPATH=""
POD_RUN_CPU_RANGE=""

# -- docker images --
cmd_img_new() {
    while [ $# -gt 0 ]
    do
        case $1 in
            -f|--file)
                DOCKERFILE=$2; shift ;;
            *)
                echo "img.new :: unrecognized $1 option!"
                exit 1
                ;;
        esac
        shift
    done

    docker build \
        -f $DOCKERFILE \
        --build-arg WHOAMI=$(whoami) \
        -t $IMGNAME \
        .
}

cmd_img_rm() {
    docker rmi $@
}

cmd_img() {
    case $1 in
        new)
            shift
            cmd_img_new "$@"
            ;;
        rm)
            shift
            cmd_img_rm "$@"
            ;;
        ls)
            shift
            docker images | grep cubrid
            ;;
        *|help)
            echo "  img new"
            echo "  img rm"
            echo "  img ls"
            exit 1
            ;;
    esac
}
# -- docker images --

# -- docker containers --
cmd_pod_run() {
    while [ $# -gt 0 ]
    do
        case $1 in
            -s|--src)
                SOURCEPATH=$2; shift ;;
            --cpu-range)
                POD_RUN_CPU_RANGE="--cpuset-cpus ${2}"; shift ;;
            *)
                echo "pod.run :: unrecognized $1 option!"
                exit 1
                ;;
        esac
        shift
    done

    docker run \
        --rm \
        -it \
        -v $SOURCEPATH:$SOURCEPATH \
        -w $SOURCEPATH \
        -h ${CONTAINERNAME}-$(whoami) \
        -u $(whoami) \
        $POD_RUN_CPU_RANGE \
        --name $CONTAINERNAME \
        ${IMGNAME}:latest
}

cmd_pod_rm() {
    while [ $# -gt 0 ]
    do
        docker stop $1 >/dev/null
        docker rm $1
        shift
    done
}

cmd_pod() {
    case $1 in
        run)
            shift
            cmd_pod_run "$@"
            ;;
        rm)
            shift
            cmd_pod_rm "$@"
            ;;
        ls)
            shift
            docker ps -a | grep cubrid
            ;;
        *|help)
            echo "  pod run"
            echo "  pod rm"
            echo "  pod ls"
            exit 1
            ;;
    esac

}
# -- docker containers --

print_usage() {
    echo "usage: sandbox.sh <cmd>"
    echo "  cmd"
    echo "    - img help"
    echo "    - pod help"
}

[ -z $(which docker) ] && {
    echo "Docker is not installed!";
    exit 1;
}

case $1 in
    img)
        shift
        cmd_img "$@"
        ;;
    pod)
        shift
        cmd_pod "$@"
        ;;
    help|*)
        print_usage
        ;;
esac

exit 0
