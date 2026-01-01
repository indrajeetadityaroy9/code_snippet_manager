# DAM - Developer Asset Manager

A command-line tool to store, tag, search, and retrieve code snippets. Built with a B+ tree storage engine for efficient indexing and persistent storage.

- **Snippet Storage**: Store code snippets with inline content, automatic language detection, and metadata
- **Tag-Based Organization**: Tag snippets for easy categorization and retrieval
- **Language Detection**: Automatic detection from shebang lines and file extensions
- **Persistent Storage**: B+ tree-backed storage with buffer pool and disk management
- **Fast Queries**: Efficient lookup by ID, name, tag, or language

## Quick Start

```bash
# Build
cmake -B build -S . && cmake --build build

# Install locally
cmake --install build --prefix ~/.local

# Add a snippet
dam add -n my-script -t bash -t utils <<< 'echo "Hello, World!"'

# List all snippets
dam list

# Get a snippet by name or ID
dam get my-script
dam get 1

# Search snippets
dam search "hello"
dam search -t bash        # Filter by tag
dam search -l python      # Filter by language

# Edit a snippet (opens in $EDITOR)
dam edit my-script

# Manage tags
dam tag                   # List all tags
dam tag my-script +new-tag -old-tag

# Remove a snippet
dam rm my-script          # Prompts for confirmation
dam rm -f my-script       # Skip confirmation
```

## CLI Commands

| Command | Description |
|---------|-------------|
| `dam add` | Add a new snippet (from file, stdin, editor, or `-i` interactive LLM mode) |
| `dam get` | Retrieve a snippet by ID or name |
| `dam list` | List all snippets |
| `dam edit` | Edit a snippet in your default editor |
| `dam search` | Search snippets by content, tag, or language |
| `dam tag` | List tags or modify snippet tags |
| `dam rm` | Remove a snippet |

Run `dam --help` or `dam <command> --help` for detailed options.

## Build from Source

```bash
# Quick build with Makefile wrapper
make              # Release build
make test         # Run tests
make install      # Install to ~/.local

# Or use CMake directly
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

