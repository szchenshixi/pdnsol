# Get the current date and time
string(TIMESTAMP SMART_SPLIT_BUILD_DATE "%Y-%m-%d %H:%M:%S")

# Get the current Git commit has
execute_process(
    COMMAND git rev-parse HEAD
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
    OUTPUT_VARIABLE SMART_SPLIT_GIT_COMMIT_HASH
    ERROR_QUIET
    OUTPUT_STRIP_TRAILING_WHITESPACE
)

# Specify the template configuration file
set(CONFIG_HEADER_TEMPLATE "${PROJECT_SOURCE_DIR}/config.h.in")
set(CONFIG_HEADER_OUTPUT "${PROJECT_BINARY_DIR}/config.h")

# Configure the header file
configure_file(
    "${CONFIG_HEADER_TEMPLATE}"
    "${CONFIG_HEADER_OUTPUT}"
    @ONLY # @VAR@ format only, ignoring ${VAR} syntax
)
