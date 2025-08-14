#!/bin/bash

# Test runner script for conmon BATS tests

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$SCRIPT_DIR")"

# Default values
CONMON_BINARY="${CONMON_BINARY:-$PROJECT_ROOT/bin/conmon}"
RUNTIME_BINARY="${RUNTIME_BINARY:-/usr/bin/runc}"
BATS_OPTIONS="${BATS_OPTIONS:-}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

usage() {
    cat << EOF
Usage: $0 [OPTIONS] [TEST_FILES...]

Run conmon BATS tests.

OPTIONS:
    -h, --help              Show this help message
    -c, --conmon BINARY     Path to conmon binary (default: $CONMON_BINARY)
    -r, --runtime BINARY    Path to runtime binary (default: $RUNTIME_BINARY)
    -v, --verbose           Verbose output
    -t, --tap               Output in TAP format
    -j, --jobs N            Run tests in parallel with N jobs
    --filter PATTERN        Run only tests matching PATTERN

EXAMPLES:
    $0                      Run all tests
    $0 01-basic.bats        Run only basic tests
    $0 --verbose            Run all tests with verbose output
    $0 --filter "version"   Run only tests with 'version' in the name

ENVIRONMENT VARIABLES:
    CONMON_BINARY          Path to conmon binary
    RUNTIME_BINARY         Path to runtime binary
    BATS_OPTIONS           Additional options to pass to bats
EOF
}

log_info() {
    echo -e "${GREEN}[INFO]${NC} $*"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $*" >&2
}

check_dependencies() {
    local missing_deps=()

    if ! command -v bats >/dev/null 2>&1; then
        missing_deps+=("bats")
    fi

    if [[ ! -x "$CONMON_BINARY" ]]; then
        missing_deps+=("conmon binary at $CONMON_BINARY")
    fi

    if [[ ! -x "$RUNTIME_BINARY" ]]; then
        missing_deps+=("runtime binary at $RUNTIME_BINARY")
    fi

    if [[ ${#missing_deps[@]} -gt 0 ]]; then
        log_error "Missing dependencies:"
        printf '  - %s\n' "${missing_deps[@]}"
        return 1
    fi
}

main() {
    local verbose=false
    local tap=false
    local jobs=""
    local filter=""
    local test_files=()

    # Parse command line arguments
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                usage
                exit 0
                ;;
            -c|--conmon)
                CONMON_BINARY="$2"
                shift 2
                ;;
            -r|--runtime)
                RUNTIME_BINARY="$2"
                shift 2
                ;;
            -v|--verbose)
                verbose=true
                shift
                ;;
            -t|--tap)
                tap=true
                shift
                ;;
            -j|--jobs)
                jobs="$2"
                shift 2
                ;;
            --filter)
                filter="$2"
                shift 2
                ;;
            *.bats)
                test_files+=("$1")
                shift
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done

    # Set up BATS options
    local bats_args=()

    if [[ "$verbose" == true ]]; then
        bats_args+=("--verbose-run")
    fi

    if [[ "$tap" == true ]]; then
        bats_args+=("--tap")
    fi

    if [[ -n "$jobs" ]]; then
        bats_args+=("--jobs" "$jobs")
    fi

    if [[ -n "$filter" ]]; then
        bats_args+=("--filter" "$filter")
    fi

    # Add any additional BATS options from environment
    if [[ -n "$BATS_OPTIONS" ]]; then
        read -ra additional_opts <<< "$BATS_OPTIONS"
        bats_args+=("${additional_opts[@]}")
    fi

    # Check dependencies
    log_info "Checking dependencies..."
    if ! check_dependencies; then
        exit 1
    fi

    # Determine test files to run
    if [[ ${#test_files[@]} -eq 0 ]]; then
        # Run all .bats files in test directory
        mapfile -t test_files < <(find "$SCRIPT_DIR" -name "*.bats" | sort)
    else
        # Convert relative paths to absolute paths
        local resolved_files=()
        for file in "${test_files[@]}"; do
            if [[ "$file" =~ ^/ ]]; then
                resolved_files+=("$file")
            elif [[ "$file" =~ test/ ]]; then
                # Already prefixed with test/, use from project root
                resolved_files+=("$PROJECT_ROOT/$file")
            else
                # Add test/ prefix
                resolved_files+=("$SCRIPT_DIR/$file")
            fi
        done
        test_files=("${resolved_files[@]}")
    fi

    # Verify test files exist
    for file in "${test_files[@]}"; do
        if [[ ! -f "$file" ]]; then
            log_error "Test file not found: $file"
            exit 1
        fi
    done

    # Export environment variables for tests
    export CONMON_BINARY
    export RUNTIME_BINARY

    log_info "Running tests with:"
    log_info "  conmon binary: $CONMON_BINARY"
    log_info "  runtime binary: $RUNTIME_BINARY"
    log_info "  test files: ${test_files[*]}"

    # Run the tests
    log_info "Starting test execution..."
    if bats "${bats_args[@]}" "${test_files[@]}"; then
        log_info "All tests passed!"
        exit 0
    else
        log_error "Some tests failed!"
        exit 1
    fi
}

# Only run main if script is executed directly
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    main "$@"
fi