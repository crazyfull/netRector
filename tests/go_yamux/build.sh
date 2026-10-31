#!/usr/bin/env bash
# Builds the hashicorp/yamux interop helper with Docker: tests/go_yamux/goyamux (linux, static)
set -euo pipefail
cd "$(dirname "$0")"
docker run --rm -v "$PWD":/src -w /src golang:1.23 sh -c 'go mod init goyamux >/dev/null 2>&1 || true; go get github.com/hashicorp/yamux@v0.1.2 >/dev/null 2>&1; CGO_ENABLED=0 go build -o goyamux .'
echo "built: $PWD/goyamux"
