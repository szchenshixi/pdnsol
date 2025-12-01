#!/bin/env bash
# Configurations
ARCHIVE_NAME="pdnsol"
COMPRESS_THREADS=$(nproc)

# Usage: stringContain <substr> <string>
stringContain() { case $2 in *$1* ) return 0;; *) return 1;; esac ;}

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PROJECT_BASE=$(basename ${SCRIPT_DIR})
PROJECT_DIR=$(dirname ${SCRIPT_DIR})
echo "SCRIPT_DIR: ${SCRIPT_DIR}"
echo "PROJECT_BASE: ${PROJECT_BASE}"
echo "PROJECT_DIR: ${PROJECT_DIR}"
echo "COMPRESS_THREADS: ${COMPRESS_THREADS}"

if ! stringContain "pdnsol" ${PROJECT_BASE}; then
    echo "Place this script under the project root! Current: ${SCRIPT_DIR}"
    exit -1
fi
echo "Changing directory to ${PROJECT_DIR}"
echo "Compressing ${PROJECT_DIR}/${PROJECT_BASE} to ${ARCHIVE_NAME}.tar.xz"
cd ${PROJECT_DIR}
tar --exclude-vcs --exclude-vcs-ignores                           \
    --exclude="${PROJECT_BASE}/build"                             \
    --exclude="${PROJECT_BASE}/test/data"                         \
    --exclude="${PROJECT_BASE}/app.log"                           \
    --exclude="${PROJECT_BASE}/.cache"                            \
    --exclude="${PROJECT_BASE}/.vscode"                           \
    --use-compress-program="xz -9e --threads=${COMPRESS_THREADS}" \
    -cvaf ${PROJECT_BASE}.tar.xz ${PROJECT_BASE}
if [ $? -eq 0 ] && xz -t ${PROJECT_BASE}.tar.xz; then
    echo "Succeed. Compressed ${PROJECT_BASE} to ${PROJECT_BASE}.tar.xz"
else
    echo "Failed"
    exit -2
fi

