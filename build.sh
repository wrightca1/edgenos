#!/bin/bash
# build.sh - Convenience wrapper for Docker-based EdgeNOS builds
#
# Usage:
#   ./build.sh              # Full build
#   ./build.sh kernel       # Kernel only
#   ./build.sh shell        # Interactive shell in build container
#   ./build.sh clean        # Remove output directory
#   ./build.sh rebuild      # Clean + full build

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

IMAGE_NAME="edgenos-builder"
CONTAINER_NAME="edgenos-build"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[0;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}==>${NC} $*"; }
warn()  { echo -e "${YELLOW}==>${NC} $*"; }
error() { echo -e "${RED}==>${NC} $*"; }

# Check Docker is available
check_docker() {
    if ! command -v docker &>/dev/null; then
        error "Docker not found. Install Docker first."
        exit 1
    fi
    if ! docker info &>/dev/null 2>&1; then
        error "Docker daemon not running or permission denied."
        error "Try: sudo systemctl start docker && sudo usermod -aG docker $USER"
        exit 1
    fi
}

# Build the Docker image if needed
build_image() {
    if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
        info "Building Docker image (this downloads ~2GB and takes a while)..."
        docker build -t "$IMAGE_NAME" .
    else
        info "Docker image '$IMAGE_NAME' exists"
        # Check if Dockerfile is newer than image
        local img_date
        img_date=$(docker image inspect "$IMAGE_NAME" --format '{{.Created}}' 2>/dev/null)
        local df_date
        df_date=$(stat -c %Y Dockerfile 2>/dev/null || stat -f %m Dockerfile)

        # Rebuild hint
        warn "To rebuild: docker build --no-cache -t $IMAGE_NAME ."
    fi
}

# Run build
run_build() {
    local target="${1:-all}"

    mkdir -p output

    info "Running build target: $target"

    if [ "$target" = "shell" ] || [ "$target" = "bash" ]; then
        docker run --rm -it \
            -v "$SCRIPT_DIR/output:/build/output" \
            "$IMAGE_NAME" bash
    else
        docker run --rm \
            -v "$SCRIPT_DIR/output:/build/output" \
            "$IMAGE_NAME" "$target"
    fi
}

# ── Main ──────────────────────────────────────────────────────
CMD="${1:-all}"

case "$CMD" in
    clean)
        info "Cleaning output directory..."
        rm -rf output/
        info "Done."
        ;;
    rebuild)
        info "Rebuilding from scratch..."
        rm -rf output/
        docker build --no-cache -t "$IMAGE_NAME" .
        run_build all
        ;;
    docker-build)
        info "Building Docker image..."
        check_docker
        docker build -t "$IMAGE_NAME" .
        ;;
    help|-h|--help)
        echo "EdgeNOS Build System"
        echo ""
        echo "Usage: $0 [target]"
        echo ""
        echo "Targets:"
        echo "  all         Full build (default)"
        echo "  kernel      Build Linux kernel + DTB"
        echo "  modules     Build platform kernel modules"
        echo "  sdk         Build OpenMDK libraries"
        echo "  switchd     Build switch daemon"
        echo "  rootfs      Build root filesystem"
        echo "  image       Build ONIE installer"
        echo "  shell       Interactive shell in build container"
        echo "  clean       Remove output directory"
        echo "  rebuild     Clean + rebuild everything"
        echo "  docker-build  Build Docker image only"
        echo ""
        echo "Output: output/images/edgenos-as5610-52x.bin"
        ;;
    *)
        check_docker
        build_image
        run_build "$CMD"
        ;;
esac
