#!/bin/bash

systemctl restart sshd

exec "$@"
