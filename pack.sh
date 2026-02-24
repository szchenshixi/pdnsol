#!/bin/env bash
# ----------------------------
# User configurations
# ----------------------------
ARCHIVE_NAME="CommonUtils"
COMPRESS_THREADS=$(nproc)

# ----------------------------
# Environment validation
# ----------------------------
# Usage: stringContain <substr> <string>
stringContain() { case $2 in *$1* ) return 0;; *) return 1;; esac ;}

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PROJECT_BASE=$(basename ${SCRIPT_DIR})
PROJECT_DIR=$(dirname ${SCRIPT_DIR})
echo "SCRIPT_DIR:        ${SCRIPT_DIR}"
echo "PROJECT_BASE:      ${PROJECT_BASE}"
echo "PROJECT_DIR:       ${PROJECT_DIR}"
echo "COMPRESS_THREADS:  ${COMPRESS_THREADS}"
echo "ARCHIVE_NAME:      ${ARCHIVE_NAME}.tar.xz"

if ! stringContain "CommonUtils" ${PROJECT_BASE}; then
    echo "Place this script under the project root! Current: ${SCRIPT_DIR}"
    exit -1
fi
echo "Changing directory to ${PROJECT_DIR}"

# ----------------------------
# Compression stamp
# ----------------------------
echo "Establishing the compression stamp (git/date)"
cd ${PROJECT_DIR}/${PROJECT_BASE}
# Latest commit hash
if ! GIT_HEAD_HASH="$(git rev-parse --verify HEAD 2>/dev/null)"; then
  STAMP=$(date +%Y%m%d-%H%M) # e.g., 20260224-2028
else
  STAMP=${GIT_HEAD_HASH:0:13} # The first 13 characters
fi

# ----------------------------
# Latest commit hash
# ----------------------------
echo \
"Compressing ${PROJECT_DIR}/${PROJECT_BASE} to ${ARCHIVE_NAME}-${STAMP}.tar.xz"
cd ${PROJECT_DIR}
tar --exclude-vcs --exclude-vcs-ignores                           \
    --exclude="${PROJECT_BASE}/build"                             \
    --exclude="${PROJECT_BASE}/.cache"                            \
    --exclude="${PROJECT_BASE}/.vscode"                           \
    --use-compress-program="xz -9e --threads=${COMPRESS_THREADS}" \
    -cvaf ${ARCHIVE_NAME}-${STAMP}.tar.xz ${PROJECT_BASE}
if [ $? -eq 0 ] && xz -t ${ARCHIVE_NAME}.tar.xz; then
    echo \
    "Succeed. Compressed ${PROJECT_BASE} to ${ARCHIVE_NAME}-${STAMP}.tar.xz"
else
    echo "Failed"
    exit -1
fi

