#!/usr/bin/env bash
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT


set -euo pipefail

prefix_flags=()
if [[ -n "${OPENASP_SOURCE_ROOT:-}" ]]; then
    prefix_flags+=("-ffile-prefix-map=${OPENASP_SOURCE_ROOT}=." "-fdebug-prefix-map=${OPENASP_SOURCE_ROOT}=.")
fi
if [[ -n "${EGRET_SOURCE_ROOT:-}" ]]; then
    prefix_flags+=("-ffile-prefix-map=${EGRET_SOURCE_ROOT}=egret" "-fdebug-prefix-map=${EGRET_SOURCE_ROOT}=egret")
fi

exec cc "${prefix_flags[@]}" "$@" $(pkg-config --static --libs libmdbsql) -liconv -lz -lm
