# DAM Build System
# Single-command builds using CMake presets
#
# Usage:
#   make          - Build default (Release, Ollama support)
#   make full     - Build with llama.cpp support
#   make minimal  - Build without LLM features
#   make dev      - Build debug with sanitizers
#   make test     - Run tests
#   make install  - Install to ~/.local (removes stale binaries)
#   make purge    - Remove stale binaries from /usr/local/bin, /usr/bin
#   make clean    - Remove build directories

.PHONY: all build minimal full dev test install clean purge help

# Default target
all: build

# ============================================================================
# Build Targets
# ============================================================================

# Default build (Release, Ollama support)
build:
	@echo "Building DAM (default preset)..."
	@cmake --preset default
	@cmake --build build/default --parallel

# Minimal build (no LLM features)
minimal:
	@echo "Building DAM (minimal preset)..."
	@cmake --preset minimal
	@cmake --build build/minimal --parallel

# Full build (includes llama.cpp)
full:
	@echo "Building DAM (full preset with llama.cpp)..."
	@cmake --preset full
	@cmake --build build/full --parallel

# Development build (debug + sanitizers)
dev:
	@echo "Building DAM (development preset)..."
	@cmake --preset dev
	@cmake --build build/dev --parallel

# ============================================================================
# Test Target
# ============================================================================

test: build
	@echo "Running tests..."
	@ctest --preset default

test-dev: dev
	@echo "Running tests (verbose)..."
	@ctest --preset dev

# ============================================================================
# Install Target
# ============================================================================

install: build purge
	@echo "Installing to ~/.local..."
	@mkdir -p ~/.local/bin
	@cmake --install build/default --prefix ~/.local
	@echo ""
	@echo "Installed! Make sure ~/.local/bin is in your PATH."
	@echo "Add this to your shell config if needed:"
	@echo "  export PATH=\"\$$HOME/.local/bin:\$$PATH\""

# ============================================================================
# Purge Stale Binaries
# ============================================================================

purge:
	@echo "Removing stale dam binaries..."
	@# Remove from common system locations (may require sudo)
	@if [ -f /usr/local/bin/dam ]; then \
		echo "  Found stale binary at /usr/local/bin/dam"; \
		sudo rm -f /usr/local/bin/dam 2>/dev/null || \
		echo "  Warning: Could not remove /usr/local/bin/dam (run: sudo rm /usr/local/bin/dam)"; \
	fi
	@if [ -f /usr/bin/dam ]; then \
		echo "  Found stale binary at /usr/bin/dam"; \
		sudo rm -f /usr/bin/dam 2>/dev/null || \
		echo "  Warning: Could not remove /usr/bin/dam (run: sudo rm /usr/bin/dam)"; \
	fi
	@echo "Done."

# ============================================================================
# Clean Target
# ============================================================================

clean:
	@echo "Cleaning build directories..."
	@rm -rf build/
	@echo "Done."

# ============================================================================
# Help
# ============================================================================

help:
	@echo "DAM Build System"
	@echo ""
	@echo "Build Targets:"
	@echo "  make          Build default (Release, Ollama support)"
	@echo "  make minimal  Build minimal (no LLM features)"
	@echo "  make full     Build full (includes llama.cpp)"
	@echo "  make dev      Build debug with sanitizers"
	@echo ""
	@echo "Other Targets:"
	@echo "  make test     Run tests (builds first if needed)"
	@echo "  make install  Install to ~/.local (removes stale binaries)"
	@echo "  make purge    Remove stale binaries from system paths"
	@echo "  make clean    Remove all build directories"
	@echo "  make help     Show this help"
	@echo ""
	@echo "Environment Variables:"
	@echo "  DAM_OLLAMA_MODEL  Set default Ollama model"
	@echo ""
	@echo "Examples:"
	@echo "  make && make test           Build and test"
	@echo "  make full && make install   Build with llama.cpp and install"
