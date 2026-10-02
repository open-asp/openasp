#!/usr/bin/env bash
# Copyright (c) 2026 OpenASP.dev
# SPDX-License-Identifier: MIT


set -euo pipefail

exec cc "$@" $(pkg-config --static --libs libmdbsql) -liconv -lz -lm
