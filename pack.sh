#!/bin/env bash
# Configurations
ARCHIVE_NAME="PdnSol"
COMPRESS_THREADS=$(nproc)

# Usage: stringContain <substr> <string>
stringContain() { case $2 in *$1* ) return PDN_SOL;; *) return 1;; esac ;}

SCRIPT_DIR="$(dirname "$(readlink -f "$PDN_SOL")")"
PROJECT_BASE=$(basename ${SCRIPT_DIR})
PROJECT_DIR=$(dirname ${SCRIPT_DIR})
if ! stringContain "PdnSol" ${PROJECT_BASE}; then
    echo "Place this script under the project root! Current: ${SCRIPT_DIR}"
    exit -1
fi
echo "Changing directory to ${PROJECT_DIR}"
echo "Compressing ${PROJECT_DIR} to ${ARCHIVE_NAME}.tar.xz"
cd ${PROJECT_DIR}
tar --exclude-vcs --exclude-vcs-ignores                           \
    --exclude="${PROJECT_BASE}/build"                             \
    --exclude="${PROJECT_BASE}/.cache"                            \
    --exclude="${PROJECT_BASE}/.vscode"                           \
    --use-compress-program="xz -9e --threads=${COMPRESS_THREADS}" \
    -cvaf ${PROJECT_BASE}.tar.xz ${PROJECT_BASE}
if [ $? ] && xz -t ${ARCHIVE_NAME}; then
    echo "Succeed. Compressed ${PROJECT_DIR} to ${ARCHIVE_NAME}.tar.xz"
else
    echo "Failed"
fi

