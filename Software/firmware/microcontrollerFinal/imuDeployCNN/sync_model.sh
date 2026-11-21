#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$script_dir/../../.." && pwd)
source_dir="$repo_root/software/models/swim_imu_esp32_deployment"

for file in \
  imu_cnn_model_data.cc \
  imu_cnn_model_data.h \
  imu_inference_config.h \
  deployment_manifest.json
do
  if [ ! -f "$source_dir/$file" ]; then
    printf 'Missing deployment artifact: %s\n' "$source_dir/$file" >&2
    exit 1
  fi
  cp "$source_dir/$file" "$script_dir/$file"
done

printf 'Synchronized model bundle from %s\n' "$source_dir"
