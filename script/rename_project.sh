#!/bin/bash
stringContain() { case $2 in *$1* ) return 0;; *) return 1;; esac ;}

# Define the replacement pairs
declare -A REPLACEMENTS
REPLACEMENTS=(
    ["CommonUtils"]="PdnSol"
    ["COMMON_UTILS"]="PDN_SOL"
)
EXTENSIONS=(
    # *.cpp
    # *.hpp
    # *.cmake
    # *.sh
    config.h.in
    # CMakeLists.txt
)

SCRIPT_DIR="$(dirname "$(readlink -f "$0")")"
PROJECT_DIR="$(readlink -f $(basename ${SCRIPT_DIR})/..)"

echo "SCRIPT_DIR: ${SCRIPT_DIR}"
echo "PROJECT_DIR: ${PROJECT_DIR}"

# Iterate through the files
for ext in ${EXTENSIONS[@]}; do
    echo "Extension: $ext"
    while IFS= read -r -d '' file; do
        # Check if the file exists
        if [[ ! -f "$file" ]]; then
            echo "File $file does not exist."
            continue
        fi

        relative_path=$(realpath --relative-to=${PROJECT_DIR} "$file")
        echo "Processing file: $relative_path"
        # Iterate through the replacement pairs
        for old in "${!REPLACEMENTS[@]}"; do
            new="${REPLACEMENTS[$old]}"
            # Replace the keywords using "sed"
            sed -i "s/$old/$new/g" "${file}"
        done
    done < <(find ${PROJECT_DIR} -type f -name "${ext}" -print0)
done

echo "Replacement complete."
